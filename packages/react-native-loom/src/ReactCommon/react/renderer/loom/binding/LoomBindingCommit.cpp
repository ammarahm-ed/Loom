/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */


// LoomBinding: worker jobs, and publishing and reconciling React commits.

#include "LoomBindingInternals.h"

#if LOOM_RENDERER
namespace facebook::react {

void LoomBinding::submitWorkerOperation(
    std::shared_ptr<SurfaceToken> token,
    LoomWorker::Task compute,
    std::function<void()> applied,
    bool synchronous,
    bool profileCommit,
    bool inlineEligible) {
  auto job = std::make_unique<LoomWorker::Job>();
  job->inlineEligible = inlineEligible;
  job->compute = std::move(compute);
  job->applied = std::move(applied);
  job->synchronous = synchronous;
  job->profileCommit = profileCommit;
  submitWorkerJob(std::move(token), std::move(job));
}

void LoomBinding::submitWorkerJob(
    std::shared_ptr<SurfaceToken> token,
    std::unique_ptr<LoomWorker::Job> job,
    bool* ranSynchronously) {
  if (ranSynchronously != nullptr) {
    *ranSynchronously = false;
  }
  job->active = [token] { return token->active.load(); };
  job->failed = [this, token](std::exception_ptr error) {
    if (!token->active.exchange(false))
      return;
    std::string reason =
        "Async Loom renderer computation or UI application failed";
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& exception) {
      reason += ": ";
      reason += exception.what();
    } catch (...) {
    }
    host_->failSurface(token->surfaceId, reason);
  };
  if (const auto scope = LoomPlatformTraits::current().withWorkerScope;
      scope != nullptr && job->compute) {
    auto compute = std::move(job->compute);
    job->compute = [scope, compute = std::move(compute)]() mutable {
      scope([&compute] { compute(); });
    };
  }
  // A forced-sync commit computes on the UI thread either way. Running it here
  // when the worker is idle skips the worker round trip; when the worker still
  // owns work the job is queued behind it, which is what preserves ordering.
  if (job->synchronous && worker_->runIfIdle(job)) {
    if (ranSynchronously != nullptr) {
      *ranSynchronously = true;
    }
    return;
  }
  // A small commit costs less than the worker hand-off, so it is computed right
  // here (UI thread in main-thread mode, JS thread otherwise) when no other
  // work is outstanding.
  if (worker_->tryRunInline(job)) {
    return;
  }
  worker_->submit(std::move(job));
}

void LoomBinding::publishWorkerCommit(
    SurfaceId surfaceId,
    const std::vector<std::shared_ptr<NodeRef>>& children,
    bool forceSync) {
#if RN_JANK_REPORTER
  // Loom (debug): lets the jank report say how much of a slow task
  // was spent committing to the native renderer.
  struct JankCommitTimer {
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    ~JankCommitTimer() {
      jank::Reporter::addCommit(
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - start)
              .count());
    }
  } jankCommitTimer;
