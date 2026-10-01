/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.react.fabric.mounting

import com.facebook.react.ReactRootView
import com.facebook.react.bridge.JavaOnlyMap
import com.facebook.react.bridge.JavaOnlyArray
import com.facebook.react.bridge.ReactTestHelper
import com.facebook.react.bridge.ReadableArray
import com.facebook.react.bridge.ReadableNativeMap
import com.facebook.react.fabric.mounting.MountingManager.MountItemExecutor
import com.facebook.react.internal.featureflags.ReactNativeFeatureFlagsForTests
import com.facebook.react.common.mapbuffer.ReadableMapBuffer
import com.facebook.react.bridge.WritableMap
import com.facebook.react.uimanager.ReactStylesDiffMap
import com.facebook.react.uimanager.StateWrapper
import com.facebook.react.uimanager.ThemedReactContext
import com.facebook.react.uimanager.ViewManager
import com.facebook.react.uimanager.ViewManagerRegistry
import com.facebook.react.views.view.ReactViewManager
import com.facebook.react.views.text.PreparedLayout
import com.facebook.react.views.text.PreparedLayoutTextView
import com.facebook.react.views.text.PreparedLayoutTextViewManager
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import org.assertj.core.api.Assertions.assertThat
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

/**
 * Regression coverage for the Java boundary of the direct UI-thread renderer.
 *
 * The C++ renderer owns revision/layout correctness; this test deliberately
 * verifies only the direct operation contract after JNI decoding:
 * create → insert → layout → replay → delete. It must not exercise
 * MountItemDispatcher or the legacy Fabric transaction queue.
 */
@RunWith(RobolectricTestRunner::class)
class UIThreadDirectViewApplierTest {
  private val surfaceId = 1
  private val childTag = 42
  private lateinit var mountingManager: MountingManager
  private lateinit var surfaceManager: SurfaceMountingManager
  private lateinit var applier: UIThreadDirectViewApplier
  private lateinit var stateManager: StateRecordingViewManager
  private lateinit var rootView: ReactRootView
  private val operationOrder = mutableListOf<String>()

  @Before
  fun setUp() {
    ReactNativeFeatureFlagsForTests.setUp()
    val reactContext = ReactTestHelper.createCatalystContextForTest()
    val themedContext = ThemedReactContext(reactContext, reactContext, null, -1)
    operationOrder.clear()
    stateManager = StateRecordingViewManager(operationOrder)
    mountingManager =
        MountingManager(
            ViewManagerRegistry(listOf<ViewManager<*, *>>(stateManager)),
            MountItemExecutor {},
        )
    rootView = ReactRootView(themedContext)
    mountingManager.startSurface(surfaceId, themedContext, rootView)
    surfaceManager = mountingManager.getSurfaceManagerEnforced(surfaceId, "direct-test")
    applier = UIThreadDirectViewApplier(mountingManager)
  }

  @Test
  fun nativeOperationWireValuesRemainStable() {
    assertThat(UIThreadDirectViewApplier.OP_CREATE).isEqualTo(0)
    assertThat(UIThreadDirectViewApplier.OP_UPDATE_PROPS).isEqualTo(1)
    assertThat(UIThreadDirectViewApplier.OP_INSERT).isEqualTo(2)
    assertThat(UIThreadDirectViewApplier.OP_REMOVE).isEqualTo(3)
    assertThat(UIThreadDirectViewApplier.OP_LAYOUT).isEqualTo(4)
    assertThat(UIThreadDirectViewApplier.OP_DELETE).isEqualTo(5)
    assertThat(UIThreadDirectViewApplier.OP_PADDING).isEqualTo(6)
    assertThat(UIThreadDirectViewApplier.OP_OVERFLOW_INSET).isEqualTo(7)
  }

