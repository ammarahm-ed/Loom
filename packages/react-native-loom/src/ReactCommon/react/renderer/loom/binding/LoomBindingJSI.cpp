/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */


// LoomBinding: the JavaScript host object (`nativeFabricUIManager`).

#include "LoomBindingInternals.h"

#if LOOM_RENDERER
namespace facebook::react {

#pragma mark - JS API

jsi::Value LoomBinding::get(
    jsi::Runtime& runtime,
    const jsi::PropNameID& name) {
#if LOOM_PROFILE
  // The JS-side cached Fabric host functions use this opt-in marker to measure
  // complete JS→JSI-call elapsed time. It is set only in diagnostic builds.
  runtime.global().setProperty(
      runtime, "__LOOM_PROFILE", true);
#endif
  const auto method = name.utf8(runtime);

  // Read once at module load by the Lists override: JavaScript shares the main
  // thread with the UI, so long synchronous render passes block frames.
  if (method == "loomJsOnMain") {
    return jsi::Value(LoomWorker::kJavaScriptOnUIThread);
  }

  // `Loom` (react-native-loom): the runtime settings registry. The wrapper reads
  // it once and keeps the object.
  if (method == "loomSettings") {
    constexpr bool kWorker = true;
    return jsi::Object::createFromHostObject(
        runtime,
        std::make_shared<LoomSettingsHostObject>(
            LoomWorker::kJavaScriptOnUIThread, kWorker));
  }

  // Read once at module load by the React renderer override: lets it use the
  // host operations only this renderer guarantees (updating a raw text node in
  // place, for one).
  if (method == "loomRenderer") {
    return jsi::Value(true);
  }

  // React Native's Pressability and other legacy UIManager call sites still
  // measure by React tag through FabricUIManager.measure(). In Loom renderer
  // mode, findShadowNodeByTag_DEPRECATED returns a NodeRef, but falling through
  // to UIManagerBinding::get would measure the backing shadow-tree snapshot.
  // That snapshot does not include the live native scroll offset, so responder
  // regions drift out of alignment as soon as a ScrollView moves.
  if (method == "measure" || method == "measureInWindow" ||
      method == "measureLayout") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        method == "measureLayout" ? 4 : 2,
        [this, method](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          return getDirectDOM(rt, method, arguments, count);
        });
  }

  if (method == "unstable_forceNextCommitSync") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        0,
        [this](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
          forceNextCommitSync_ = true;
          return jsi::Value::undefined();
        });
  }
  if (method == "configureNextLayoutAnimation") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        3,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* args,
            size_t count) {
          // Direct mounts do not feed LayoutAnimationKeyFrameManager. Never
          // accept an animation configuration whose callbacks cannot finish.
          forceNextCommitSync_ = true;
          if (count > 2 && args[2].isObject() &&
              args[2].asObject(rt).isFunction(rt)) {
            args[2].asObject(rt).asFunction(rt).call(rt);
          }
          return jsi::Value::undefined();
        });
  }

  if (method == "createNode") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        5,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "createNode", 5, count);
          auto surface = surfaceIdFromValue(rt, arguments[2]);
          LOOM_HOST_CALL_SCOPE(
              profile, *this, surface, ProfiledHostMethod::CreateNode);
          auto tag = tagFromValue(arguments[0]);
          auto token = tokenFor(surface);
          auto draft = std::make_shared<DraftNode>();
          draft->surfaceId = surface;
          draft->surfaceGeneration = token->generation;
          draft->identity = ++nextDraftIdentity_;
          draft->tag = tag;
          const auto componentName = stringFromValue(rt, arguments[1]);
          // One shared name per component type instead of an allocation per
          // node; React only creates a handful of distinct host types.
          auto& interned = internedComponentNames_[componentName];
          if (interned == nullptr) {
            interned = std::make_shared<const std::string>(componentName);
          }
          draft->componentName = interned;
          classifyDraft(*draft);
          draft->compatDescriptor = compatDescriptorFor(interned);
          const auto rawText = draft->roles.isRawText()
              ? rawTextFromValue(rt, arguments[3])
              : std::nullopt;
          if (rawText.has_value()) {
            draft->initialRawText =
                std::make_shared<const std::string>(*rawText);
          } else {
            // Unknown raw-text props retain the general conversion path so
            // future host props are not silently discarded.
            draft->initialProps = std::make_shared<const folly::dynamic>(
                propsFromValue(rt, arguments[3]));
          }
          draft->hasInitialProps = true;
          if (draft->compatDescriptor != nullptr) {
            parseCompatProps(rt, *draft, arguments[3]);
          }
          draft->instanceHandle =
              instanceHandleFromValue(rt, arguments[4], arguments[0]);
          return jsi::Object::createFromHostObject(
              rt,
              std::make_shared<NodeRef>(
                  surface, tag, std::move(token), std::move(draft)));
        });
  }

  if (method == "cloneNode") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        1,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "cloneNode", 1, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref == nullptr) {
            return jsi::Value::null();
          }
          LOOM_HOST_CALL_SCOPE(
              profile, *this, ref->surfaceId, ProfiledHostMethod::CloneNode);
          auto draft = cloneDraft(ref);
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(*ref, std::move(draft)));
        });
  }

  // React's `cloneHiddenInstance`: the same version, hidden by an offscreen
  // boundary. Not a props patch, so committing the source version again
  // restores it without undoing anything.
  if (method == "cloneNodeHidden") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        1,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "cloneNodeHidden", 1, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref == nullptr) {
            return jsi::Value::null();
          }
          LOOM_HOST_CALL_SCOPE(
              profile, *this, ref->surfaceId, ProfiledHostMethod::CloneNode);
          auto draft = cloneDraft(ref);
          if (draft != nullptr) {
            draft->offscreenHidden = true;
          }
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(*ref, std::move(draft)));
        });
  }

  if (method == "cloneNodeWithNewProps") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "cloneNodeWithNewProps", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref == nullptr) {
            return jsi::Value::null();
          }
          LOOM_HOST_CALL_SCOPE(
              profile,
              *this,
              ref->surfaceId,
              ProfiledHostMethod::CloneNodeWithNewProps);
          auto draft = cloneDraft(ref);
          auto patch = std::make_shared<PropsPatch>();
          if (isRawTextRef(*ref)) {
            patch->rawText = rawTextFromValue(rt, arguments[1]);
          }
          if (!patch->rawText.has_value()) {
            patch->props = propsFromValue(rt, arguments[1]);
            patch->unsafeText = propsContainTextEffects(patch->props);
            if (draft->compatDescriptor != nullptr) {
              parseCompatProps(rt, *draft, arguments[1]);
            }
          }
          linkPendingPatches(*draft, *patch);
          draft->propsTail = std::move(patch);
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(*ref, std::move(draft)));
        });
  }

  if (method == "cloneNodeWithNewChildren") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        1,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "cloneNodeWithNewChildren", 1, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref == nullptr) {
            return jsi::Value::null();
          }
          LOOM_HOST_CALL_SCOPE(
              profile,
              *this,
              ref->surfaceId,
              ProfiledHostMethod::CloneNodeWithNewChildren);
          auto draft = cloneDraft(ref, true);
          if (count > 1 && arguments[1].isObject()) {
            auto set = arguments[1].asObject(rt).getHostObject<ChildSet>(rt);
            if (set != nullptr) {
              for (const auto& child : set->nodes) {
                if (child != nullptr && child->draft != nullptr &&
                    child->surfaceToken == ref->surfaceToken) {
                  draft->children->push_back(child->draft);
                }
              }
            }
          }
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(*ref, std::move(draft)));
        });
  }

  if (method == "cloneNodeWithNewChildrenAndProps") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "cloneNodeWithNewChildrenAndProps", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref == nullptr) {
            return jsi::Value::null();
          }
          LOOM_HOST_CALL_SCOPE(
              profile,
              *this,
              ref->surfaceId,
              ProfiledHostMethod::CloneNodeWithNewChildrenAndProps);
          // The host API allows both arities:
          //   cloneNodeWithNewChildrenAndProps(node, props)
          //   cloneNodeWithNewChildrenAndProps(node, children, props)
          // Without a children argument the children are dropped, exactly as
          // the shadow-tree path does.
          const bool hasChildren = count == 3;
          auto draft = cloneDraft(ref, true);
          if (hasChildren) {
            auto set = arguments[1].asObject(rt).getHostObject<ChildSet>(rt);
            if (set != nullptr) {
              for (const auto& child : set->nodes) {
                if (child != nullptr && child->draft != nullptr &&
                    child->surfaceToken == ref->surfaceToken) {
                  draft->children->push_back(child->draft);
                }
              }
            }
          }
          auto patch = std::make_shared<PropsPatch>();
          const auto& propsValue = arguments[hasChildren ? 2 : 1];
          if (isRawTextRef(*ref)) {
            patch->rawText = rawTextFromValue(rt, propsValue);
          }
          if (!patch->rawText.has_value()) {
            patch->props = propsFromValue(rt, propsValue);
            patch->unsafeText = propsContainTextEffects(patch->props);
            if (draft->compatDescriptor != nullptr) {
              parseCompatProps(rt, *draft, propsValue);
            }
          }
          linkPendingPatches(*draft, *patch);
          draft->propsTail = std::move(patch);
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(*ref, std::move(draft)));
        });
  }

  // Loom: clone a parent whose host children are the same nodes in the same
  // order, swapping in the new versions of only the children React cloned.
  // React calls this instead of cloneNodeWithNewChildren(AndProps) plus one
  // appendChild per child. Returns null when the source version does not hold
  // an explicit child list, or a changed child is not one of its children, and
  // React then takes the regular path.
  //   cloneNodeReplacingChildren(node, changedChildren, propsOrNull)
  if (method == "cloneNodeReplacingChildren") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        3,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "cloneNodeReplacingChildren", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref == nullptr || ref->draft == nullptr ||
              !ref->draft->childrenExplicit ||
              ref->draft->retainedLiveChildren != nullptr) {
            return jsi::Value::null();
          }
          LOOM_HOST_CALL_SCOPE(
              profile,
              *this,
              ref->surfaceId,
              ProfiledHostMethod::CloneNodeWithNewChildren);
          // The changed children arrive as null (none), a single node (the
          // common case, no array to build or read through JSI), or an array.
          std::vector<std::shared_ptr<DraftNode>> changed;
          const auto acceptChild = [&](const jsi::Value& value) {
            auto child = nodeRef(rt, value);
            if (child == nullptr || child->draft == nullptr ||
                child->surfaceToken != ref->surfaceToken) {
              return false;
            }
            changed.push_back(child->draft);
            return true;
          };
          if (arguments[1].isObject()) {
            auto object = arguments[1].asObject(rt);
            if (object.isHostObject(rt)) {
              if (!acceptChild(arguments[1])) {
                return jsi::Value::null();
              }
            } else if (object.isArray(rt)) {
              auto changedArray = object.getArray(rt);
              const auto changedCount = changedArray.size(rt);
              changed.reserve(changedCount);
              for (size_t index = 0; index < changedCount; ++index) {
                if (!acceptChild(changedArray.getValueAtIndex(rt, index))) {
                  return jsi::Value::null();
                }
              }
            } else {
              return jsi::Value::null();
            }
          } else if (!arguments[1].isNull() && !arguments[1].isUndefined()) {
            return jsi::Value::null();
          }
          const auto& sourceChildren = *ref->draft->children;
          std::shared_ptr<std::vector<std::shared_ptr<DraftNode>>> children;
          if (!changed.empty()) {
            children =
                std::make_shared<std::vector<std::shared_ptr<DraftNode>>>(
                    sourceChildren);
            size_t replaced = 0;
            if (changed.size() <= 8) {
              for (auto& slot : *children) {
                if (slot == nullptr) {
                  continue;
                }
                for (const auto& next : changed) {
                  if (slot->tag == next->tag) {
                    slot = next;
                    ++replaced;
                    break;
                  }
                }
              }
            } else {
              TagMap<size_t> changedIndex;
              changedIndex.reserve(changed.size());
              for (size_t index = 0; index < changed.size(); ++index) {
                changedIndex.emplace(changed[index]->tag, index);
              }
              for (auto& slot : *children) {
                if (slot == nullptr) {
                  continue;
                }
                const auto found = changedIndex.find(slot->tag);
                if (found != changedIndex.end()) {
                  slot = changed[found->second];
                  ++replaced;
                }
              }
            }
            if (replaced != changed.size()) {
              return jsi::Value::null();
            }
          }
          auto draft = cloneDraft(ref);
          if (draft == nullptr) {
            return jsi::Value::null();
          }
          if (children != nullptr) {
            draft->children = std::move(children);
          }
          if (count > 2 && arguments[2].isObject()) {
            auto patch = std::make_shared<PropsPatch>();
            patch->props = propsFromValue(rt, arguments[2]);
            patch->unsafeText = propsContainTextEffects(patch->props);
            if (draft->compatDescriptor != nullptr) {
              parseCompatProps(rt, *draft, arguments[2]);
            }
            linkPendingPatches(*draft, *patch);
            draft->propsTail = std::move(patch);
          }
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(*ref, std::move(draft)));
        });
  }

  // In-place updates (see `pendingPatches_`).
  //   loomPatchNode(node, payload): payload is a props diff, or a string for
  //   a raw text node. Returns true when recorded.
  if (method == "loomPatchNode") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "loomPatchNode", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref == nullptr || ref->draft == nullptr) {
            return jsi::Value(false);
          }
          PendingPatch patch;
          patch.tag = ref->tag;
          patch.identity = ref->draft->identity;
          if (arguments[1].isString()) {
            patch.rawText = arguments[1].asString(rt).utf8(rt);
          } else if (arguments[1].isObject()) {
            patch.props = propsFromValue(rt, arguments[1]);
          } else {
            return jsi::Value(false);
          }
          pendingPatches_[ref->surfaceId].push_back(std::move(patch));
          return jsi::Value(true);
        });
  }
  //   loomFlushPatches(surfaceId)
  if (method == "loomFlushPatches") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        1,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "loomFlushPatches", 1, count);
          flushPendingPatches(surfaceIdFromValue(rt, arguments[0]));
          return jsi::Value::undefined();
        });
  }

  if (method == "createChildSet") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        0,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t)
            -> jsi::Value {
          auto set = std::make_shared<ChildSet>();
#if LOOM_PROFILE
          const auto start = std::chrono::steady_clock::now();
          set->createdAt = start;
          set->createdAtEpochNanoseconds =
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
          set->createCallNanoseconds = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count());