#endif
  // First commit: React only renders once startup has finished. Resolving here
  // also seeds the text capability answer from the UI thread.
  (void)platformUIExecutor();
  auto packet = std::make_unique<WorkerCommit>();
  packet->surfaceId = surfaceId;
  packet->token = tokenFor(surfaceId);
  packet->api = apiInstance_;
  packet->animationFrame = AnimationFrameWork::active();
  // Draft data is built once by the host. Freeze each version; appendChild
  // thereafter creates a new version and copies only its changed child list.
  auto identities = std::make_shared<TagMap<uint64_t>>();
  // The versions this publish newly freezes, i.e. the changed part of the graph.
  // They are marked committed once the commit has been applied, which is what lets
  // a later reconcile stop at an unchanged subtree.
  auto committedDrafts =
      std::make_shared<std::vector<std::shared_ptr<DraftNode>>>();
  // One publication identity, stamped on every draft this walk reaches. A shared
  // version is then visited once by comparison, instead of inserting every node
  // into a visited set.
  const auto publishSequence = ++publishSequence_;
  // Custom text managers/effect factories/default font handlers remain on UI,
  // so the commit has to know whether it contains text and whether anything in
  // it depends on host state. Both are settled during this visit rather than in a
  // second walk over the graph, so the drafts are reached once, in tree order.
  bool hasText = false;
  bool unsafeTextProps = false;
  bool retainsLiveSnapshot = false;
  // Size of the changed part of the tree, the cost estimate for inlining.
  size_t changedTexts = 0;
  std::function<DraftNode::FreezeFacts(const std::shared_ptr<DraftNode>&)> freeze =
      [&](const auto& draft) -> DraftNode::FreezeFacts {
        if (!draft)
          return {};
        if (draft->frozenCommit == publishSequence)
          return draft->freezeFacts;
        draft->frozenCommit = publishSequence;
        if (draft->surfaceGeneration != packet->token->generation)
          throw std::logic_error("Stale worker draft");
        // An already-published version is unchanged. The host builds a new version
        // whenever props or children change, so this subtree's answers are still
        // the ones recorded when it was published, and reusing them is what keeps
        // an unchanged subtree from being walked on every commit. The generation
        // check above still runs, so a stale draft is caught either way.
        if (draft->published)
          return draft->freezeFacts;

        draft->published = true;
        (*identities)[draft->tag] = draft->identity;
        committedDrafts->push_back(draft);

        DraftNode::FreezeFacts facts;
        facts.retainsLiveSnapshot = !draft->childrenExplicit;
        facts.unsafeTextProps = draft->retainedUnsafeTextProps;
        if (draft->initialProps) {
          if (!draft->initialPropsTextScanned) {
            draft->initialPropsTextScanned = true;
            draft->initialPropsHasTextEffects =
                propsContainTextEffects(*draft->initialProps);
          }
          facts.unsafeTextProps |= draft->initialPropsHasTextEffects;
        }
        // Only the pending patches can still change what this commit measures.
        // Applied patches are folded into `retainedUnsafeTextProps` as their
        // history is dropped, so this stays bounded by the pending work instead
        // of growing with app uptime.
        for (auto patch = draft->propsTail; patch && !patch->applied;
             patch = patch->previous) {
          facts.unsafeTextProps |= patch->unsafeText;
        }
        facts.hasText = draft->roles.carriesText();
        if (facts.hasText)
          ++changedTexts;

        for (const auto& child : *draft->children) {
          const auto childFacts = freeze(child);
          facts.hasText |= childFacts.hasText;
          facts.unsafeTextProps |= childFacts.unsafeTextProps;
          facts.retainsLiveSnapshot |= childFacts.retainsLiveSnapshot;
        }
        draft->freezeFacts = facts;
        return facts;
      };
  for (const auto& ref : children) {
    if (!ref || ref->surfaceToken != packet->token)
      throw std::logic_error("Foreign worker root");
    const auto facts = freeze(ref->draft);
    hasText |= facts.hasText;
    unsafeTextProps |= facts.unsafeTextProps;
    retainsLiveSnapshot |= facts.retainsLiveSnapshot;
    packet->roots.push_back(ref->draft);
  }
  // Insert only the tags this walk newly published. `freeze` skips unchanged
  // subtrees, so the map it produces is a delta rather than the whole graph, and
  // erasing the surface first would drop the entry for every skipped subtree.
  // Reads validate the stored identity against the live draft (see
  // `setWorkerNativeProps`); `applyPublishedIdentities` erases an entry once the
  // version is mounted or removed.
  {
    std::lock_guard state(stateMutex_);
    for (const auto& [tag, identity] : *identities)
      submittedIdentities_[surfaceId][tag] = identity;
  }
  auto document = pendingDocumentInstanceHandles_.find(surfaceId);
  if (document != pendingDocumentInstanceHandles_.end()) {
    packet->documentHandle = std::move(document->second);
    pendingDocumentInstanceHandles_.erase(document);
  }
  // Asked only when the answer can change the decision: the query is a JNI
  // call plus a view-manager registry lookup, and it must not be cached
  // across commits because `TextEffectRegistry.hasRegisteredFactories()`
  // changes when a library registers or unregisters a factory.
  const bool textSafe =
      !hasText || canMeasureTextOnWorker();
  // The text-safety decision belongs to this commit only. Earlier revisions
  // kept it on the surface token, which made every later job on the surface
  // (setNativeProps, constraints) synchronous as well.
  const bool synchronousText =
      retainsLiveSnapshot || (hasText && (!textSafe || unsafeTextProps));
  forceSync = forceSync || synchronousText;
  auto& live = renderer(surfaceId);
  std::shared_ptr<LoomRenderer> owner;
  {
    std::lock_guard state(stateMutex_);
    owner = renderers_.at(surfaceId);
  }
  auto token = packet->token;
  // Filled by the compute step: the tags the renderer dropped, or nothing with
  // `fullIdentities` set when that commit walked the whole tree.
  auto removedTags = std::make_shared<std::vector<Tag>>();
  auto fullIdentities = std::make_shared<bool>(false);
  auto applied = [this, token, identities, committedDrafts, removedTags,
                  fullIdentities] {
    applyPublishedIdentities(
        token,
        *identities,
        *committedDrafts,
        *fullIdentities ? nullptr : removedTags.get());
  };
  // The worker pauses after all preceding UI results. A synchronous job borrows
  // the same renderer on UI for its ENTIRE reconciliation/style/layout/diff.
  // Reentrant submissions remain ordered after the enclosing UI result.
  (void)live;
  // Loom: cost estimate. Units are the changed nodes, with a text
  // counting extra because measuring and preparing it dominates a small update.
  // A commit also carries a fixed cost proportional to the whole tree (layout
  // pass, unsupported-attachment scan, identity collection), so the changed
  // size alone cannot predict its time: small commits additionally have to be
  // cheap in practice, judged by the recently observed time of such commits.
  const double units =
      1.0 + static_cast<double>(committedDrafts->size()) + 4.0 * changedTexts;
  const bool smallCommit = units <= kInlineMaxUnits;
  const bool inlineEligible = !forceSync && smallCommit &&
      smallCommitMs_.load(std::memory_order_relaxed) < kInlineBudgetMs;
  auto compute = LoomWorker::Task([this,
                                               owner,
                                               packet = std::move(packet),
                                               identities,
                                               publishIdentities = applied,
                                               removedTags,
                                               fullIdentities,
                                               forceSync,
                                               smallCommit] {
    const auto computeStart = std::chrono::steady_clock::now();
    const bool fullCommit = owner->nextCommitIsFull() ||
        packet->documentHandle != nullptr;
    owner->setAnimationFrameCommit(packet->animationFrame);
    reconcileWorkerCommit(*owner, *packet);
    owner->setAnimationFrameCommit(false);
    if (smallCommit) {
      // Keeps the budget test honest on this device right now: a cold CPU, or
      // a large tree whose fixed per-commit cost dominates, turns it off.
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - computeStart)
                            .count();
      const double previous = smallCommitMs_.load(std::memory_order_relaxed);
      smallCommitMs_.store(
          static_cast<float>(previous * 0.7 + ms * 0.3),
          std::memory_order_relaxed);
    }

    // An incremental commit reports a delta: the versions it published are
    // already in `identities`, and the renderer lists the tags it dropped.
    // Only a full commit re-collects the whole live set (and then reconciles
    // removals against it), which is what every commit used to do.
    *removedTags = owner->takeDroppedTags();
    if (!fullCommit) {
      if (forceSync)
        publishIdentities();
      return;
    }
    *fullIdentities = true;
    removedTags->clear();
    // Include children retained through compatibility/native DOM snapshots.
    // This metadata is result-owned and is never read concurrently by UI.
    identities->clear();
    std::function<void(Tag)> collect = [&](Tag tag) {
      if (identities->count(tag))
        return;
      (*identities)[tag] = owner->identityOf(tag);
      // Read-only recursion, so the borrowed list is safe and no vector is
      // copied per visited node.
      if (const auto* children = owner->childrenOfRef(tag); children != nullptr) {
        for (const auto child : *children)
          collect(child);
      }
    };
    collect(owner->rootTag());
    if (forceSync)
      publishIdentities();
  });
  submitWorkerOperation(
      token,
      std::move(compute),
      std::move(applied),
      forceSync,
      true,
      inlineEligible);
  // The queued synchronous job computes on the UI thread inside its own apply,
  // so a JS thread has nothing to gain by waiting for it: readers use the
  // barrier, and the UI thread never waits for JS.
  if (forceSync && worker_->onUIThread())
    worker_->synchronize();
}