  @Test
  fun viewManagerFailure_invalidatesSurfaceAndDoesNotReplayBatch() {
    val failureMessages = mutableListOf<String>()
    applier =
        UIThreadDirectViewApplier(mountingManager) { failedSurfaceId, message ->
          failureMessages.add(message)
          mountingManager.stopSurface(failedSurfaceId)
          applier.clearSurface(failedSurfaceId)
        }
    stateManager.onCreateView = { throw IllegalStateException("injected create failure") }
    applier.dispatchCommand(surfaceId, childTag, "mustNotRun", JavaOnlyArray.of())

    org.assertj.core.api.Assertions.assertThatThrownBy {
      applier.applyBatch(
          surfaceId,
          intArrayOf(OP_CREATE, childTag, 1),
          arrayOf("RCTView", JavaOnlyMap.of()),
      )
    }.isInstanceOf(IllegalStateException::class.java)

    assertThat(failureMessages).hasSize(1)
    assertThat(failureMessages.single()).contains("CREATE", "tag $childTag")
    assertThat(applier.isDirectTag(surfaceId, childTag)).isFalse()
    assertThat(stateManager.commandCount).isZero()
  }

  @Test
  fun propsAndStateFailuresReportTheExactFailingOperation() {
    applier.applyBatch(
        surfaceId,
        intArrayOf(OP_CREATE, childTag, 1),
        arrayOf("RCTView", JavaOnlyMap.of()),
    )
    val messages = mutableListOf<String>()
    applier = failingApplier(messages)
    stateManager.onUpdateProperties = { throw IllegalStateException("props failed") }

    org.assertj.core.api.Assertions.assertThatThrownBy {
      applier.applyBatch(
          surfaceId,
          intArrayOf(OP_UPDATE_PROPS, childTag),
          arrayOf(JavaOnlyMap.of("opacity", 0.5)),
      )
    }.isInstanceOf(IllegalStateException::class.java)
    assertThat(messages.single()).contains("UPDATE_PROPS", "tag $childTag")

    setUp()
    val stateMessages = mutableListOf<String>()
    applier = failingApplier(stateMessages)
    applier.applyBatch(
        surfaceId,
        intArrayOf(OP_CREATE, childTag, 1),
        arrayOf("RCTView", JavaOnlyMap.of()),
    )
    stateManager.onStateUpdate = { throw IllegalStateException("state failed") }

    org.assertj.core.api.Assertions.assertThatThrownBy {
      applier.applyBatch(
          surfaceId,
          intArrayOf(),
          emptyArray(),
          null,
          intArrayOf(childTag),
          arrayOf(TestStateWrapper),
      )
    }.isInstanceOf(IllegalStateException::class.java)
    assertThat(stateMessages.single()).contains("STATE", "tag $childTag")
  }

  private fun failingApplier(messages: MutableList<String>) =
      UIThreadDirectViewApplier(mountingManager) { failedSurfaceId, message ->
        messages.add(message)
        mountingManager.stopSurface(failedSurfaceId)
        applier.clearSurface(failedSurfaceId)
      }

  @Test
  fun deferredBatch_keepsStateForTheCreateBeingReplayed() {
    val detachedSurfaceId = 3
    val detachedTag = 44
    mountingManager.startSurface(detachedSurfaceId, surfaceManager.context, null)
    val detachedRoot = ReactRootView(checkNotNull(surfaceManager.context))

    applier.applyBatch(
        detachedSurfaceId,
        intArrayOf(OP_CREATE, detachedTag, 1),
        arrayOf("RCTView", JavaOnlyMap.of()),
        null,
        intArrayOf(detachedTag),
        arrayOf(TestStateWrapper),
    )

    mountingManager.attachRootView(
        detachedSurfaceId,
        detachedRoot,
        checkNotNull(surfaceManager.context),
    )

    assertThat(stateManager.stateUpdateCount).isEqualTo(1)
  }

