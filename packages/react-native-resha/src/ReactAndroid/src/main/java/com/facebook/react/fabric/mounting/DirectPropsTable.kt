/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.react.fabric.mounting

import com.facebook.proguard.annotations.DoNotStrip
import com.facebook.react.bridge.JavaOnlyArray
import com.facebook.react.bridge.JavaOnlyMap
import com.facebook.react.bridge.ReadableMap

/**
 * React Native X: the props of one direct-renderer batch, encoded as ints by the native side
 * (`DirectPropsEncoder` in FabricMountingManager.cpp).
 *
 * Reading a ReadableNativeMap goes through JNI for its keys and again for its values, which cost
 * several microseconds per view when a large update was applied. This table crosses JNI once per
 * batch and is decoded here into plain [JavaOnlyMap]s, so view managers still get a [ReadableMap].
 * Numbers are decoded as Double, like ReadableNativeMap returns them.
 */
@DoNotStrip
internal class DirectPropsTable
@DoNotStrip
constructor(
    private val offsets: IntArray,
    private val data: IntArray,
    private val strings: Array<String?>,
) {
  private var cursor = 0

  /** The props map for object slot [slot], or null when that slot carries a Java object. */
  fun mapAt(slot: Int): ReadableMap? {
    val offset = offsets.getOrNull(slot) ?: return null
    if (offset < 0) return null
    cursor = offset
    check(data[cursor++] == TYPE_MAP) { "Direct props slot $slot is not a map" }
    return readMap()
  }

  private fun readMap(): JavaOnlyMap {
    val count = data[cursor++]
    val map = JavaOnlyMap()
    repeat(count) {
      val key = strings[data[cursor++]]!!
      when (val type = data[cursor++]) {
        TYPE_NULL -> map.putNull(key)
        TYPE_FALSE -> map.putBoolean(key, false)
        TYPE_TRUE -> map.putBoolean(key, true)
        TYPE_INT -> map.putDouble(key, data[cursor++].toDouble())
        TYPE_DOUBLE -> map.putDouble(key, readDouble())
        TYPE_STRING -> map.putString(key, strings[data[cursor++]])
        TYPE_MAP -> map.putMap(key, readMap())
        TYPE_ARRAY -> map.putArray(key, readArray())
        else -> error("Unknown direct props value type $type")
      }
    }
    return map
  }

  private fun readArray(): JavaOnlyArray {
    val count = data[cursor++]
    val array = JavaOnlyArray()
    repeat(count) {
      when (val type = data[cursor++]) {
        TYPE_NULL -> array.pushNull()
        TYPE_FALSE -> array.pushBoolean(false)
        TYPE_TRUE -> array.pushBoolean(true)
        TYPE_INT -> array.pushDouble(data[cursor++].toDouble())
        TYPE_DOUBLE -> array.pushDouble(readDouble())
        TYPE_STRING -> array.pushString(strings[data[cursor++]])
        TYPE_MAP -> array.pushMap(readMap())
        TYPE_ARRAY -> array.pushArray(readArray())
        else -> error("Unknown direct props value type $type")
      }
    }
    return array
  }

  private fun readDouble(): Double {
    val high = data[cursor++].toLong() shl 32
    val low = data[cursor++].toLong() and 0xffffffffL
    return java.lang.Double.longBitsToDouble(high or low)
  }

  private companion object {
    const val TYPE_NULL = 0
    const val TYPE_FALSE = 1
    const val TYPE_TRUE = 2
    const val TYPE_INT = 3
    const val TYPE_DOUBLE = 4
    const val TYPE_STRING = 5
    const val TYPE_MAP = 6
    const val TYPE_ARRAY = 7
  }
}