void LoomBinding::applyPublishedIdentities(
    const std::shared_ptr<SurfaceToken>& token,
    const TagMap<uint64_t>& identities,
    const std::vector<std::shared_ptr<DraftNode>>& committedDrafts,
    const std::vector<Tag>* removedTags) {
  std::lock_guard state(stateMutex_);
  if (removedTags != nullptr) {
    // Delta: drop exactly the tags the renderer removed (when this surface
    // still owns them), then upsert the published versions below.
    for (const auto tag : *removedTags) {
      const auto mountedToken = mountedTokens_.find(tag);
      if (mountedToken != mountedTokens_.end() &&
          mountedToken->second->surfaceId == token->surfaceId) {
        eraseMountedLocked(tag);
      }
    }
  }
  if (hasFabricFamilies_.load(std::memory_order_relaxed)) {
    releaseFabricFamilies(token->surfaceId, removedTags, identities);
  }
  // Erase only what this commit no longer contains. This pass is what discovers
  // removals, and it stays correct because `identities` is the full live set
  // rather than the freeze walk's output.
  // Walks this surface's own mounted tags only (a full commit already walked its
  // whole tree to build `identities`), never every mounted tag of every surface.
  if (removedTags == nullptr) {
    if (auto mounted = mountedTagsBySurface_.find(token->surfaceId);
        mounted != mountedTagsBySurface_.end()) {
      std::vector<Tag> gone;
      for (const auto& entry : mounted->second) {
        if (identities.find(entry.first) == identities.end()) {
          gone.push_back(entry.first);
        }
      }
      for (const auto tag : gone) {
        eraseMountedLocked(tag);
      }
    }
  }
  // [LoomMemory] These versions are mounted now (or gone), so they no longer
  // count as "submitted": `setWorkerNativeProps` finds them through
  // `mountedIdentities_`. Entries were only ever dropped with the surface, i.e.
  // one per tag the surface ever published.
  if (auto surfaceSubmitted = submittedIdentities_.find(token->surfaceId);
      surfaceSubmitted != submittedIdentities_.end()) {
    auto& submitted = surfaceSubmitted->second;
    if (removedTags != nullptr) {
      for (const auto tag : *removedTags) {
        submitted.erase(tag);
      }
    }
    for (const auto& [tag, identity] : identities) {
      if (auto entry = submitted.find(tag);
          entry != submitted.end() && entry->second == identity) {
        submitted.erase(entry);
      }
    }
  }
  // Write only the entries that actually differ. Every commit used to erase and
  // re-insert an entry for every reachable tag, which copies a `shared_ptr` and
  // rewrites a hash entry per node even when nothing about it changed.
  // Deliberately idempotent: a synchronous commit publishes from the worker and
  // is then reported on UI, so this runs twice for one commit.
  for (const auto& [tag, identity] : identities) {
    const auto mounted = mountedIdentities_.find(tag);
    if (mounted != mountedIdentities_.end() && mounted->second == identity) {
      const auto mountedToken = mountedTokens_.find(tag);
      if (mountedToken != mountedTokens_.end() && mountedToken->second == token) {
        continue;
      }
    }
    setMountedLocked(tag, identity, token);
  }
  // The batch has been applied, so the live renderer now holds these versions and a
  // later reconcile may treat them as an untouched subtree. Marking has to happen
  // here rather than in `freeze`: publishes can be in flight, and a version frozen
  // into a packet the live tree has not reached yet must not look committed.
  for (const auto& draft : committedDrafts) {
    if (draft != nullptr) {
      draft->workerCommitted = true;
    }
  }
}