#endif
          return jsi::Object::createFromHostObject(rt, std::move(set));
        });
  }

  if (method == "appendChildToSet") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "appendChildToSet", 2, count);
          auto set = arguments[0].asObject(rt).getHostObject<ChildSet>(rt);
          auto child = nodeRef(rt, arguments[1]);
          if (set != nullptr && child != nullptr && child->draft != nullptr) {
#if LOOM_PROFILE
            auto generation = profileCommitGeneration_[child->surfaceId];
            if (generation == 0) {
              generation = 1;
              profileCommitGeneration_[child->surfaceId] = generation;
            }
            auto& hostProfile = profileHostCalls_[child->surfaceId][generation];
            if (hostProfile.firstHostCallEpochNanoseconds == 0) {
              hostProfile.firstHostCallEpochNanoseconds =
                  set->createdAtEpochNanoseconds;
              hostProfile.firstHostCallSteadyNanoseconds =
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      set->createdAt.time_since_epoch())
                      .count();
            }
            LOOM_HOST_CALL_SCOPE(
                profile,
                *this,
                child->surfaceId,
                ProfiledHostMethod::AppendChildToSet);
#endif
            if (set->surfaceToken == nullptr) {
              set->surfaceToken = child->surfaceToken;
              set->surfaceId = child->surfaceId;
            }
            if (set->surfaceToken == child->surfaceToken) {
              set->nodes.push_back(std::move(child));
            }
          }
          return jsi::Value::undefined();
        });
  }

  if (method == "appendChild") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "appendChild", 2, count);
          auto parent = nodeRef(rt, arguments[0]);
          auto child = nodeRef(rt, arguments[1]);
          if (parent != nullptr && child != nullptr &&
              parent->draft != nullptr && child->draft != nullptr &&
              parent->surfaceToken == child->surfaceToken) {
            LOOM_HOST_CALL_SCOPE(
                profile,
                *this,
                parent->surfaceId,
                ProfiledHostMethod::AppendChild);
            if (parent->draft->published)
              parent->draft = cloneDraft(parent);
            auto& draft = *parent->draft;
            if (!draft.childrenExplicit) {
              worker_->synchronize();
              std::vector<RetainedLiveChild> currentChildren;
              auto owner = renderers_.find(parent->surfaceId);
              if (owner != renderers_.end() &&
                  owner->second->identityOf(parent->tag) == draft.identity) {
                for (const auto childTag :
                     owner->second->childrenOf(parent->tag)) {
                  currentChildren.push_back(
                      {childTag, owner->second->identityOf(childTag)});
                }
              }
              draft.children =
                  std::make_shared<std::vector<std::shared_ptr<DraftNode>>>();
              draft.retainedLiveChildren =
                  std::make_shared<const std::vector<RetainedLiveChild>>(
                      std::move(currentChildren));
              draft.childrenExplicit = true;
            } else if (draft.children.use_count() != 1) {
              draft.children =
                  std::make_shared<std::vector<std::shared_ptr<DraftNode>>>(
                      *draft.children);
            }
            draft.children->push_back(child->draft);
          }
          return jsi::Value::undefined();
        });
  }

  if (method == "completeRoot") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "completeRoot", 2, count);
          auto surface = surfaceIdFromValue(rt, arguments[0]);
          [[maybe_unused]] uint64_t generation = 0;
