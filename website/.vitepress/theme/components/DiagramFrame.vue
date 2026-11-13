<script setup lang="ts">
import {nextTick, onBeforeUnmount, ref} from 'vue';

defineProps<{title: string}>();

const open = ref(false);
const frame = ref<HTMLElement | null>(null);
const closeButton = ref<HTMLButtonElement | null>(null);
const dialog = ref<HTMLElement | null>(null);

// Pan and zoom state of the overlay, applied as one CSS transform.
const scale = ref(1);
const tx = ref(0);
const ty = ref(0);
const pointers = new Map<number, {x: number; y: number}>();
let pinchStart: {distance: number; scale: number} | null = null;
let moved = false;
let previousOverflow = '';

function reset() {
  scale.value = 1;
  tx.value = 0;
  ty.value = 0;
}

async function show() {
  if (open.value) return;
  reset();
  previousOverflow = document.body.style.overflow;
  document.body.style.overflow = 'hidden';
  open.value = true;
  document.addEventListener('keydown', onKey, true);
  await nextTick();
  closeButton.value?.focus();
}

function hide() {
  if (!open.value) return;
  open.value = false;
  pointers.clear();
  pinchStart = null;
  document.body.style.overflow = previousOverflow;
  document.removeEventListener('keydown', onKey, true);
  frame.value?.focus();
}

function onKey(event: KeyboardEvent) {
  if (event.key === 'Escape') {
    event.preventDefault();
    event.stopPropagation();
    hide();
  } else if (event.key === 'Tab') {
    // The close button is the only focusable control: keep focus on it.
    event.preventDefault();
    closeButton.value?.focus();
  }
}

function onFrameKey(event: KeyboardEvent) {
  if (event.key === 'Enter' || event.key === ' ') {
    event.preventDefault();
    show();
  }
}

function clamp(value: number, low: number, high: number) {
  return Math.min(high, Math.max(low, value));
}

function distance() {
  const [a, b] = [...pointers.values()];
  return Math.hypot(a.x - b.x, a.y - b.y);
}

function onPointerDown(event: PointerEvent) {
  (event.currentTarget as HTMLElement).setPointerCapture?.(event.pointerId);
  pointers.set(event.pointerId, {x: event.clientX, y: event.clientY});
  moved = false;
  if (pointers.size === 2) pinchStart = {distance: distance(), scale: scale.value};
}

function onPointerMove(event: PointerEvent) {
  const previous = pointers.get(event.pointerId);
  if (!previous) return;
  const next = {x: event.clientX, y: event.clientY};
  if (pointers.size === 2 && pinchStart) {
    pointers.set(event.pointerId, next);
    scale.value = clamp(pinchStart.scale * (distance() / pinchStart.distance), 1, 8);
    moved = true;
    return;
  }
  if (scale.value > 1) {
    tx.value += next.x - previous.x;
    ty.value += next.y - previous.y;
  }
  if (Math.abs(next.x - previous.x) + Math.abs(next.y - previous.y) > 2) moved = true;
  pointers.set(event.pointerId, next);
}

function onPointerUp(event: PointerEvent) {
  pointers.delete(event.pointerId);
  if (pointers.size < 2) pinchStart = null;
  if (scale.value <= 1) reset();
}

function onBackdropClick(event: MouseEvent) {
  // A tap on empty backdrop closes; a drag that ended there does not.
  if (event.target === event.currentTarget && !moved) hide();
}

function onWheel(event: WheelEvent) {
  event.preventDefault();
  scale.value = clamp(scale.value * (event.deltaY < 0 ? 1.15 : 1 / 1.15), 1, 8);
  if (scale.value === 1) reset();
}

function onDoubleClick() {
  if (scale.value > 1) reset();
  else scale.value = 2.5;
}

onBeforeUnmount(() => {
  if (open.value) {
    document.body.style.overflow = previousOverflow;
    document.removeEventListener('keydown', onKey, true);
  }
});
</script>

<template>
  <figure class="loom-frame">
    <div
      ref="frame"
      class="loom-frame-inline"
      tabindex="0"
      role="button"
      :aria-label="`${title}. Open full screen`"
      @click="show"
      @keydown="onFrameKey"
    >
      <slot />
    </div>
      <span class="loom-frame-expand" aria-hidden="true">
        <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M14 3h7v7M10 21H3v-7M21 3l-7.5 7.5M3 21l7.5-7.5" />
        </svg>
      </span>
    <Teleport to="body">
      <div v-if="open" class="loom-overlay" @click="onBackdropClick">
        <div ref="dialog" class="loom-overlay-dialog" role="dialog" aria-modal="true" :aria-label="title">
          <button ref="closeButton" class="loom-overlay-close" type="button" aria-label="Close diagram" @click="hide">
            <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true">
              <path d="M6 6l12 12M18 6L6 18" />
            </svg>
          </button>
          <div
            class="loom-overlay-stage"
            @pointerdown="onPointerDown"
            @pointermove="onPointerMove"
            @pointerup="onPointerUp"
            @pointercancel="onPointerUp"
            @wheel="onWheel"
            @dblclick="onDoubleClick"
            @click="onBackdropClick"
          >
            <div
              class="loom-overlay-content"
              :style="{transform: `translate(${tx}px, ${ty}px) scale(${scale})`}"
            >
              <slot />
            </div>
          </div>
        </div>
      </div>
    </Teleport>
  </figure>
</template>