void LoomBinding::reconcileWorkerCommit(
    LoomRenderer& liveRenderer,
    const WorkerCommit& packet) {
  const auto surfaceId = packet.surfaceId;
  const auto& surfaceToken = packet.token;
  if (!surfaceToken->active)
    return;
  try {
    // Selection, draft validation and the inline-attachment check are shared
    // with the synchronous reconciler.
    std::vector<std::shared_ptr<DraftNode>> rootDrafts;
    rootDrafts.reserve(packet.roots.size());
    for (const auto& draft : packet.roots) {
      if (draft == nullptr || draft->surfaceId != surfaceId) {
        throw std::logic_error("completeRoot selected an invalid draft handle");
      }
      rootDrafts.push_back(draft);
    }
    auto commitSelection =
        selectCommitDrafts(liveRenderer, surfaceId, *surfaceToken, rootDrafts);
    auto& selected = commitSelection.selected;
    const auto rootTag = commitSelection.rootTag;
    const auto& rootChildren = commitSelection.rootChildren;
    if (commitSelection.unsupportedAttachmentTag != 0) {
      throw std::logic_error(
          "Direct worker commit contains an unsupported inline text attachment");
    }

    // Frozen drafts retain their complete graph and this path never marks them
    // `committedSnapshot` (it does not compact), so paint-only is detected
    // against worker-owned identities here. The synchronous path asks the
    // complementary question - `committedSnapshot && !hasInitialProps` - because
    // it *does* compact, so it cannot read a frozen graph back. The predicates
    // are deliberately different: each uses the bookkeeping its own mode
    // maintains, and substituting one for the other would disable paint-only in
    // that mode.
    bool paintOnly =
        !selected.empty() && liveRenderer.childrenOf(rootTag) == rootChildren;
    std::vector<std::pair<Tag, folly::dynamic>> paintPatches;
    for (const auto& entry : selected) {
      if (!paintOnly)
        break;
      const auto& draft = *entry.draft;
      const bool rawText = draft.roles.isRawText();
      if (liveRenderer.identityOf(draft.tag) != draft.identity ||
          (entry.childrenChanged &&
           childrenDifferFrom(liveRenderer, draft.tag, entry.childTags)) ||
          (!rawText &&
           !liveRenderer.hasSameInstanceHandle(
               draft.tag, draft.instanceHandle))) {
        paintOnly = false;
        break;
      }
      std::vector<std::shared_ptr<const PropsPatch>> patches;
      for (auto patch = draft.propsTail; patch && !patch->applied;
           patch = patch->previous) {
        if (patch->rawText || rawText) {
          paintOnly = false;
          break;
        }
        patches.push_back(patch);
      }
      for (auto it = patches.rbegin(); it != patches.rend(); ++it)
        paintPatches.emplace_back(draft.tag, (*it)->props);
    }
    if (!packet.documentHandle && paintOnly && !paintPatches.empty() &&
        liveRenderer.tryCommitPaintOnlyProps(paintPatches)) {
      for (const auto& entry : selected) {
        for (auto patch = entry.draft->propsTail; patch && !patch->applied;
             patch = patch->previous)
          patch->applied = true;
      }
      return;
    }

    // The worker path leaves the drafts to the host, which still owns them while
    // this result is in flight, so it does not compact them.
    reconcileSelectedDrafts(
        liveRenderer, surfaceId, commitSelection, false, *packet.api);
    if (packet.documentHandle)
      liveRenderer.setInstanceHandle(surfaceId, packet.documentHandle);
  } catch (const std::exception& error) {
    liveRenderer.abortCommit(error.what());
    // failSurface is a buffered UI effect. Do not throw through the runtime
    // result callback after the failed surface has already been invalidated.
  } catch (...) {
    liveRenderer.abortCommit("unknown exception in Loom renderer worker");
  }
}

