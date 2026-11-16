/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.react.fabric.mounting

import android.os.SystemClock
import android.util.Log
import androidx.annotation.UiThread
import com.facebook.common.logging.FLog
import com.facebook.react.BuildConfig
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.bridge.ReadableArray
import com.facebook.react.bridge.UiThreadUtil
import com.facebook.react.bridge.ReadableNativeMap
import com.facebook.react.bridge.WritableMap
import com.facebook.react.common.mapbuffer.ReadableMapBuffer
import com.facebook.react.uimanager.StateWrapper
import com.facebook.react.uimanager.ReferenceStateWrapper
import com.facebook.react.uimanager.PixelUtil
import com.facebook.react.views.text.PreparedLayout
import java.util.LinkedHashMap
import java.util.concurrent.ConcurrentHashMap
import java.util.function.BiConsumer
import kotlin.math.round

/**
 * Direct UI-thread view applier for the standalone LOOM_RENDERER.
 *
 * It applies the phased operation batch produced by `LoomRenderer` against
 * [SurfaceMountingManager], so the same view manager, view state and view
 * recycling semantics apply as on the mount-item path, without going through
 * `MountItem` translation or the mount queue.
 *
 * The batch is flat: phase offsets delimit create/props, removals, insertions,
 * padding, layout, and delete instructions. Operation codes and integer operands live in
 * `instructions`, while the non-numeric operands (component names and prop
 * maps) are passed out of band in `objects`, in the order the operations
 * reference them. The operation layout is documented in
 * `LoomRenderer::DirectOperation` and mirrored in
 * `FabricMountingManager::directApplyBatch`.
 */
