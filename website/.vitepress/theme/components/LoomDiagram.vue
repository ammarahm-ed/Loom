<script setup lang="ts">
import {computed} from 'vue';
import DiagramFrame from './DiagramFrame.vue';
import {layout} from './layout';
import {specs} from './diagrams';

const props = defineProps<{name: string}>();
const spec = specs[props.name];
const g = computed(() => layout(spec));
const uid = `loom-${props.name}`;
</script>

<template>
  <DiagramFrame :title="spec.title">
    <svg
      class="loom-svg"
      :viewBox="`0 0 ${g.width} ${g.height}`"
      role="img"
      :aria-labelledby="`${uid}-t`"
      xmlns="http://www.w3.org/2000/svg"
    >
      <title :id="`${uid}-t`">{{ spec.title }}</title>
      <defs>
        <marker :id="`${uid}-a`" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto">
          <path d="M1 1.5L9 5L1 8.5z" class="loom-arrow-head" />
        </marker>
      </defs>
      <g v-for="(lane, i) in g.lanes" :key="'l' + i">
        <rect :x="0" :y="lane.y" :width="g.width" :height="lane.h" rx="10" class="loom-lane" />
        <text v-for="(line, j) in lane.lines" :key="j" :x="16" :y="lane.cy + (j - (lane.lines.length - 1) / 2) * 16 + 4" class="loom-lane-label">{{ line }}</text>
      </g>
      <path
        v-for="(e, i) in g.edges"
        :key="'e' + i"
        :d="e.d"
        :class="['loom-edge', {dashed: e.dashed}]"
        :marker-end="`url(#${uid}-a)`"
      />
      <g v-for="(n, i) in g.nodes" :key="'n' + i" :class="['loom-card', n.tone, {dashed: n.dashed}]">
        <rect :x="n.x" :y="n.y" :width="n.w" :height="n.h" rx="10" />
        <text :x="n.x + n.w / 2" text-anchor="middle" class="loom-card-title">
          <tspan v-for="(line, j) in n.titleLines" :key="j" :x="n.x + n.w / 2" :y="n.ty + j * 19">{{ line }}</tspan>
        </text>
        <text :x="n.x + n.w / 2" text-anchor="middle" class="loom-card-sub">
          <tspan v-for="(line, j) in n.subLines" :key="j" :x="n.x + n.w / 2" :y="n.sy + j * 16">{{ line }}</tspan>
        </text>
      </g>
      <g v-for="(e, i) in g.edges" :key="'c' + i">
        <template v-if="e.label">
          <rect :x="e.lx - e.lw / 2" :y="e.ly - 10" :width="e.lw" height="20" rx="10" class="loom-chip" />
          <text :x="e.lx" :y="e.ly + 4" text-anchor="middle" class="loom-chip-text">{{ e.label }}</text>
        </template>
      </g>
    </svg>
  </DiagramFrame>
</template>