LoomBinding::CommitSelection
LoomBinding::selectCommitDrafts(
    LoomRenderer& liveRenderer,
    SurfaceId surfaceId,
    const SurfaceToken& surfaceToken,
    const std::vector<std::shared_ptr<DraftNode>>& rootDrafts) {
  struct SelectionRecord {
    enum class Visit : uint8_t { Unseen, Active, Done };
    uint64_t identity{0};
    size_t selectedIndex{std::numeric_limits<size_t>::max()};
    size_t parentCount{0};
    Visit visit{Visit::Unseen};
    bool selected{false};
  };
  constexpr auto noSelectedIndex = std::numeric_limits<size_t>::max();

  CommitSelection selection;
  auto& selected = selection.selected;
  TagMap<SelectionRecord> selectionByTag;

  std::function<void(const std::shared_ptr<DraftNode>&)> selectDraft;
  std::function<void(Tag)> retainLiveSubtree;
  // A retained child is unchanged live structure: recording its root is
  // enough. Its subtree is not walked - that walk made every commit cost the
  // whole tree, and nothing below an unchanged node can be added, removed or
  // re-parented without React cloning the node (which makes it a draft).
  retainLiveSubtree = [&](Tag tag) {
    auto& record = selectionByTag[tag];
    if (record.visit == SelectionRecord::Visit::Active) {
      throw std::logic_error("Cycle in retained UI-thread renderer tree");
    }
    if (record.selected) {
      return;
    }
    record.identity = liveRenderer.identityOf(tag);
    record.selected = true;
    record.visit = SelectionRecord::Visit::Done;
  };
  selectDraft = [&](const std::shared_ptr<DraftNode>& draft) {
    if (draft == nullptr || draft->surfaceId != surfaceId ||
        draft->surfaceGeneration != surfaceToken.generation) {
      throw std::logic_error("Invalid or stale UI-thread renderer draft");
    }
    auto& record = selectionByTag[draft->tag];
    if (record.visit == SelectionRecord::Visit::Active) {
      throw std::logic_error("Cycle in UI-thread renderer draft tree");
    }
    if (record.selected) {
      if (record.identity != draft->identity) {
        throw std::logic_error(
            "Conflicting draft identities share a React tag");
      }
      throw std::logic_error(
          "A UI-thread renderer draft has multiple parents");
    }
    record.identity = draft->identity;
    record.selected = true;
    record.visit = SelectionRecord::Visit::Active;
    const auto liveIdentity = liveRenderer.identityOf(draft->tag);
    const bool sameLiveIdentity =
        liveIdentity != 0 && liveIdentity == draft->identity;
    // The compacting path clears a committed draft's children, so it can only read
    // them back from the live renderer. The worker path never compacts, so it marks
    // its versions committed once the batch has been applied, and their children
    // still agree with the live tree. A draft that retains live children is
    // deliberately excluded: that branch re-validates each retained child's identity,
    // so skipping it would drop the check rather than only the walk.
    const bool useLiveChildren = sameLiveIdentity &&
        ((draft->committedSnapshot && !draft->childrenExplicit) ||
         (draft->workerCommitted && draft->retainedLiveChildren == nullptr));
    record.selectedIndex = selected.size();
    selected.push_back({draft, {}, !useLiveChildren});

    if (useLiveChildren) {
      if (const auto* children = liveRenderer.childrenOfRef(draft->tag);
          children != nullptr) {
        for (const auto child : *children) {
          retainLiveSubtree(child);
        }
      }
      record.visit = SelectionRecord::Visit::Done;
      return;
    }
    if (draft->retainedLiveChildren != nullptr) {
      for (const auto& child : *draft->retainedLiveChildren) {
        auto& childRecord = selectionByTag[child.tag];
        if (++childRecord.parentCount > 1) {
          throw std::logic_error(
              "A UI-thread renderer draft has a child with multiple parents");
        }
        if (child.identity == 0 ||
            liveRenderer.identityOf(child.tag) != child.identity) {
          throw std::logic_error(
              "A retained UI-thread renderer child changed identity");
        }
        selected[record.selectedIndex].childTags.push_back(child.tag);
        retainLiveSubtree(child.tag);
      }
    }
    for (const auto& child : *draft->children) {
      if (child == nullptr) {
        continue;
      }
      auto& childRecord = selectionByTag[child->tag];
      if (++childRecord.parentCount > 1) {
        throw std::logic_error(
            "A UI-thread renderer draft has multiple parents");
      }
      // Recursive selection can grow `selected` and reallocate its storage,
      // so reacquire this parent by index rather than retaining a reference.
      selected[record.selectedIndex].childTags.push_back(child->tag);
      selectDraft(child);
    }
    record.visit = SelectionRecord::Visit::Done;
  };

  selection.rootChildren.reserve(rootDrafts.size());
  for (const auto& draft : rootDrafts) {
    selection.rootChildren.push_back(draft->tag);
    if (++selectionByTag[draft->tag].parentCount > 1) {
      throw std::logic_error("A selected root draft appears more than once");
    }
    selectDraft(draft);
  }
  selection.rootTag =
      liveRenderer.rootTag() == 0 ? surfaceId : liveRenderer.rootTag();
  auto& rootRecord = selectionByTag[selection.rootTag];
  rootRecord.identity = liveRenderer.identityOf(selection.rootTag);
  rootRecord.selected = true;
  rootRecord.visit = SelectionRecord::Visit::Done;

  // Reject inline view attachments while the selected graph is still only a
  // draft. `prepare()` used to discover these after nodes, Yoga, event
  // targets, and child lists had already been changed in the live renderer.
  selection.selectedIdentities.reserve(selectionByTag.size());
  for (const auto& [tag, record] : selectionByTag) {
    if (!record.selected) {
      continue;
    }
    selection.selectedIdentities.emplace(tag, record.identity);
  }
  // Removal roots and the inline-attachment check, both limited to the parents
  // whose child lists changed. Only such a parent can drop a child or gain an
  // unsupported one: an unchanged parent keeps its (already validated)
  // children.
  const auto rolesOf = [&](Tag tag) -> LoomComponentTable::Roles {
    const auto record = selectionByTag.find(tag);
    if (record != selectionByTag.end() &&
        record->second.selectedIndex != noSelectedIndex) {
      const auto& draft = selected[record->second.selectedIndex].draft;
      if (draft->componentName != nullptr) {
        return draft->roles;
      }
    }
    return liveRenderer.componentRoles(tag);
  };
  const auto noteRemovedChildren = [&](Tag parentTag,
                                       const std::vector<Tag>& next) {
    const auto* live = liveRenderer.childrenOfRef(parentTag);
    if (live == nullptr || live->empty()) {
      return;
    }
    if (*live == next) {
      return;
    }
    std::unordered_set<Tag> keep(next.begin(), next.end());
    for (const auto child : *live) {
      if (keep.count(child) == 0) {
        selection.removalRoots.push_back(child);
      }
    }
  };
  noteRemovedChildren(selection.rootTag, selection.rootChildren);
  for (const auto& entry : selected) {
    const auto& draft = *entry.draft;
    const auto liveIdentity = liveRenderer.identityOf(draft.tag);
    if (liveIdentity != 0 && liveIdentity != draft.identity) {
      // The old version of a replaced node leaves the tree as a whole.
      selection.removalRoots.push_back(draft.tag);
    } else if (entry.childrenChanged) {
      noteRemovedChildren(draft.tag, entry.childTags);
    }
    if (!entry.childrenChanged || selection.unsupportedAttachmentTag != 0) {
      continue;
    }
    const auto parentRoles = rolesOf(draft.tag);
    if (!parentRoles.isText() && !parentRoles.textInput) {
      continue;
    }
    for (const auto child : entry.childTags) {
      if (!rolesOf(child).isTextStack()) {
        selection.unsupportedAttachmentTag = child;
        break;
      }
    }
  }
  return selection;
}