@UiThread
internal class LoomViewApplier(
    private val mountingManager: MountingManager,
    private val onFailure: BiConsumer<Int, String> = BiConsumer { _, _ -> },
) {

  internal class BatchProfile {
    var totalNanos = 0L
    var propsCalls = 0
    var stateCalls = 0
    var layoutCalls = 0
    var otherCalls = 0
    var propsPreprocessNanos = 0L
    var propsManagerNanos = 0L
    var propsTotalNanos = 0L
    var stateNanos = 0L
    var layoutNanos = 0L
    var otherNanos = 0L
  }

  /** Matches FabricMountingManager::writeUpdateLayoutMountItem's round(scale()). */
  private fun dipToPixel(value: Float): Int = round(PixelUtil.toPixelFromDIP(value)).toInt()

  private val directTags = HashMap<Int, MutableSet<Int>>()
  /** Direct ownership is a surface property; tags below track Java view state. */
  private val directSurfaces = HashSet<Int>()
  private val pendingBatches = HashMap<Int, ArrayDeque<PendingBatch>>()
  private val pendingImperative = HashMap<Int, ArrayDeque<PendingImperative>>()
  // Generations are read once per instruction to detect a surface that was
  // invalidated mid-batch, so the map is concurrent: writers still hold
  // `ownershipLock` for their read-modify-write and for the companion removals,
  // while the hot validity check needs no lock.
  private val surfaceGeneration = ConcurrentHashMap<Int, Long>()
  private val drainScheduled = HashSet<Int>()
  /** Prevents a ViewManager callback from interleaving a nested mount batch. */
  private val dispatchingSurfaces = HashSet<Int>()
  /** Cross-thread event ingress may inspect ownership while UI commits mutate it. */
  private val ownershipLock = Any()



  private data class PendingBatch(
      val instructions: IntArray,
      val objects: Array<Any?>,
      val stateMapBuffer: ReadableMapBuffer?,
      val stateTags: IntArray,
      val stateWrappers: Array<Any?>,
      val preparedTextTags: IntArray,
      val preparedTextLayouts: Array<Any?>,
      val phaseOffsets: IntArray?,
      val requiresTagReplacement: Boolean,
      val propsTable: LoomPropsTable?,
  )

  private data class RemoveInstruction(
      val offset: Int,
      val parentTag: Int,
      val reactTag: Int,
      val position: Int,
  )

  private class DirectMapBufferStateWrapper(
      override val stateDataMapBuffer: ReadableMapBuffer,
  ) : StateWrapper {
    override val stateData: ReadableNativeMap? = null

    override fun updateState(map: WritableMap) = Unit

    override fun destroyState() = Unit
  }

  private class DirectPreparedTextStateWrapper(
      override val stateDataReference: Any,
  ) : ReferenceStateWrapper {
    override val stateDataMapBuffer: ReadableMapBuffer? = null
    override val stateData: ReadableNativeMap? = null
    override fun updateState(map: WritableMap) = Unit
    override fun destroyState() = Unit
  }

  private sealed class PendingImperative(open val reactTag: Int) {
    data class Command(
        override val reactTag: Int,
        val commandName: String,
        val args: ReadableArray,
    ) : PendingImperative(reactTag)

    data class Accessibility(
        override val reactTag: Int,
        val eventType: String,
    ) : PendingImperative(reactTag)

    data class Responder(
        override val reactTag: Int,
        val isJSResponder: Boolean,
        val blockNativeResponder: Boolean,
    ) : PendingImperative(reactTag)
  }

  private fun surface(surfaceId: Int): SurfaceMountingManager {
    check(UiThreadUtil.isOnUiThread()) { "Direct Fabric view operations must run on the UI thread" }
    return mountingManager.getSurfaceManagerEnforced(surfaceId, "LoomViewApplier")
  }

  /**
   * True for tags this renderer created. Event dispatch for a direct surface is
   * routed to the JSI event manager instead of the Fabric event emitter, and
   * this is how that boundary is recognized.
   */
  fun isDirectTag(surfaceId: Int, reactTag: Int): Boolean = synchronized(ownershipLock) {
    val result = directTags[surfaceId]?.contains(reactTag) == true
    result
  }

  fun isDirectSurface(surfaceId: Int): Boolean = synchronized(ownershipLock) {
    directSurfaces.contains(surfaceId)
  }

  fun getScrollOffsetPacked(surfaceId: Int, reactTag: Int): Long {
    check(UiThreadUtil.isOnUiThread()) { "Direct Fabric measurement must run on the UI thread" }
    return surface(surfaceId).getLiveScrollOffset(reactTag)
  }

  fun getViewLocationInSurfacePacked(surfaceId: Int, reactTag: Int): Long {
    check(UiThreadUtil.isOnUiThread()) { "Direct Fabric measurement must run on the UI thread" }
    return surface(surfaceId).getLiveViewLocationInSurfacePacked(reactTag)
  }

  /** Resolves legacy event calls that do not carry a surface id. */
  fun surfaceIdForDirectTag(reactTag: Int): Int? = synchronized(ownershipLock) {
    directTags.entries.firstOrNull { it.value.contains(reactTag) }?.key
  }

  fun clearSurface(surfaceId: Int) {
    synchronized(ownershipLock) {
      surfaceGeneration[surfaceId] = (surfaceGeneration[surfaceId] ?: 0L) + 1L
      directSurfaces.remove(surfaceId)
      directTags.remove(surfaceId)
      pendingBatches.remove(surfaceId)
      pendingImperative.remove(surfaceId)
      drainScheduled.remove(surfaceId)
      dispatchingSurfaces.remove(surfaceId)
    }
  }

  fun applyBatch(
      surfaceId: Int,
      instructions: IntArray,
      objects: Array<Any?>,
      stateMapBuffer: ReadableMapBuffer? = null,
      stateTags: IntArray = intArrayOf(),
      stateWrappers: Array<Any?> = emptyArray(),
      preparedTextTags: IntArray = intArrayOf(),
      preparedTextLayouts: Array<Any?> = emptyArray(),
      phaseOffsets: IntArray? = null,
      requiresTagReplacement: Boolean = false,
      propsTable: LoomPropsTable? = null,
  ) {
    val manager = surface(surfaceId)
    var deferred = false
    var shouldScheduleDrain = false
    var applyImmediately = false
    var scheduleGeneration = 0L
    synchronized(ownershipLock) {
      directSurfaces.add(surfaceId)
      if (!manager.isRootViewAttached || dispatchingSurfaces.contains(surfaceId)) {
        pendingBatches.getOrPut(surfaceId) { ArrayDeque() }
            .addLast(
                PendingBatch(
                    // The native encoder allocates these arrays per JNI call and
                    // never touches them afterwards, so holding the references is
                    // enough; copying them per deferred batch is wasted work.
                    instructions,
                    objects,
                    stateMapBuffer,
                    stateTags,
                    stateWrappers,
                    preparedTextTags,
                    preparedTextLayouts,
                    phaseOffsets,
                    requiresTagReplacement,
                    propsTable,
                ),
            )
        deferred = true
        if (!manager.isRootViewAttached && drainScheduled.add(surfaceId)) {
          shouldScheduleDrain = true
          scheduleGeneration = surfaceGeneration[surfaceId] ?: 0L
        }
      } else {
        dispatchingSurfaces.add(surfaceId)
        applyImmediately = true
      }
    }
    if (deferred) {
      if (shouldScheduleDrain) {
        manager.scheduleDirectBatchOnViewAttach {
          synchronized(ownershipLock) {
            if ((surfaceGeneration[surfaceId] ?: 0L) != scheduleGeneration) {
              return@scheduleDirectBatchOnViewAttach
            }
            drainScheduled.remove(surfaceId)
          }
          drainPendingBatches(surfaceId)
        }
      }
      if (manager.isRootViewAttached) drainPendingBatches(surfaceId)
      return
    }
    if (applyImmediately) {
      try {
        applyBatchNow(
            surfaceId,
            manager,
            instructions,
            objects,
            stateMapBuffer,
            stateTags,
            stateWrappers,
            preparedTextTags,
            preparedTextLayouts,
            phaseOffsets,
            requiresTagReplacement,
            propsTable)
      } finally {
        synchronized(ownershipLock) { dispatchingSurfaces.remove(surfaceId) }
      }
      drainPendingBatches(surfaceId)
    }
  }

  /**
   * Imperative work is ordered after any direct batch that is waiting for the
   * root attachment. C++ state is published before Java ViewState exists, so
   * checking only the C++ tag would allow commands/responders/accessibility to
   * run against a missing Java view.
   */
  fun dispatchCommand(
      surfaceId: Int,
      reactTag: Int,
      commandName: String,
      args: ReadableArray,
  ) {
    enqueueImperative(
        surfaceId,
        PendingImperative.Command(reactTag, commandName, args),
    )
  }

  fun sendAccessibilityEvent(surfaceId: Int, reactTag: Int, eventType: String) {
    if (eventType !in ACCESSIBILITY_EVENT_TYPES) {
      throw IllegalArgumentException(
          "sendAccessibilityEventFromJS: invalid eventType $eventType",
      )
    }
    enqueueImperative(
        surfaceId,
        PendingImperative.Accessibility(reactTag, eventType),
    )
  }

  fun setJSResponder(
      surfaceId: Int,
      reactTag: Int,
      isJSResponder: Boolean,
      blockNativeResponder: Boolean,
  ) {
    enqueueImperative(
        surfaceId,
        PendingImperative.Responder(reactTag, isJSResponder, blockNativeResponder),
    )
  }

  private fun enqueueImperative(surfaceId: Int, operation: PendingImperative) {
    val manager = surface(surfaceId)
    var runNow = false
    var shouldScheduleDrain = false
    var scheduleGeneration = 0L
    synchronized(ownershipLock) {
      val canRunNow =
          manager.isRootViewAttached &&
              pendingBatches[surfaceId].isNullOrEmpty() &&
              (!requiresMountedTarget(operation) ||
                  directTags[surfaceId]?.contains(operation.reactTag) == true)
      if (canRunNow) {
        runNow = true
      } else {
        pendingImperative.getOrPut(surfaceId) { ArrayDeque() }.addLast(operation)
        if (!manager.isRootViewAttached && drainScheduled.add(surfaceId)) {
          shouldScheduleDrain = true
          scheduleGeneration = surfaceGeneration[surfaceId] ?: 0L
        }
      }
    }
    if (runNow) {
      applyImperative(manager, surfaceId, operation)
    }
    if (shouldScheduleDrain) {
      manager.scheduleDirectBatchOnViewAttach {
          synchronized(ownershipLock) {
            if ((surfaceGeneration[surfaceId] ?: 0L) != scheduleGeneration) {
              return@scheduleDirectBatchOnViewAttach
            }
            drainScheduled.remove(surfaceId)
          }
          drainPendingBatches(surfaceId)
      }
    }
  }

  private fun drainPendingBatches(surfaceId: Int) {
    val manager = surface(surfaceId)
    while (true) {
      val batches = synchronized(ownershipLock) {
        if (!manager.isRootViewAttached || dispatchingSurfaces.contains(surfaceId)) {
          return
        }
        val selectedBatches = pendingBatches.remove(surfaceId)
        if (selectedBatches != null && selectedBatches.isNotEmpty()) {
          dispatchingSurfaces.add(surfaceId)
        }
        selectedBatches
      }
      if (batches.isNullOrEmpty()) break
      try {
        while (batches.isNotEmpty()) {
          val batch = batches.removeFirst()
          applyBatchNow(
              surfaceId,
              manager,
              batch.instructions,
              batch.objects,
              batch.stateMapBuffer,
              batch.stateTags,
              batch.stateWrappers,
              batch.preparedTextTags,
              batch.preparedTextLayouts,
              batch.phaseOffsets,
              batch.requiresTagReplacement,
              batch.propsTable,
          )
        }
      } finally {
        synchronized(ownershipLock) { dispatchingSurfaces.remove(surfaceId) }
      }
    }
    drainPendingImperative(surfaceId, manager)
  }

  private fun drainPendingImperative(
      surfaceId: Int,
      manager: SurfaceMountingManager,
  ) {
    while (true) {
      val next = synchronized(ownershipLock) {
        if (!manager.isRootViewAttached ||
            pendingBatches[surfaceId]?.isNotEmpty() == true) {
          return
        }
        val operations = pendingImperative[surfaceId] ?: return
        if (operations.isEmpty()) {
          pendingImperative.remove(surfaceId)
          return
        }
        val operation = operations.removeFirst()
        val shouldDrop = requiresMountedTarget(operation) &&
            directTags[surfaceId]?.contains(operation.reactTag) != true
        if (operations.isEmpty()) pendingImperative.remove(surfaceId)
        operation to shouldDrop
      }
      val operation = next.first
      if (next.second) {
        FLog.e(
            "LoomViewApplier",
            "Dropping imperative operation for unmounted direct tag ${operation.reactTag} " +
                "on surface $surfaceId",
        )
        continue
      }
      applyImperative(manager, surfaceId, operation)
    }
  }

  private fun applyImperative(
      manager: SurfaceMountingManager,
      surfaceId: Int,
      operation: PendingImperative,
  ) {
    when (operation) {
      is PendingImperative.Command ->
          mountingManager.receiveCommand(
              surfaceId,
              operation.reactTag,
              operation.commandName,
              operation.args,
          )
      is PendingImperative.Accessibility -> {
        val eventType =
            when (operation.eventType) {
              "focus" -> android.view.accessibility.AccessibilityEvent.TYPE_VIEW_FOCUSED
              "windowStateChange" ->
                  android.view.accessibility.AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED
              "click" -> android.view.accessibility.AccessibilityEvent.TYPE_VIEW_CLICKED
              "viewHoverEnter" -> android.view.accessibility.AccessibilityEvent.TYPE_VIEW_HOVER_ENTER
              else -> error("validated accessibility event type")
            }
        mountingManager.sendAccessibilityEvent(surfaceId, operation.reactTag, eventType)
      }
      is PendingImperative.Responder ->
          if (operation.isJSResponder) {
            mountingManager.getSurfaceManagerEnforced(surfaceId, "setJSResponder")
                .setJSResponder(
                    operation.reactTag,
                    operation.reactTag,
                    operation.blockNativeResponder,
                )
          } else {
            mountingManager.clearJSResponder()
          }
    }
  }

  private fun requiresMountedTarget(operation: PendingImperative): Boolean =
      operation !is PendingImperative.Responder || operation.isJSResponder

  private fun applyBatchNow(
      surfaceId: Int,
      manager: SurfaceMountingManager,
      instructions: IntArray,
      objects: Array<Any?>,
      stateMapBuffer: ReadableMapBuffer?,
      stateTags: IntArray,
      stateWrappers: Array<Any?>,
      preparedTextTags: IntArray,
      preparedTextLayouts: Array<Any?>,
      phaseOffsets: IntArray?,
      requiresTagReplacement: Boolean,
      propsTable: LoomPropsTable?,
  ) {
    // Loom (debug builds only): time the apply so a slow one is reported with its size.
    val jankStart = if (BuildConfig.DEBUG) System.nanoTime() else 0L
    applyBatchNowInternal(
        surfaceId,
        manager,
        instructions,
        objects,
        stateMapBuffer,
        stateTags,
        stateWrappers,
        preparedTextTags,
        preparedTextLayouts,
        phaseOffsets,
        requiresTagReplacement,
        propsTable,
    )
    if (BuildConfig.DEBUG) {
      com.facebook.react.common.JankReporterSupport.noteApply(
          System.nanoTime() - jankStart, instructions.size)
    }
  }

  private fun applyBatchNowInternal(
      surfaceId: Int,
      manager: SurfaceMountingManager,
      instructions: IntArray,
      objects: Array<Any?>,
      stateMapBuffer: ReadableMapBuffer?,
      stateTags: IntArray,
      stateWrappers: Array<Any?>,
      preparedTextTags: IntArray,
      preparedTextLayouts: Array<Any?>,
      phaseOffsets: IntArray?,
      requiresTagReplacement: Boolean,
      propsTable: LoomPropsTable?,
  ) {
    val profile = if (BuildConfig.LOOM_PROFILE) BatchProfile() else null
    val batchStartNanos = profile?.let { SystemClock.elapsedRealtimeNanos() } ?: 0L
    val surfaceManager = profile?.let { mountingManager.getSurfaceManager(surfaceId) }
    if (profile != null) surfaceManager?.beginDirectBatchProfile(profile)
    val batchGeneration = surfaceGeneration[surfaceId] ?: 0L
    var failingOpcode = "DECODE"
    var failingTag: Int? = null
    var failingOffset = -1
    fun surfaceGenerationIsCurrent(): Boolean =
        (surfaceGeneration[surfaceId] ?: 0L) == batchGeneration

    try {
    if (phaseOffsets != null) {
      var validOffsets = phaseOffsets.size == 7 &&
          phaseOffsets.first() == 0 && phaseOffsets.last() == instructions.size
      for (offsetIndex in 1 until phaseOffsets.size) {
        validOffsets = validOffsets &&
            phaseOffsets[offsetIndex - 1] <= phaseOffsets[offsetIndex]
      }
      check(validOffsets) {
        "Invalid direct mount phase offsets"
      }
    }
    synchronized(ownershipLock) {
      directTags.getOrPut(surfaceId) { HashSet() }
    }
    val statesByTag = LinkedHashMap<Int, StateWrapper>(stateTags.size)
    if (stateMapBuffer != null) {
      val tags = stateMapBuffer.getIntBuffer(0)
      val states = stateMapBuffer.getMapBufferList(1)
      for (stateIndex in tags.indices) {
        statesByTag[tags[stateIndex]] = DirectMapBufferStateWrapper(states[stateIndex])
      }
    }
    for (stateIndex in stateTags.indices) {
      val stateWrapper = stateWrappers.getOrNull(stateIndex) as? StateWrapper ?: continue
      statesByTag[stateTags[stateIndex]] = stateWrapper
    }
    check(preparedTextTags.size == preparedTextLayouts.size) {
      "Prepared text state tags and layouts must have matching lengths"
    }
    for (stateIndex in preparedTextTags.indices) {
      val prepared = preparedTextLayouts[stateIndex] as? PreparedLayout
          ?: error("Prepared text state ${preparedTextTags[stateIndex]} is not a PreparedLayout")
      statesByTag[preparedTextTags[stateIndex]] = DirectPreparedTextStateWrapper(prepared)
    }
    var reusedTags: Set<Int> = emptySet()
    var preAppliedRemoveOffsets: Set<Int> = emptySet()
    if (requiresTagReplacement) {
      // Rare identity replacement pays for a discovery pass; normal batches
      // proceed directly to execution without allocating reuse sets or
      // collecting remove offsets.
      val createdTagsInBatch = HashSet<Int>()
      val deleteTagsInBatch = HashSet<Int>()
      val removeInstructions = ArrayList<RemoveInstruction>()
      var scanIndex = 0
      var scanObjectIndex = 0
      while (scanIndex < instructions.size) {
        val instructionOffset = scanIndex
        failingOffset = instructionOffset
        failingOpcode = opcodeName(instructions[scanIndex])
        failingTag = instructions.getOrNull(scanIndex + 1)
        when (instructions[scanIndex++]) {
          OP_CREATE -> {
            createdTagsInBatch.add(instructions[scanIndex])
            scanIndex += 2
            scanObjectIndex += 2
          }
          OP_UPDATE_PROPS -> {
            scanIndex += 1
            scanObjectIndex += 1
          }
          OP_INSERT -> scanIndex += 3
          OP_REMOVE -> {
            val parentTag = instructions[scanIndex++]
            val tag = instructions[scanIndex++]
            val position = instructions[scanIndex++]
            removeInstructions.add(
                RemoveInstruction(instructionOffset, parentTag, tag, position),
            )
          }
          OP_LAYOUT -> scanIndex += 8
          OP_DELETE -> {
            val tag = instructions[scanIndex++]
            deleteTagsInBatch.add(tag)
          }
          OP_PADDING, OP_OVERFLOW_INSET -> scanIndex += 5
          else -> throw IllegalStateException("Unknown direct operation in UI-thread batch")
        }
      }
      check(scanObjectIndex <= objects.size) {
        "Direct operation object count exceeds the supplied object array"
      }
      reusedTags = createdTagsInBatch.intersect(deleteTagsInBatch)
      val appliedRemoveOffsets = HashSet<Int>()
      for (remove in removeInstructions) {
        if (!surfaceGenerationIsCurrent()) return
        if (remove.parentTag in reusedTags || remove.reactTag in reusedTags) {
          manager.removeViewAt(remove.reactTag, remove.parentTag, remove.position)
          appliedRemoveOffsets.add(remove.offset)
        }
      }
      preAppliedRemoveOffsets = appliedRemoveOffsets
      for (tag in reusedTags) {
        if (!surfaceGenerationIsCurrent()) return
        synchronized(ownershipLock) { directTags[surfaceId]?.remove(tag) }
        if (manager.getViewExists(tag)) {
          manager.deleteView(tag)
          check(!manager.getViewExists(tag)) {
            "Cannot reuse Loom renderer tag $tag while its previous view is retained " +
                "for an active touch"
          }
        }
      }
    }
    var index = 0
    var objectIndex = 0
    val deletedTags = HashSet<Int>()
    if (phaseOffsets != null) {
      var deleteIndex = phaseOffsets[5]
      while (deleteIndex < phaseOffsets[6]) {
        check(instructions[deleteIndex] == OP_DELETE) { "Expected delete in delete phase" }
        val deletedTag = instructions[deleteIndex + 1]
        deletedTags.add(deletedTag)
        deleteIndex += 2
      }
    }
    val primaryEnd = phaseOffsets?.get(4) ?: instructions.size
    while (index < primaryEnd) {
      if (!surfaceGenerationIsCurrent()) return
      failingOffset = index
      val op = instructions[index++]
      failingOpcode = opcodeName(op)
      failingTag = when (op) {
        OP_INSERT, OP_REMOVE -> instructions.getOrNull(index + 1)
        else -> instructions.getOrNull(index)
      }
      when (op) {
        OP_CREATE -> {
          val tag = instructions[index++]
          val isLayoutable = instructions[index++] != 0
          val componentName = objects[objectIndex++] as String
          val propsSlot = objectIndex++
          val props = objects[propsSlot] as ReadableMap? ?: propsTable?.mapAt(propsSlot)
              ?: error("Direct create $tag has no props")
          // State travels in the same JNI batch as its create/update operations.
          // Pass it through the normal ViewManager creation contract for newly
          // created nodes, then apply any remaining states after the batch.
          val initialState = statesByTag.remove(tag)
          synchronized(ownershipLock) { directTags.getOrPut(surfaceId) { HashSet() }.add(tag) }
          // Creating a tag whose ViewState is still registered would silently bind
          // the new generation to the previous view, because
          // `SurfaceMountingManager.createView` returns early when a view exists.
          // Retire the old state first and fail loudly when it cannot be removed,
          // exactly like the flagged reuse pre-pass above.
          if (manager.getViewExists(tag)) {
            manager.deleteView(tag)
            check(!manager.getViewExists(tag)) {
              "Cannot create Loom renderer tag $tag while its previous view is retained " +
                  "for an active touch"
            }
          }
          // `isLayoutable == false` still registers a view state: the tag keeps
          // its props, event routing, commands and accessibility, it just does
          // not get a view. Such tags are never inserted into a parent.
          profileTimed(profile, "other") {
            manager.createView(componentName, tag, props, initialState, null, isLayoutable)
          }
        }
        OP_UPDATE_PROPS -> {
          val tag = instructions[index++]
          val propsSlot = objectIndex++
          manager.updateProps(
              tag,
              objects[propsSlot] as ReadableMap? ?: propsTable?.mapAt(propsSlot)
                  ?: error("Direct props update $tag has no props"))
        }
        OP_INSERT -> {
          val parentTag = instructions[index++]
          val tag = instructions[index++]
          val position = instructions[index++]
          profileTimed(profile, "other") { manager.addViewAt(parentTag, tag, position) }
        }
        OP_REMOVE -> {
          val parentTag = instructions[index++]
          val tag = instructions[index++]
          val position = instructions[index++]
          if (index - 4 !in preAppliedRemoveOffsets) {
            profileTimed(profile, "other") { manager.removeViewAt(tag, parentTag, position) }
          }
        }
        OP_LAYOUT -> {
          index += 8
        }
        OP_PADDING, OP_OVERFLOW_INSET -> {
          val tag = instructions[index++]
          val left = dipToPixel(Float.fromBits(instructions[index++]))
          val top = dipToPixel(Float.fromBits(instructions[index++]))
          val right = dipToPixel(Float.fromBits(instructions[index++]))
          val bottom = dipToPixel(Float.fromBits(instructions[index++]))
          if (op == OP_PADDING) {
            profileTimed(profile, "other") { manager.updatePadding(tag, left, top, right, bottom) }
          } else {
            profileTimed(profile, "other") { manager.updateOverflowInset(tag, left, top, right, bottom) }
          }
        }
        OP_DELETE -> {
          val tag = instructions[index++]
          if (tag !in reusedTags) {
            deletedTags.add(tag)
          }
        }
        else -> throw IllegalStateException("Unknown direct operation in UI-thread batch")
      }
    }
    for ((reactTag, stateWrapper) in statesByTag) {
      if (!surfaceGenerationIsCurrent()) return
      failingOpcode = "STATE"
      failingTag = reactTag
      val shouldApply = reactTag !in deletedTags && isDirectTag(surfaceId, reactTag)
      if (shouldApply) {
        profileTimed(profile, "state") { manager.updateState(reactTag, stateWrapper) }
        profile?.stateCalls = (profile?.stateCalls ?: 0) + 1
      }
    }
    // Layout/deletes are explicit trailing phases, after props and state.
    // This avoids rescanning and decoding operations that are already applied.
    if (phaseOffsets != null) {
      index = phaseOffsets[4]
      while (index < phaseOffsets[5]) {
        if (!surfaceGenerationIsCurrent()) return
        failingOffset = index
        failingOpcode = opcodeName(instructions[index])
        failingTag = instructions.getOrNull(index + 1)
        check(instructions[index++] == OP_LAYOUT) { "Expected layout in layout phase" }
        val tag = instructions[index++]
        val parentTag = instructions[index++]
        val displayType = instructions[index++]
        val layoutDirection = instructions[index++]
        val x = dipToPixel(Float.fromBits(instructions[index]))
        val y = dipToPixel(Float.fromBits(instructions[index + 1]))
        val width = dipToPixel(Float.fromBits(instructions[index + 2]))
        val height = dipToPixel(Float.fromBits(instructions[index + 3]))
        index += 4
        profileTimed(profile, "layout") {
          manager.updateLayout(tag, parentTag, x, y, width, height, displayType, layoutDirection)
        }
        profile?.layoutCalls = (profile?.layoutCalls ?: 0) + 1
      }
      index = phaseOffsets[5]
      while (index < phaseOffsets[6]) {
        if (!surfaceGenerationIsCurrent()) return
        failingOffset = index
        failingOpcode = opcodeName(instructions[index])
        val tag = instructions[index + 1]
        failingTag = tag
        check(instructions[index++] == OP_DELETE) { "Expected delete in delete phase" }
        index++
        if (tag !in reusedTags) {
          synchronized(ownershipLock) { directTags[surfaceId]?.remove(tag) }
          if (manager.getViewExists(tag)) {
            profileTimed(profile, "other") { manager.deleteView(tag) }
          }
        }
      }
    } else {
    // Legacy unit-test batches retain the old decoder path; production batches
    // always carry the phase table from the native encoder.
    index = 0
    while (index < instructions.size) {
      if (!surfaceGenerationIsCurrent()) return
      failingOffset = index
      failingOpcode = opcodeName(instructions[index])
      failingTag = instructions.getOrNull(index + 1)
      when (val op = instructions[index++]) {
        OP_CREATE -> index += 2
        OP_UPDATE_PROPS -> index += 1
        OP_INSERT, OP_REMOVE -> index += 3
        OP_LAYOUT -> {
          val tag = instructions[index++]
          val parentTag = instructions[index++]
          val displayType = instructions[index++]
          val layoutDirection = instructions[index++]
          val x = dipToPixel(Float.fromBits(instructions[index]))
          val y = dipToPixel(Float.fromBits(instructions[index + 1]))
          val width = dipToPixel(Float.fromBits(instructions[index + 2]))
          val height = dipToPixel(Float.fromBits(instructions[index + 3]))
          index += 4
          manager.updateLayout(
              tag,
              parentTag,
              x,
              y,
              width,
              height,
              displayType,
              layoutDirection,
          )
        }
        OP_DELETE -> {
          val tag = instructions[index++]
          if (tag in reusedTags) {
            // The old generation was deleted before OP_CREATE; this trailing
            // delete belongs to that generation and must not remove the new one.
          } else {
            synchronized(ownershipLock) { directTags[surfaceId]?.remove(tag) }
          }
          if (tag !in reusedTags && manager.getViewExists(tag)) {
            manager.deleteView(tag)
          }
        }
        OP_PADDING, OP_OVERFLOW_INSET -> index += 5
        else -> throw IllegalStateException("Unknown direct operation in UI-thread batch")
      }
    }
    }
    } catch (failure: Throwable) {
      val diagnostic = buildString {
        append("Loom renderer batch failed at ")
        append(failingOpcode)
        if (failingTag != null) append(" tag ").append(failingTag)
        append(" offset ").append(failingOffset)
        append(": ").append(failure.message ?: failure.javaClass.simpleName)
      }
      synchronized(ownershipLock) {
        surfaceGeneration[surfaceId] = (surfaceGeneration[surfaceId] ?: 0L) + 1L
        directSurfaces.remove(surfaceId)
        directTags.remove(surfaceId)
        pendingBatches.remove(surfaceId)
        pendingImperative.remove(surfaceId)
        drainScheduled.remove(surfaceId)
      }
      FLog.e("LoomViewApplier", diagnostic, failure)
      try {
        onFailure.accept(surfaceId, diagnostic)
      } catch (stopFailure: Throwable) {
        FLog.e(
            "LoomViewApplier",
            "Unable to stop failed Loom renderer surface $surfaceId",
            stopFailure,
        )
      }
      // The surface is stopped and this applier's generation is already invalidated above, so the
      // failure is consumed here. Rethrowing would escape the native batch application (a Java
      // exception crossing JNI into the renderer's effect loop) and take the process down.
    } finally {
      if (profile != null) {
        profile.totalNanos = SystemClock.elapsedRealtimeNanos() - batchStartNanos
        surfaceManager?.endDirectBatchProfile(profile)
        val toMs: (Long) -> String = { nanos -> "%.3f".format(nanos / 1_000_000.0) }
        Log.i(
            "LoomDirectMountProfile",
            "surface=$surfaceId totalMs=${toMs(profile.totalNanos)} " +
                "props=${profile.propsCalls}/${toMs(profile.propsTotalNanos)} " +
                "propsPreprocessMs=${toMs(profile.propsPreprocessNanos)} " +
                "viewManagerUpdatePropertiesMs=${toMs(profile.propsManagerNanos)} " +
                "state=${profile.stateCalls}/${toMs(profile.stateNanos)} " +
                "layout=${profile.layoutCalls}/${toMs(profile.layoutNanos)} " +
                "other=${profile.otherCalls}/${toMs(profile.otherNanos)}",
        )
      }
    }
  }

  private inline fun profileTimed(profile: BatchProfile?, category: String, block: () -> Unit) {
    if (profile == null) {
      block()
      return
    }
    val start = SystemClock.elapsedRealtimeNanos()
    try {
      block()
    } finally {
      val elapsed = SystemClock.elapsedRealtimeNanos() - start
      when (category) {
        "state" -> profile.stateNanos += elapsed
        "layout" -> profile.layoutNanos += elapsed
        else -> {
          profile.otherNanos += elapsed
          profile.otherCalls++
        }
      }
    }
  }

  private fun opcodeName(opcode: Int): String =
      when (opcode) {
        OP_CREATE -> "CREATE"
        OP_UPDATE_PROPS -> "UPDATE_PROPS"
        OP_INSERT -> "INSERT"
        OP_REMOVE -> "REMOVE"
        OP_LAYOUT -> "LAYOUT"
        OP_DELETE -> "DELETE"
        OP_PADDING -> "PADDING"
        OP_OVERFLOW_INSET -> "OVERFLOW_INSET"
        else -> "UNKNOWN($opcode)"
      }

  companion object {
    const val OP_CREATE = 0
    const val OP_UPDATE_PROPS = 1
    const val OP_INSERT = 2
    const val OP_REMOVE = 3
    const val OP_LAYOUT = 4
    const val OP_DELETE = 5
    const val OP_PADDING = 6
    const val OP_OVERFLOW_INSET = 7
    val ACCESSIBILITY_EVENT_TYPES =
        setOf("focus", "windowStateChange", "click", "viewHoverEnter")
  }
}
