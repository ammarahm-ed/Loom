<script setup lang="ts">
import {computed} from 'vue';
import DiagramFrame from './DiagramFrame.vue';
import {FRAME_MS, timelines} from './timelines';

const props = defineProps<{name: string}>();
const spec = timelines[props.name];
const uid = `loom-tl-${props.name}`;

const WIDTH = 960;
const LABEL_W = 110;
const ROW_H = 52;
const ROW_GAP = 10;
const TOP = 30;
const RESULT_H = 40;
const LEGEND_H = 30;

const TONE_NAMES: Record<string, string> = {
  gray: 'touch / draw',
  indigo: 'your JavaScript (React render)',
  red: 'long urgent JavaScript',
  green: 'apply view changes',
  brand: 'Loom: layout, text, diff',
  purple: 'JavaScript worker',
  yellow: 'other',
};

const g = computed(() => {
  const span = spec.frames * FRAME_MS;
  const plotW = WIDTH - LABEL_W - 12;
  const x = (ms: number) => LABEL_W + (ms / span) * plotW;
  const rowY = (r: number) => TOP + r * (ROW_H + ROW_GAP);
  const rowsBottom = rowY(spec.rows.length) - ROW_GAP;
  const resultY = rowsBottom + 14;
  const legendY = resultY + (spec.results ? RESULT_H : 0) + 4;
  const tones = [...new Set(spec.blocks.map(b => b.tone ?? 'brand'))];
  let lx = LABEL_W;
  let ly = legendY;
  const legend = tones.map(tone => {
    const name = TONE_NAMES[tone] ?? tone;
    const w = 26 + name.length * 6.6 + 22;
    if (lx + w > WIDTH && lx > LABEL_W) {
      lx = LABEL_W;
      ly += 22;
    }
    const item = {tone, name, x: lx, y: ly};
    lx += w;
    return item;
  });
  const height = ly + LEGEND_H;

  const vsyncs = Array.from({length: spec.frames}, (_, i) => {
    const ms = (i + 1) * FRAME_MS;
    return {x: x(ms), label: `refresh ${i + 1}`, ms};
  });

  const blocks = spec.blocks.map(b => {
    const bx = x(b.start);
    const w = Math.max(4, x(b.end) - bx);
    const label = b.label ?? '';
    // Fit the label: about 7 px per character at 12 px.
    const fits = label.length * 6.1 + 8 <= w;
    return {
      x: bx,
      y: rowY(b.row) + 9,
      w,
      h: ROW_H - 18,
      tone: b.tone ?? 'brand',
      label: fits ? label : '',
      cx: bx + w / 2,
      cy: rowY(b.row) + ROW_H / 2 + 4,
    };
  });

  const arrows = (spec.arrows ?? []).map(a => {
    const fromDown = a.toRow > a.fromRow;
    const y1 = fromDown ? rowY(a.fromRow) + ROW_H - 9 : rowY(a.fromRow) + 9;
    const y2 = fromDown ? rowY(a.toRow) + 9 : rowY(a.toRow) + ROW_H - 9;
    return {x1: x(a.at), y1, x2: x(a.to ?? a.at), y2};
  });

  const results = (spec.results ?? []).map((r, i) => {
    const cx = vsyncs[i].x;
    return {cx, y: resultY, result: r.result, label: r.label};
  });

  return {
    height,
    rows: spec.rows.map((name, r) => ({name, y: rowY(r)})),
    rowsBottom,
    vsyncs,
    blocks,
    arrows,
    results,
    legend,
  };
});
</script>

<template>
  <DiagramFrame :title="spec.title">
    <svg
      class="loom-svg"
      :viewBox="`0 0 ${WIDTH} ${g.height}`"
      role="img"
      :aria-labelledby="`${uid}-t`"
      xmlns="http://www.w3.org/2000/svg"
    >
      <title :id="`${uid}-t`">{{ spec.title }}</title>
      <defs>
        <marker :id="`${uid}-a`" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto">
          <path d="M1 1.5L9 5L1 8.5z" class="loom-arrow-head" />
        </marker>
      </defs>
      <g v-for="(row, i) in g.rows" :key="'r' + i">
        <rect :x="0" :y="row.y" :width="WIDTH" :height="ROW_H" rx="10" class="loom-lane" />
        <text :x="14" :y="row.y + ROW_H / 2 + 4" class="loom-lane-label">{{ row.name }}</text>
      </g>
      <g v-for="(v, i) in g.vsyncs" :key="'v' + i">
        <line :x1="v.x" :x2="v.x" :y1="TOP - 8" :y2="g.rowsBottom + 6" class="loom-vsync" />
        <text :x="v.x" :y="TOP - 14" text-anchor="middle" class="loom-vsync-label">{{ v.label }}</text>
      </g>
      <g v-for="(b, i) in g.blocks" :key="'b' + i" :class="['loom-tl-block', b.tone]">
        <rect :x="b.x" :y="b.y" :width="b.w" :height="b.h" rx="6" />
        <text v-if="b.label" :x="b.cx" :y="b.cy" text-anchor="middle" class="loom-tl-text">{{ b.label }}</text>
      </g>
      <line
        v-for="(a, i) in g.arrows"
        :key="'a' + i"
        :x1="a.x1"
        :y1="a.y1"
        :x2="a.x2"
        :y2="a.y2"
        class="loom-edge dashed"
        :marker-end="`url(#${uid}-a)`"
      />
      <g v-for="(r, i) in g.results" :key="'s' + i" :class="['loom-tl-result', r.result]">
        <template v-if="r.result !== 'none'">
          <rect :x="r.cx - 62" :y="r.y" width="124" height="26" rx="13" />
          <text :x="r.cx" :y="r.y + 17" text-anchor="middle">{{ r.label }}</text>
        </template>
      </g>
      <g v-for="(l, i) in g.legend" :key="'g' + i" :class="['loom-tl-block', l.tone]">
        <rect :x="l.x" :y="l.y + 6" width="18" height="12" rx="3" />
        <text :x="l.x + 24" :y="l.y + 16" class="loom-tl-legend">{{ l.name }}</text>
      </g>
    </svg>
  </DiagramFrame>
</template>