void LoomBinding::reconcileSelectedDrafts(
    LoomRenderer& liveRenderer,
    SurfaceId surfaceId,
    const CommitSelection& selection,
    bool compact,
    loom::internal::Instance& api) {
  static const auto emptyDraftChildren =
      std::make_shared<std::vector<std::shared_ptr<DraftNode>>>();
  const auto& selected = selection.selected;

  // Preserve cleanup edges owned by a parent that the selected graph removes.
  liveRenderer.captureRemovedMountedEdgesFrom(
      selection.selectedIdentities, selection.removalRoots);

  for (const auto& entry : selected) {
    const auto& draft = *entry.draft;
    const auto currentIdentity = liveRenderer.identityOf(draft.tag);
    const bool sameLiveIdentity =
        currentIdentity != 0 && currentIdentity == draft.identity;
    if (!sameLiveIdentity) {
      if (!draft.hasInitialProps || draft.componentName == nullptr ||
          (draft.initialProps == nullptr && draft.initialRawText == nullptr)) {
        throw std::logic_error("Stale UI-thread renderer draft identity");
      }
      if (draft.initialRawText != nullptr) {
        liveRenderer.createRawTextNode(draft.tag, *draft.initialRawText);
      } else {
        liveRenderer.createNode(
            draft.tag,
            *draft.componentName,
            *draft.initialProps,
            draft.instanceHandle);
      }
      liveRenderer.setIdentity(draft.tag, draft.identity);
    } else {
      liveRenderer.setInstanceHandle(draft.tag, draft.instanceHandle);
    }

    std::vector<std::shared_ptr<const PropsPatch>> patches;
    for (auto patch = draft.propsTail; patch != nullptr;
         patch = patch->previous) {
      // Props patches form an append-only chronological chain (only `applied`
      // is mutable). Publication always applies the unapplied suffix in order,
      // so an applied patch marks the boundary before the committed history.
      // Stop here rather than rescanning the node's full history on later
      // commits.
      if (patch->applied) {
        break;
      }
      patches.push_back(patch);
    }
    for (auto patch = patches.rbegin(); patch != patches.rend(); ++patch) {
      if ((*patch)->rawText.has_value()) {
        liveRenderer.updateRawText(draft.tag, *(*patch)->rawText);
      } else {
        liveRenderer.cloneNodeWithNewProps(draft.tag, (*patch)->props);
      }
    }
    if (draft.compatProps != nullptr) {
      liveRenderer.setCompatProps(draft.tag, draft.compatProps);
    }
  }

  for (const auto& entry : selected) {
    if (!entry.childrenChanged) {
      continue;
    }
    if (childrenDifferFrom(liveRenderer, entry.draft->tag, entry.childTags)) {
      liveRenderer.setChildren(entry.draft->tag, entry.childTags);
    }
  }

  // Mark the selected handles as committed before native callbacks can re-enter
  // completeRoot. Props patches are shared with older refs, so an explicitly
  // applied patch is never replayed over a later setNativeProps.
  for (const auto& entry : selected) {
    auto& draft = *entry.draft;
    for (auto patch = draft.propsTail; patch != nullptr;
         patch = patch->previous) {
      if (patch->applied) {
        break;
      }
      patch->applied = true;
    }
    if (!compact) {
      continue;
    }
    // The commit is applied and the live node is the source of truth, so the
    // applied history is dropped rather than retained per prop update. A
    // reentrant commit that appends a patch while the root completes starts a
    // fresh chain from this assignment.
    draft.propsTail = nullptr;
    draft.committedSnapshot = true;
    draft.hasInitialProps = false;
    draft.initialProps.reset();
    draft.initialPropsTextScanned = false;
    draft.initialPropsHasTextEffects = false;
    draft.initialRawText.reset();
    draft.componentName.reset();
    draft.childrenExplicit = false;
    draft.children = emptyDraftChildren;
    draft.retainedLiveChildren.reset();
  }

  liveRenderer.completeRoot(surfaceId, selection.rootChildren);
  if (loom::internal::wantsLayoutCommitted(api)) {
    std::vector<Tag> changed;
    changed.reserve(selected.size());
    for (const auto& entry : selected) {
      changed.push_back(entry.draft->tag);
    }
    loom::internal::notifyLayoutCommitted(
        api, surfaceId, std::move(changed));
  }
}