#if LOOM_PROFILE
          generation = profileCommitGeneration_[surface];
          if (generation == 0) {
            generation = 1;
          }
          auto& currentHostProfiles = profileHostCalls_[surface];
          HostProfile hostProfile;
          if (auto existing = currentHostProfiles.find(generation);
              existing != currentHostProfiles.end()) {
            hostProfile = std::move(existing->second);
            currentHostProfiles.erase(existing);
          }
          if (hostProfile.firstHostCallEpochNanoseconds == 0) {
            hostProfile.firstHostCallEpochNanoseconds =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
          }
          profileCommitGeneration_[surface] = generation + 1;
          const auto completeRootStart = std::chrono::steady_clock::now();
          const auto completeRootStartSteadyNanoseconds =
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  completeRootStart.time_since_epoch())
                  .count();
          const auto hostCallsToCompleteRootMs =
              hostProfile.firstHostCallSteadyNanoseconds == 0
              ? 0.0
              : std::max(
                    0.0,
                    (completeRootStartSteadyNanoseconds -
                     hostProfile.firstHostCallSteadyNanoseconds) /
                        1.0e6);
#endif
          auto set = arguments[1].asObject(rt).getHostObject<ChildSet>(rt);
          auto token = tokenFor(surface);
          if (set != nullptr && set->surfaceToken != nullptr &&
              set->surfaceToken != token) {
            throw jsi::JSError(
                rt, "completeRoot received a stale or foreign child set");
          }
          const auto selectedRoots = set == nullptr
              ? std::vector<std::shared_ptr<NodeRef>>{}
              : set->nodes;