  @Test
  fun applyBatch_beforeRootAttach_replaysTheWholeBatchAfterAttach() {
    val detachedSurfaceId = 2
    val detachedChildTag = 43
    mountingManager.startSurface(detachedSurfaceId, surfaceManager.context, null)
    val detachedRoot = ReactRootView(checkNotNull(surfaceManager.context))

    applier.applyBatch(
        detachedSurfaceId,
        intArrayOf(
            OP_CREATE,
            detachedChildTag,
            1,
            OP_INSERT,
            detachedSurfaceId,
            detachedChildTag,
            0,
            OP_LAYOUT,
            detachedChildTag,
            detachedSurfaceId,
            0,
            0,
            0f.toRawBits(),
            0f.toRawBits(),
            64f.toRawBits(),
            24f.toRawBits(),
        ),
        arrayOf("RCTView", JavaOnlyMap.of()),
    )

    assertThat(applier.isDirectTag(detachedSurfaceId, detachedChildTag)).isFalse()
    assertThat(mountingManager.getSurfaceManagerEnforced(detachedSurfaceId, "detached-test").getViewExists(detachedChildTag)).isFalse()

    mountingManager.attachRootView(detachedSurfaceId, detachedRoot, checkNotNull(surfaceManager.context))

    val detachedManager = mountingManager.getSurfaceManagerEnforced(detachedSurfaceId, "attached-test")
    assertThat(applier.isDirectTag(detachedSurfaceId, detachedChildTag)).isTrue()
    assertThat(detachedManager.getView(detachedChildTag)).isNotNull
    assertThat(detachedManager.getView(detachedChildTag)!!.width).isEqualTo(64)
    assertThat(detachedManager.getView(detachedChildTag)!!.height).isEqualTo(24)
  }

  @Test
  fun command_beforeRootAttach_runsAfterTheCreatingBatch() {
    val detachedSurfaceId = 4
    val detachedTag = 45
    mountingManager.startSurface(detachedSurfaceId, surfaceManager.context, null)
    val detachedRoot = ReactRootView(checkNotNull(surfaceManager.context))

    applier.applyBatch(
        detachedSurfaceId,
        intArrayOf(OP_CREATE, detachedTag, 1),
        arrayOf("RCTView", JavaOnlyMap.of()),
    )
    applier.dispatchCommand(detachedSurfaceId, detachedTag, "directTest", JavaOnlyArray.of())

    assertThat(stateManager.commandCount).isZero()

    mountingManager.attachRootView(
        detachedSurfaceId,
        detachedRoot,
        checkNotNull(surfaceManager.context),
    )

    assertThat(stateManager.commandCount).isEqualTo(1)
    assertThat(stateManager.lastCommand).isEqualTo("directTest")
  }

  @Test
  fun applyBatch_replaysCreateInsertAndLayoutWithoutLegacyDispatcher() {
    applier.applyBatch(
        surfaceId,
        intArrayOf(
            OP_CREATE,
            childTag,
            1,
            OP_INSERT,
            surfaceId,
            childTag,
            0,
            OP_LAYOUT,
            childTag,
            surfaceId,
            0,
            0,
            0f.toRawBits(),
            0f.toRawBits(),
            100f.toRawBits(),
            40f.toRawBits(),
        ),
        arrayOf("RCTView", JavaOnlyMap.of()),
    )

    assertThat(applier.isDirectTag(surfaceId, childTag)).isTrue()
    assertThat(surfaceManager.getViewExists(childTag)).isTrue()
    assertThat(surfaceManager.getView(childTag)).isNotNull()
    assertThat(surfaceManager.getView(childTag)!!.width).isEqualTo(100)
    assertThat(surfaceManager.getView(childTag)!!.height).isEqualTo(40)
  }