void LoomBinding::publishRootCommit(
    SurfaceId surfaceId,
    const std::vector<std::shared_ptr<NodeRef>>& children,
    const std::shared_ptr<SurfaceToken>& surfaceToken,
    uint64_t profileGeneration) {
  static const auto emptyDraftChildren =
      std::make_shared<std::vector<std::shared_ptr<DraftNode>>>();
  if (surfaceToken == nullptr || !surfaceToken->active ||
      surfaceToken->surfaceId != surfaceId) {
    return;
  }
  if (publishingRootCommits_.count(surfaceId) != 0) {
    pendingRootCommits_[surfaceId].push_back(
        {surfaceToken, children, profileGeneration});
    return;
  }
  auto& currentRenderer = renderer(surfaceId);
  if (currentRenderer.isCommitting()) {
    pendingRootCommits_[surfaceId].push_back(
        {surfaceToken, children, profileGeneration});
    return;
  }
  const bool profileSyncCommit = LoomWorkerProfile::enabled();
  const auto syncCommitStart = profileSyncCommit
      ? LoomWorkerProfile::Clock::now()
      : LoomWorkerProfile::Clock::time_point{};
  publishingRootCommits_.insert(surfaceId);
  // releaseSurfaceInternal removes the map entry immediately, but this strong
  // local reference keeps the active commit's renderer alive until callbacks
  // have unwound. A restarted surface gets a new renderer and new token.
  auto liveRendererOwner = renderers_.at(surfaceId);
  auto& liveRenderer = *liveRendererOwner;
  try {
#if LOOM_PROFILE
    const auto reconcileStart = std::chrono::steady_clock::now();
#endif
    // Same selection and validation as the worker reconciler; this path runs it
    // inline on the UI thread instead of on the worker.
    std::vector<std::shared_ptr<DraftNode>> rootDrafts;
    rootDrafts.reserve(children.size());
    for (const auto& childRef : children) {
      if (childRef == nullptr || childRef->draft == nullptr ||
          childRef->surfaceToken != surfaceToken ||
          childRef->surfaceId != surfaceId) {
        throw std::logic_error("completeRoot selected an invalid draft handle");
      }
      rootDrafts.push_back(childRef->draft);
    }
    auto commitSelection =
        selectCommitDrafts(liveRenderer, surfaceId, *surfaceToken, rootDrafts);
    auto& selected = commitSelection.selected;
    const auto rootTag = commitSelection.rootTag;
    const auto& rootChildren = commitSelection.rootChildren;
    if (commitSelection.unsupportedAttachmentTag != 0) {
      // A host view inside Text is not supported. Stop the surface exactly like
      // the worker path does; returning here alone would report the commit as
      // applied and freeze the surface on its previous contents, and every later
      // commit carrying the attachment would be dropped the same way.
      surfaceToken->active = false;
      host_->reportUnsupportedInlineAttachment(
          surfaceId, commitSelection.unsupportedAttachmentTag);
      host_->failSurface(
          surfaceId,
          "Loom renderer commit contains an unsupported inline text attachment");
      publishingRootCommits_.erase(surfaceId);
      drainPendingRootCommits(surfaceId);
      return;
    }

    // A deliberately small direct path handles commits that change only known
    // paint props. Require an unchanged, already-mounted identity/child graph;
    // unknown props, new nodes, changed handles, pending NativeDOM root setup,
    // and every structural/text/layout change fall through to full
    // reconciliation.
    //
    // `committedSnapshot && !hasInitialProps` is this path's way of asking "has
    // this draft already been committed once", which the worker path instead
    // answers from worker-owned identities because it never compacts. See the
    // note on the worker's paint-only block: the predicates are not
    // interchangeable.
    bool paintOnlyCandidate =
        pendingDocumentInstanceHandles_.count(surfaceId) == 0 &&
        !selected.empty() && liveRenderer.childrenOf(rootTag) == rootChildren;
    std::vector<std::pair<Tag, folly::dynamic>> paintOnlyPatches;
    if (paintOnlyCandidate) {
      for (const auto& entry : selected) {
        const auto& draft = *entry.draft;
        if (!draft.committedSnapshot || draft.hasInitialProps ||
            (entry.childrenChanged &&
             childrenDifferFrom(liveRenderer, draft.tag, entry.childTags)) ||
            liveRenderer.identityOf(draft.tag) != draft.identity ||
            !liveRenderer.hasSameInstanceHandle(
                draft.tag, draft.instanceHandle)) {
          paintOnlyCandidate = false;
          break;
        }
        std::vector<std::shared_ptr<const PropsPatch>> newPatches;
        for (auto patch = draft.propsTail; patch != nullptr;
             patch = patch->previous) {
          if (patch->applied) {
            break;
          }
          if (patch->rawText.has_value()) {
            paintOnlyCandidate = false;
            break;
          }
          newPatches.push_back(patch);
        }
        if (!paintOnlyCandidate)
          break;
        for (auto patch = newPatches.rbegin(); patch != newPatches.rend();
             ++patch) {
          paintOnlyPatches.emplace_back(draft.tag, (*patch)->props);
        }
      }
    }
    if (paintOnlyCandidate && !paintOnlyPatches.empty() &&
        liveRenderer.tryCommitPaintOnlyProps(paintOnlyPatches)) {
      for (auto& entry : selected) {
        auto& draft = *entry.draft;
        for (auto patch = draft.propsTail; patch != nullptr;
             patch = patch->previous) {
          if (patch->applied) {
            break;
          }
          patch->applied = true;
        }
        // Everything above is applied, and the live node is now the source of
        // truth for it, so drop the history instead of retaining one patch per
        // prop update. Patches appended by a reentrant commit stay reachable
        // through the new head this assignment gives them.
        draft.propsTail = nullptr;
        draft.committedSnapshot = true;
        draft.hasInitialProps = false;
        draft.initialProps.reset();
    draft.initialPropsTextScanned = false;
    draft.initialPropsHasTextEffects = false;
        draft.initialRawText.reset();
        draft.componentName.reset();
        draft.childrenExplicit = false;
        draft.children = emptyDraftChildren;
        draft.retainedLiveChildren.reset();
      }
      publishingRootCommits_.erase(surfaceId);
      drainPendingRootCommits(surfaceId);
      return;
    }

    // This path owns the renderer outright, so it compacts the drafts it
    // commits: the live node is now the source of truth for every applied patch.
    reconcileSelectedDrafts(
        liveRenderer, surfaceId, commitSelection, true, *apiInstance_);
    // This path keeps no mounted-identity delta; drop the record.
    (void)liveRenderer.takeDroppedTags();
#if LOOM_PROFILE
    // Reconciliation and the root commit are one atomic step now, so this marks
    // the end of both.
    const auto reconcileEnd = std::chrono::steady_clock::now();
#endif
#if LOOM_PROFILE
    const auto profileAfterCommit = std::chrono::steady_clock::now();
    if (LoomWorkerProfile::enabled()) {
      const auto profileMs = [](auto from, auto to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
      };
      loomLog(
          LoomLogLevel::Profile,
          "LoomPublishProfile",
          "surface=%d gen=%llu reconciliation=%.3fms rendererCommit=%.3fms",
          static_cast<int>(surfaceId),
          static_cast<unsigned long long>(profileGeneration),
          profileMs(reconcileStart, reconcileEnd),
          profileMs(reconcileEnd, profileAfterCommit));
    }
#endif
    if (surfaceToken->active) {
      auto documentHandle = pendingDocumentInstanceHandles_.find(surfaceId);
      if (documentHandle != pendingDocumentInstanceHandles_.end()) {
        liveRenderer.setInstanceHandle(
            liveRenderer.rootTag(), std::move(documentHandle->second));
        pendingDocumentInstanceHandles_.erase(documentHandle);
      }
    }
    publishingRootCommits_.erase(surfaceId);
    drainPendingRootCommits(surfaceId);
    if (profileSyncCommit) {
      loomLog(
          LoomLogLevel::Profile,
          "LoomDirectSyncProfile",
          "generation=%llu main=%d work=%.3f",
          static_cast<unsigned long long>(profileGeneration),
          LoomPlatformTraits::current().onUIThread() ? 1 : 0,
          LoomWorkerProfile::ms(
              syncCommitStart, LoomWorkerProfile::Clock::now()));
    }
  } catch (const std::exception& error) {
    liveRenderer.abortCommit(error.what());
    publishingRootCommits_.erase(surfaceId);
    pendingRootCommits_.erase(surfaceId);
    drainingRootCommits_.erase(surfaceId);
    throw;
  } catch (...) {
    liveRenderer.abortCommit(
        "unknown exception while reconciling a root commit");
    publishingRootCommits_.erase(surfaceId);
    pendingRootCommits_.erase(surfaceId);
    drainingRootCommits_.erase(surfaceId);
    throw;
  }
}