#if LOOM_PROFILE
          if (set != nullptr) {
            const auto createChildSetIndex =
                static_cast<size_t>(ProfiledHostMethod::CreateChildSet);
            ++hostProfile.methods[createChildSetIndex].calls;
            hostProfile.methods[createChildSetIndex].totalNanoseconds +=
                set->createCallNanoseconds;
          }
          const auto completeRootProfileIndex =
              static_cast<size_t>(ProfiledHostMethod::CompleteRoot);
          ++hostProfile.methods[completeRootProfileIndex].calls;
#endif
          // In-place patches recorded by this commit go first, so they are
          // never queued behind (or merged into) the structural commit.
          flushPendingPatches(surface);
          const bool forceSync = forceNextCommitSync_ ||
              (count > 2 && arguments[2].isBool() && arguments[2].getBool());
          forceNextCommitSync_ = false;
          publishWorkerCommit(surface, selectedRoots, forceSync);
#if LOOM_PROFILE
          hostProfile.methods[completeRootProfileIndex].totalNanoseconds +=
              static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - completeRootStart)
                      .count());
          if (LoomWorkerProfile::enabled()) {
            const auto method =
                [&hostProfile](
                    ProfiledHostMethod value) -> const HostMethodStats& {
              return hostProfile.methods[static_cast<size_t>(value)];
            };
            const auto methodMs = [](const HostMethodStats& stats) {
              return static_cast<double>(stats.totalNanoseconds) / 1.0e6;
            };
            loomLog(
                LoomLogLevel::Profile,
                "LoomHostProfile",
                "surface=%d gen=%llu firstHostToCompleteRootMs=%.3f "
                "createNode=%llu/%.3f clone=%llu/%.3f cloneProps=%llu/%.3f "
                "cloneChildren=%llu/%.3f cloneChildrenProps=%llu/%.3f "
                "createSet=%llu/%.3f appendSet=%llu/%.3f appendChild=%llu/%.3f "
                "completeRoot=%llu/%.3f",
                static_cast<int>(surface),
                static_cast<unsigned long long>(generation),
                hostCallsToCompleteRootMs,
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CreateNode).calls),
                methodMs(method(ProfiledHostMethod::CreateNode)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CloneNode).calls),
                methodMs(method(ProfiledHostMethod::CloneNode)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CloneNodeWithNewProps).calls),
                methodMs(method(ProfiledHostMethod::CloneNodeWithNewProps)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CloneNodeWithNewChildren).calls),
                methodMs(method(ProfiledHostMethod::CloneNodeWithNewChildren)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CloneNodeWithNewChildrenAndProps)
                        .calls),
                methodMs(
                    method(ProfiledHostMethod::CloneNodeWithNewChildrenAndProps)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CreateChildSet).calls),
                methodMs(method(ProfiledHostMethod::CreateChildSet)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::AppendChildToSet).calls),
                methodMs(method(ProfiledHostMethod::AppendChildToSet)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::AppendChild).calls),
                methodMs(method(ProfiledHostMethod::AppendChild)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CompleteRoot).calls),
                methodMs(method(ProfiledHostMethod::CompleteRoot)));
          }
