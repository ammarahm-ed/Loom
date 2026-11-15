/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */


// LoomBinding: node references, drafts, and the Fabric compat descriptors and props.

#include "LoomBindingInternals.h"

#if LOOM_RENDERER
namespace facebook::react {

std::shared_ptr<LoomBinding::NodeRef> LoomBinding::makeNodeRef(
    const NodeRef& source,
    std::shared_ptr<DraftNode> draft) {
  if (source.surfaceToken != nullptr && source.surfaceToken->active) {
    return std::make_shared<NodeRef>(
        source.surfaceId, source.tag, source.surfaceToken, std::move(draft));
  }
  return makeNodeRef(source.surfaceId, source.tag, std::move(draft));
}

std::shared_ptr<LoomBinding::NodeRef>
LoomBinding::makeNodeRef(
    SurfaceId surfaceId,
    Tag tag,
    std::shared_ptr<DraftNode> draft) {
  return std::make_shared<NodeRef>(
      surfaceId, tag, tokenFor(surfaceId), std::move(draft));
}

std::shared_ptr<LoomBinding::SurfaceToken>
LoomBinding::tokenFor(SurfaceId surfaceId) {
  std::lock_guard state(stateMutex_);
  auto iterator = surfaceTokens_.find(surfaceId);
  if (iterator != surfaceTokens_.end() && iterator->second->active) {
    return iterator->second;
  }
  auto& generation = surfaceGenerations_[surfaceId];
  if (generation == 0) {
    generation = 1;
  }
  auto token = std::make_shared<SurfaceToken>();
  token->surfaceId = surfaceId;
  token->generation = generation;
  surfaceTokens_[surfaceId] = token;
  return token;
}

const ComponentDescriptor* LoomBinding::compatDescriptorFor(
    const std::shared_ptr<const std::string>& componentName) {
  auto found = compatDescriptorByName_.find(componentName.get());
  if (found != compatDescriptorByName_.end()) {
    return found->second;
  }
  std::shared_ptr<LoomCompat> compat;
  {
    std::lock_guard state(stateMutex_);
    compat = compat_;
  }
  if (compat == nullptr) {
    // No renderer yet: decide on a later createNode.
    return nullptr;
  }
  const auto* descriptor = compat->descriptorFor(*componentName);
  compatDescriptorByName_.emplace(componentName.get(), descriptor);
  return descriptor;
}

void LoomBinding::parseCompatProps(
    jsi::Runtime& rt,
    DraftNode& draft,
    const jsi::Value& value) {
  std::shared_ptr<LoomCompat> compat;
  {
    std::lock_guard state(stateMutex_);
    compat = compat_;
  }
  if (compat == nullptr || !value.isObject()) {
    return;
  }
  try {
    PropsParserContext context{draft.surfaceId, compat->contextContainer()};
    draft.compatProps = draft.compatDescriptor->cloneProps(
        context, draft.compatProps, RawProps(rt, value));
  } catch (const std::exception& error) {
    // The renderer falls back to parsing the merged folly props.
    LOG(ERROR) << "Loom compat: cannot parse props for "
               << (draft.componentName ? *draft.componentName : "?") << ": "
               << error.what();
    draft.compatProps.reset();
  }
}

std::shared_ptr<LoomBinding::DraftNode>
LoomBinding::cloneDraft(
    const std::shared_ptr<NodeRef>& source,
    bool replaceChildren) {
  if (source == nullptr || source->draft == nullptr ||
      source->surfaceToken == nullptr || !source->surfaceToken->active) {
    return nullptr;
  }
  auto draft = std::make_shared<DraftNode>(*source->draft);
  // A clone is a new, unpublished version. The publication bookkeeping belongs
  // to the source version only: inheriting `published`/`workerCommitted` would
  // make `freeze` and selection treat the clone as an unchanged, already-live
  // subtree, so its new props and children would never reach the renderer.
  draft->published = false;
  draft->workerCommitted = false;
  draft->frozenCommit = 0;
  draft->freezeFacts = {};
  if (replaceChildren) {
    draft->children =
        std::make_shared<std::vector<std::shared_ptr<DraftNode>>>();
    draft->retainedLiveChildren.reset();
    draft->childrenExplicit = true;
  }
  return draft;
}

bool LoomBinding::propsContainTextEffects(const folly::dynamic& value) {
  if (value.isObject()) {
    for (const auto& item : value.items()) {
      if (item.first.isString() && item.first.getString() == "textEffects" &&
          !item.second.isNull())
        return true;
      if (propsContainTextEffects(item.second))
        return true;
    }
  } else if (value.isArray()) {
    for (const auto& item : value) {
      if (propsContainTextEffects(item))
        return true;
    }
  }
  return false;
}

void LoomBinding::linkPendingPatches(
    DraftNode& draft,
    PropsPatch& patch) {
  auto boundary = draft.propsTail;
  while (boundary && boundary->applied.load(std::memory_order_relaxed)) {
    draft.retainedUnsafeTextProps |= boundary->unsafeText;
    boundary = boundary->previous;
  }
  patch.previous = std::move(boundary);
}

bool LoomBinding::isRawTextRef(const NodeRef& ref) const {
  if (ref.draft != nullptr && ref.draft->componentName != nullptr) {
    return ref.draft->roles.isRawText();
  }
  const auto renderer = findRenderer(ref.surfaceId);
  return renderer != nullptr && renderer->componentRoles(ref.tag).isRawText();
}

LoomRenderer* LoomBinding::rendererOwning(Tag tag) {
  // Tags are unique across surfaces, so the first renderer that knows the tag
  // is the one that owns it.
  std::lock_guard state(stateMutex_);
  for (auto& entry : renderers_) {
    if (entry.second->exists(tag)) {
      return entry.second.get();
    }
  }
  return nullptr;
}

#pragma mark - Nodes

std::shared_ptr<LoomBinding::NodeRef>
LoomBinding::nodeRef(jsi::Runtime& runtime, const jsi::Value& value)
    const {
  if (!value.isObject()) {
    return nullptr;
  }
  auto ref = value.asObject(runtime).getHostObject<NodeRef>(runtime);
  if (ref == nullptr || ref->surfaceToken == nullptr ||
      !ref->surfaceToken->active ||
      ref->surfaceToken->surfaceId != ref->surfaceId ||
      (ref->tag != 0 &&
       (ref->draft == nullptr || ref->draft->identity == 0 ||
        ref->draft->surfaceGeneration != ref->surfaceToken->generation))) {
    return nullptr;
  }
  return ref;
}

bool LoomBinding::isLiveNodeRef(const NodeRef& ref) const {
  const auto renderer = findRenderer(ref.surfaceId);
  if (renderer == nullptr) {
    return false;
  }
  const auto tag = ref.tag == 0 ? renderer->rootTag() : ref.tag;
  if (tag == 0 || !renderer->exists(tag)) {
    return false;
  }
  if (tag == renderer->rootTag() && (ref.tag == 0 || ref.draft == nullptr)) {
    return true;
  }
  return ref.draft != nullptr && renderer->identityOf(tag) == ref.draft->identity;
}

std::vector<Tag> LoomBinding::tagsOf(
    const std::shared_ptr<ChildSet>& set) {
  std::vector<Tag> tags;
  if (set == nullptr) {
    return tags;
  }
  tags.reserve(set->nodes.size());
  for (const auto& node : set->nodes) {
    if (node != nullptr) {
      tags.push_back(node->tag);
    }
  }
  return tags;
}

} // namespace facebook::react
#endif // LOOM_RENDERER