  @Test
  fun applyBatch_updatesStateBeforeMeasuringAndLayingOutTheView() {
    applier.applyBatch(
        surfaceId,
        intArrayOf(OP_CREATE, childTag, 1),
        arrayOf("RCTView", JavaOnlyMap.of()),
    )

    operationOrder.clear()
    applier.applyBatch(
        surfaceId,
        intArrayOf(
            OP_UPDATE_PROPS,
            childTag,
            OP_LAYOUT,
            childTag,
            surfaceId,
            0,
            0,
            3f.toRawBits(),
            4f.toRawBits(),
            80f.toRawBits(),
            24f.toRawBits(),
        ),
        arrayOf(JavaOnlyMap.of()),
        null,
        intArrayOf(childTag),
        arrayOf(TestStateWrapper),
    )

    val view = surfaceManager.getView(childTag)
    assertThat(view).isNotNull
    assertThat(operationOrder.indexOf("state"))
        .isLessThan(operationOrder.indexOf("measure"))
    assertThat(operationOrder.count { it == "measure" }).isEqualTo(1)
    assertThat(view!!.left).isEqualTo(3)
    assertThat(view.top).isEqualTo(4)
    assertThat(view.width).isEqualTo(80)
    assertThat(view.height).isEqualTo(24)
  }

  @Test
  fun phasedBatch_appliesStateBeforeLayoutWithoutReplayingSkippedOperations() {
    applier.applyBatch(
        surfaceId,
        intArrayOf(OP_CREATE, childTag, 1),
        arrayOf("RCTView", JavaOnlyMap.of()),
    )
    operationOrder.clear()

    val instructions = intArrayOf(
        OP_UPDATE_PROPS,
        childTag,
        OP_LAYOUT,
        childTag,
        surfaceId,
        0,
        0,
        0f.toRawBits(),
        0f.toRawBits(),
        60f.toRawBits(),
        20f.toRawBits(),
    )
    // [create/props, removals, inserts, padding, layout, deletes, end]
    val phaseOffsets = intArrayOf(0, 2, 2, 2, 2, 11, 11)
    applier.applyBatch(
        surfaceId,
        instructions,
        arrayOf(JavaOnlyMap.of("opacity", 0.8)),
        null,
        intArrayOf(childTag),
        arrayOf(TestStateWrapper),
        phaseOffsets = phaseOffsets,
    )

    assertThat(operationOrder.indexOf("state"))
        .isLessThan(operationOrder.indexOf("measure"))
    assertThat(surfaceManager.getView(childTag)!!.width).isEqualTo(60)
  }

  @Test
  fun preparedTextStateUsesThePreparedLayoutReferenceDirectly() {
    val reactContext = ReactTestHelper.createCatalystContextForTest()
    val themedContext = ThemedReactContext(reactContext, reactContext, null, -1)
    val preparedManager = PreparedLayoutTextViewManager()
    val preparedMountingManager =
        MountingManager(
            ViewManagerRegistry(listOf<ViewManager<*, *>>(preparedManager)),
            MountItemExecutor {},
        )
    preparedMountingManager.startSurface(surfaceId, themedContext, ReactRootView(themedContext))
    val preparedApplier = UIThreadDirectViewApplier(preparedMountingManager)
    val text = "Prepared direct state"
    val paint = TextPaint(TextPaint.ANTI_ALIAS_FLAG)
    val layout = StaticLayout.Builder.obtain(text, 0, text.length, paint, 200)
        .setAlignment(Layout.Alignment.ALIGN_NORMAL)
        .setIncludePad(true)
        .build()
    val preparedLayout = PreparedLayout(layout, 0, 0f, intArrayOf(childTag), 0, 0)

    preparedApplier.applyBatch(
        surfaceId,
        intArrayOf(OP_CREATE, childTag, 1),
        arrayOf("RCTText", JavaOnlyMap.of()),
        preparedTextTags = intArrayOf(childTag),
        preparedTextLayouts = arrayOf(preparedLayout),
        phaseOffsets = intArrayOf(0, 3, 3, 3, 3, 3, 3),
    )

    val view = preparedMountingManager.getSurfaceManagerEnforced(surfaceId, "test")
        .getView(childTag) as PreparedLayoutTextView
    assertThat(view.preparedLayout).isSameAs(preparedLayout)
  }