#endif
          return jsi::Value::undefined();
        });
  }

  if (method == "findShadowNodeByTag_DEPRECATED") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        1,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          worker_->synchronize();
          requireArguments(rt, "findShadowNodeByTag_DEPRECATED", 1, count);
          auto tag = tagFromValue(arguments[0]);
          auto* owner = rendererOwning(tag);
          if (owner == nullptr) {
            return jsi::Value::null();
          }
          auto token = tokenFor(owner->surfaceId());
          auto draft = std::make_shared<DraftNode>();
          draft->surfaceId = owner->surfaceId();
          draft->surfaceGeneration = token->generation;
          draft->identity = owner->identityOf(tag);
          draft->tag = tag;
          draft->componentName =
              std::make_shared<const std::string>(owner->componentName(tag));
          classifyDraft(*draft);
          draft->instanceHandle = owner->instanceHandle(tag);
          draft->committedSnapshot = true;
          draft->childrenExplicit = false;
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(owner->surfaceId(), tag, std::move(draft)));
        });
  }

  if (method == "setNativeProps") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "setNativeProps", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref != nullptr) {
            setWorkerNativeProps(*ref, propsFromValue(rt, arguments[1]));
          }
          return jsi::Value::undefined();
        });
  }

  if (method == "dispatchCommand") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        3,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