void LoomBinding::drainPendingRootCommits(SurfaceId surfaceId) {
  if (publishingRootCommits_.count(surfaceId) != 0) {
    return;
  }
  auto queueIterator = pendingRootCommits_.find(surfaceId);
  if (queueIterator == pendingRootCommits_.end() ||
      !drainingRootCommits_.insert(surfaceId).second) {
    return;
  }

  while (true) {
    queueIterator = pendingRootCommits_.find(surfaceId);
    if (queueIterator == pendingRootCommits_.end() ||
        queueIterator->second.empty()) {
      break;
    }
    auto pending = std::move(queueIterator->second.front());
    queueIterator->second.pop_front();
    if (pending.surfaceToken == nullptr || !pending.surfaceToken->active) {
      continue;
    }
    publishRootCommit(
        surfaceId,
        pending.children,
        pending.surfaceToken,
        pending.profileGeneration);
  }

  pendingRootCommits_.erase(surfaceId);
  drainingRootCommits_.erase(surfaceId);
}

void LoomBinding::flushPendingPatches(SurfaceId surfaceId) {
  auto pending = pendingPatches_.find(surfaceId);
  if (pending == pendingPatches_.end()) {
    return;
  }
  auto patches = std::make_shared<std::vector<PendingPatch>>(
      std::move(pending->second));
  pendingPatches_.erase(pending);
  if (patches->empty()) {
    return;
  }
  std::shared_ptr<LoomRenderer> owner;
  std::shared_ptr<SurfaceToken> token;
  {
    std::lock_guard state(stateMutex_);
    const auto renderer = renderers_.find(surfaceId);
    const auto surfaceToken = surfaceTokens_.find(surfaceId);
    if (renderer == renderers_.end() || surfaceToken == surfaceTokens_.end() ||
        !surfaceToken->second->active) {
      return;
    }
    owner = renderer->second;
    token = surfaceToken->second;
  }
  auto job = std::make_unique<LoomWorker::Job>();
  // Queued like a commit, so it applies after every commit React made before
  // it, and its view operations reach the UI thread the same way.
  job->compute = [owner, patches] {
    std::vector<std::pair<Tag, folly::dynamic>> props;
    bool text = false;
    for (auto& patch : *patches) {
      // A node that is gone, or whose tag now names another node, is skipped.
      if (owner->identityOf(patch.tag) != patch.identity) {
        continue;
      }
      if (patch.rawText.has_value()) {
        owner->updateRawText(patch.tag, std::move(*patch.rawText));
        text = true;
      } else {
        props.emplace_back(patch.tag, std::move(patch.props));
      }
    }
    if (!text && !props.empty() && owner->tryCommitPaintOnlyProps(props)) {
      return;
    }
    for (const auto& [tag, patch] : props) {
      owner->cloneNodeWithNewProps(tag, patch);
    }
    if (text || !props.empty()) {
      owner->reflow(); // runs a commit over what was just marked
    }
  };
  job->applied = [] {};
  submitWorkerJob(std::move(token), std::move(job));
}

} // namespace facebook::react
#endif // LOOM_RENDERER