  @Test
  fun reentrantMountBatchWaitsUntilTheCurrentViewManagerCallbackReturns() {
    val nestedTag = childTag + 1
    var enqueuedNestedBatch = false
    var outerCreateCallbackActive = false
    var nestedCreatedInsideOuterCallback = false
    stateManager.onCreateView = {
      if (!enqueuedNestedBatch) {
        enqueuedNestedBatch = true
        outerCreateCallbackActive = true
        applier.applyBatch(
            surfaceId,
            intArrayOf(OP_CREATE, nestedTag, 1),
            arrayOf("RCTView", JavaOnlyMap.of()),
        )
        assertThat(surfaceManager.getViewExists(nestedTag)).isFalse()
        outerCreateCallbackActive = false
      } else if (outerCreateCallbackActive) {
        nestedCreatedInsideOuterCallback = true
      }
    }

    applier.applyBatch(
        surfaceId,
        intArrayOf(OP_CREATE, childTag, 1),
        arrayOf("RCTView", JavaOnlyMap.of()),
    )

    assertThat(nestedCreatedInsideOuterCallback).isFalse()
    assertThat(surfaceManager.getViewExists(childTag)).isTrue()
    assertThat(surfaceManager.getViewExists(nestedTag)).isTrue()
  }

  @Test
  fun delete_removesDirectTagAndCachedView() {
    applier.applyBatch(
        surfaceId,
        intArrayOf(OP_CREATE, childTag, 1),
        arrayOf("RCTView", JavaOnlyMap.of()),
    )

    applier.applyBatch(surfaceId, intArrayOf(OP_DELETE, childTag), emptyArray())

    assertThat(applier.isDirectTag(surfaceId, childTag)).isFalse()
    assertThat(surfaceManager.getViewExists(childTag)).isFalse()
  }

  @Test
  fun reusedTag_deletesOldGenerationBeforeCreatingTheNewView() {
    applier.applyBatch(
        surfaceId,
        intArrayOf(
            OP_CREATE,
            childTag,
            1,
            OP_INSERT,
            surfaceId,
            childTag,
            0,
            OP_LAYOUT,
            childTag,
            surfaceId,
            0,
            0,
            0f.toRawBits(),
            0f.toRawBits(),
            40f.toRawBits(),
            20f.toRawBits(),
        ),
        arrayOf("RCTView", JavaOnlyMap.of()),
    )
    applier.applyBatch(
        surfaceId,
        intArrayOf(
            OP_REMOVE,
            surfaceId,
            childTag,
            0,
            OP_CREATE,
            childTag,
            1,
            OP_INSERT,
            surfaceId,
            childTag,
            0,
            OP_LAYOUT,
            childTag,
            surfaceId,
            0,
            0,
            0f.toRawBits(),
            0f.toRawBits(),
            60f.toRawBits(),
            30f.toRawBits(),
            OP_DELETE,
            childTag,
        ),
        arrayOf("RCTView", JavaOnlyMap.of()),
        requiresTagReplacement = true,
    )

    assertThat(surfaceManager.getViewExists(childTag)).isTrue()
    assertThat(applier.isDirectTag(surfaceId, childTag)).isTrue()
    assertThat(surfaceManager.getView(childTag)!!.width).isEqualTo(60)
  }

  @Test
  fun duplicateDelete_isIdempotentAndKeepsTagEvidenceInLog() {
    applier.applyBatch(
        surfaceId,
        intArrayOf(OP_CREATE, childTag, 1),
        arrayOf("RCTView", JavaOnlyMap.of()),
    )

    applier.applyBatch(
        surfaceId,
        intArrayOf(OP_DELETE, childTag, OP_DELETE, childTag),
        emptyArray(),
    )

    assertThat(surfaceManager.getViewExists(childTag)).isFalse()
    assertThat(applier.isDirectTag(surfaceId, childTag)).isFalse()
  }