requireArguments(rt, "dispatchCommand", 3, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref != nullptr) {
            // Ordered after the commits the worker already owns instead of
            // blocking this thread on them: the guard reads the renderer's node
            // registry, which the worker mutates while it computes.
            auto commandName = stringFromValue(rt, arguments[1]);
            auto commandArgs = commandArgsFromValue(rt, arguments[2]);
            submitOrderedUIEffect(
                ref->surfaceId,
                [this,
                 surfaceId = ref->surfaceId,
                 ref,
                 commandName = std::move(commandName),
                 commandArgs = std::move(commandArgs)] {
                  auto owner = findRenderer(surfaceId);
                  if (owner != nullptr && isLiveNodeRef(*ref)) {
                    owner->dispatchCommand(
                        ref->tag, commandName, commandArgs);
                  }
                });
          }
          return jsi::Value::undefined();
        });
  }

  if (method == "sendAccessibilityEvent") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
requireArguments(rt, "sendAccessibilityEvent", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref != nullptr) {
            auto eventType = stringFromValue(rt, arguments[1]);
            submitOrderedUIEffect(
                ref->surfaceId,
                [this,
                 surfaceId = ref->surfaceId,
                 ref,
                 eventType = std::move(eventType)] {
                  auto owner = findRenderer(surfaceId);
                  if (owner != nullptr && isLiveNodeRef(*ref)) {
                    owner->sendAccessibilityEvent(ref->tag, eventType);
                  }
                });
          }
          return jsi::Value::undefined();
        });
  }

  if (method == "setIsJSResponder") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        3,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
requireArguments(rt, "setIsJSResponder", 3, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref != nullptr) {
            // Called on every touch grant, so blocking here stalled the
            // responder handoff behind whatever the worker was computing.
            const auto isJSResponder = arguments[1].getBool();
            const auto blockNativeResponder = arguments[2].getBool();
            submitOrderedUIEffect(
                ref->surfaceId,
                [this,
                 surfaceId = ref->surfaceId,
                 ref,
                 isJSResponder,
                 blockNativeResponder] {
                  auto owner = findRenderer(surfaceId);
                  if (owner != nullptr && isLiveNodeRef(*ref)) {
                    owner->setIsJSResponder(
                        ref->tag, isJSResponder, blockNativeResponder);
                  }
                });
          }
          return jsi::Value::undefined();
        });
  }

  if (method == "getRelativeLayoutMetrics") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          worker_->synchronize();
          requireArguments(rt, "getRelativeLayoutMetrics", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          auto ancestorRef = nodeRef(rt, arguments[1]);

          LoomRenderer::Metrics metrics;
          auto* owner = ref == nullptr ? nullptr : rendererOwning(ref->tag);
          const auto ancestorTag =
              ancestorRef == nullptr ? 0 : ancestorRef->tag;
          if (owner == nullptr ||
              !owner->metricsRelativeTo(ref->tag, ancestorTag, metrics)) {
            metrics = LoomRenderer::Metrics{};
          }

          auto result = jsi::Object(rt);
          result.setProperty(rt, "left", metrics.x);
          result.setProperty(rt, "top", metrics.y);
          result.setProperty(rt, "width", metrics.width);
          result.setProperty(rt, "height", metrics.height);
          return result;
        });
  }

  if (method == "getBoundingClientRect") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          worker_->synchronize();
          requireArguments(rt, "getBoundingClientRect", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          const bool includeTransform = arguments[1].getBool();

          LoomRenderer::Metrics metrics;
          if (ref == nullptr ||
              !windowMetrics(ref->tag, metrics, includeTransform)) {
            return jsi::Value::undefined();
          }

          return jsi::Array::createWithElements(
              rt,
              jsi::Value{rt, static_cast<double>(metrics.x)},
              jsi::Value{rt, static_cast<double>(metrics.y)},
              jsi::Value{rt, static_cast<double>(metrics.width)},
              jsi::Value{rt, static_cast<double>(metrics.height)});
        });
  }

  if (method == "measureInstance") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        1,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          worker_->synchronize();
          requireArguments(rt, "measureInstance", 1, count);
          auto ref = nodeRef(rt, arguments[0]);

          LoomRenderer::Metrics metrics;
          if (ref != nullptr) {
            windowMetrics(ref->tag, metrics);
          }

          auto result = jsi::Object(rt);
          result.setProperty(rt, "x", metrics.x);
          result.setProperty(rt, "y", metrics.y);
          result.setProperty(rt, "width", metrics.width);
          result.setProperty(rt, "height", metrics.height);
          return result;
        });
  }

  if (method == "compareDocumentPosition") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          worker_->synchronize();
          requireArguments(rt, "compareDocumentPosition", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          auto otherRef = nodeRef(rt, arguments[1]);

          auto* owner = ref == nullptr ? nullptr : rendererOwning(ref->tag);
          if (owner == nullptr || otherRef == nullptr ||
              !owner->exists(otherRef->tag)) {
            return jsi::Value{
                rt, static_cast<double>(kDocumentPositionDisconnected)};
          }
          return jsi::Value{
              rt,
              static_cast<double>(
                  owner->compareDocumentPosition(ref->tag, otherRef->tag))};
        });
  }

  if (method == "findNodeAtPoint") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        4,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          worker_->synchronize();
          requireArguments(rt, "findNodeAtPoint", 4, count);
          auto ref = nodeRef(rt, arguments[0]);
          auto callback = arguments[3].getObject(rt).getFunction(rt);

          auto* owner = ref == nullptr ? nullptr : rendererOwning(ref->tag);
          if (owner == nullptr) {
            callback.call(rt, jsi::Value::null());
            return jsi::Value::undefined();
          }

          const auto hit = owner->findNodeAtPoint(
              ref->tag,
              static_cast<float>(arguments[1].getNumber()),
              static_cast<float>(arguments[2].getNumber()));
          auto handle = hit == 0 ? nullptr : owner->instanceHandle(hit);
          if (handle == nullptr) {
            callback.call(rt, jsi::Value::null());
            return jsi::Value::undefined();
          }

          // The handle is stored weak, so it has to be retained to be handed
          // back to JavaScript - the same dance the shadow-tree binding does.
          EventTarget target(handle, ref->surfaceId);
          target.setEnabled(true);
          target.retain(rt);
          auto instanceHandle = target.getInstanceHandle(rt);
          target.release(rt);

          callback.call(rt, std::move(instanceHandle));
          return jsi::Value::undefined();
        });
  }

  if (method == "applyViewTransitionName" ||
      method == "cancelViewTransitionName" ||
      method == "restoreViewTransitionName") {
    // View transitions are driven from shadow nodes, which do not exist in
    // this mode. Answering with a no-op keeps a node-taking method from
    // throwing; there is nothing meaningful to apply.
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        1,
        [](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t)
            -> jsi::Value { return jsi::Value::undefined(); });
  }

  // Nothing that takes a node is left: event handler registration, layout
  // animation configuration and the event priority constants are all
  // node-free.
  return UIManagerBinding::get(runtime, name);
}

} // namespace facebook::react
#endif // LOOM_RENDERER