  @Test
  fun clearSurface_removesDirectOwnership() {
    applier.applyBatch(
        surfaceId,
        intArrayOf(
            OP_CREATE,
            childTag,
            1,
            OP_INSERT,
            surfaceId,
            childTag,
            0,
            OP_LAYOUT,
            childTag,
            surfaceId,
            0,
            0,
            0f.toRawBits(),
            0f.toRawBits(),
            40f.toRawBits(),
            20f.toRawBits(),
        ),
        arrayOf("RCTView", JavaOnlyMap.of()),
    )

    assertThat(applier.isDirectTag(surfaceId, childTag)).isTrue()
    assertThat(applier.surfaceIdForDirectTag(childTag)).isEqualTo(surfaceId)
    applier.clearSurface(surfaceId)

    assertThat(applier.isDirectTag(surfaceId, childTag)).isFalse()
    assertThat(applier.surfaceIdForDirectTag(childTag)).isNull()
  }

  @Test
  fun stopDuringCreate_abortsTheRemainingBatchOperations() {
    stateManager.onCreateView = { applier.clearSurface(surfaceId) }

    applier.applyBatch(
        surfaceId,
        intArrayOf(
            OP_CREATE,
            childTag,
            1,
            OP_INSERT,
            surfaceId,
            childTag,
            0,
            OP_LAYOUT,
            childTag,
            surfaceId,
            0,
            0,
            0f.toRawBits(),
            0f.toRawBits(),
            40f.toRawBits(),
            20f.toRawBits(),
        ),
        arrayOf("RCTView", JavaOnlyMap.of()),
    )

    assertThat(rootView.childCount).isZero()
    assertThat(applier.isDirectTag(surfaceId, childTag)).isFalse()
  }

  private companion object {
    const val OP_CREATE = 0
    const val OP_UPDATE_PROPS = 1
    const val OP_INSERT = 2
    const val OP_REMOVE = 3
    const val OP_LAYOUT = 4
    const val OP_DELETE = 5
    const val OP_UPDATE_BACKGROUND_COLOR = 8
  }

  private class StateRecordingViewManager(
      private val operationOrder: MutableList<String>,
  ) : ReactViewManager() {
    var stateUpdateCount = 0
    var commandCount = 0
    var lastCommand: String? = null
    val lastUpdatePropKeys = mutableListOf<String>()
    var lastStateProps: Map<String, Any?> = emptyMap()
    var onCreateView: (() -> Unit)? = null
    var onUpdateProperties: (() -> Unit)? = null
    var onStateUpdate: (() -> Unit)? = null

    override fun updateProperties(
        view: com.facebook.react.views.view.ReactViewGroup,
        props: ReactStylesDiffMap,
    ) {
      onUpdateProperties?.invoke()
      lastUpdatePropKeys.clear()
      lastUpdatePropKeys.addAll(props.toMap().keys)
    }

    public override fun createViewInstance(
        context: ThemedReactContext,
    ): com.facebook.react.views.view.ReactViewGroup {
      onCreateView?.invoke()
      return OrderRecordingReactViewGroup(context, operationOrder)
    }

    override fun receiveCommand(
        root: com.facebook.react.views.view.ReactViewGroup,
        commandId: String,
        args: ReadableArray?,
    ) {
      commandCount += 1
      lastCommand = commandId
    }

    override fun updateState(
        view: com.facebook.react.views.view.ReactViewGroup,
        props: ReactStylesDiffMap,
        stateWrapper: StateWrapper,
    ): Any? {
      onStateUpdate?.invoke()
      stateUpdateCount += 1
      lastStateProps = props.toMap()
      operationOrder.add("state")
      return null
    }
  }

  private class OrderRecordingReactViewGroup(
      context: ThemedReactContext,
      private val operationOrder: MutableList<String>,
  ) : com.facebook.react.views.view.ReactViewGroup(context) {
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
      operationOrder.add("measure")
      super.onMeasure(widthMeasureSpec, heightMeasureSpec)
    }
  }

  private object TestStateWrapper : StateWrapper {
    override val stateDataMapBuffer: ReadableMapBuffer? = null
    override val stateData: ReadableNativeMap? = null
    override fun updateState(map: WritableMap) = Unit
    override fun destroyState() = Unit
  }
}
