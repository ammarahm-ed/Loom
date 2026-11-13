export interface NodeSpec {
  id: string;
  lane: number;
  col: number;
  span?: number;
  title: string;
  sub?: string;
  tone?: 'brand' | 'green' | 'purple' | 'yellow' | 'indigo';
  dashed?: boolean;
}
export interface EdgeSpec {
  from: string;
  to: string;
  label?: string;
  dashed?: boolean;
  /** Where an elbow turns, 0..1 between the two cards. */
  mid?: number;
  /** Elbow that leaves vertically first. */
  vfirst?: boolean;
}
export interface Spec {
  title: string;
  lanes: string[];
  cols: number;
  nodes: NodeSpec[];
  edges: EdgeSpec[];
}

const WIDTH = 960;
const LABEL_W = 92;
const GAP = 32;
const R = 8;

/** Splits a word longer than a line at camelCase boundaries. */
function breakWord(word: string, max: number): string[] {
  if (word.length <= max) return [word];
  const parts = word.split(/(?=[A-Z])/);
  const out: string[] = [];
  let cur = '';
  for (const part of parts) {
    if (cur && (cur + part).length > max) {
      out.push(cur);
      cur = part;
    } else {
      cur += part;
    }
  }
  if (cur) out.push(cur);
  return out;
}

function wrap(text: string, max: number): string[] {
  if (!text) return [];
  const lines: string[] = [];
  let line = '';
  for (const raw of text.split(' ')) {
    for (const word of breakWord(raw, max)) {
      if (line && (line + ' ' + word).length > max) {
        lines.push(line);
        line = word;
      } else {
        line = line ? line + ' ' + word : word;
      }
    }
  }
  if (line) lines.push(line);
  return lines;
}

type P = [number, number];

function rounded(pts: P[]): string {
  let d = `M${pts[0][0]} ${pts[0][1]}`;
  for (let i = 1; i < pts.length - 1; i++) {
    const [px, py] = pts[i - 1];
    const [x, y] = pts[i];
    const [nx, ny] = pts[i + 1];
    const r = Math.min(R, Math.hypot(x - px, y - py) / 2, Math.hypot(nx - x, ny - y) / 2);
    const ax = x - Math.sign(x - px) * r;
    const ay = y - Math.sign(y - py) * r;
    const bx = x + Math.sign(nx - x) * r;
    const by = y + Math.sign(ny - y) * r;
    d += ` L${ax} ${ay} Q${x} ${y} ${bx} ${by}`;
  }
  const last = pts[pts.length - 1];
  return d + ` L${last[0]} ${last[1]}`;
}

export function layout(spec: Spec) {
  const hasLabels = spec.lanes.some(Boolean);
  const x0 = (hasLabels ? LABEL_W : 0) + 14;
  const colW = (WIDTH - x0 - 14 + GAP) / spec.cols;
  const cardW = colW - GAP;

  const prepared = spec.nodes.map((n) => {
    const w = (n.span ?? 1) * colW - GAP;
    const titleLines = wrap(n.title, Math.floor((w - 14) / 8.1));
    const subLines = wrap(n.sub ?? '', Math.floor((w - 14) / 6.2));
    return {n, w, titleLines, subLines};
  });
  const need = Math.max(
    ...prepared.map((p) => p.titleLines.length * 19 + (p.subLines.length ? 6 + p.subLines.length * 16 : 0)),
  );
  const cardH = need + 24;
  const laneH = cardH + 46;
  const bandY = (i: number) => 10 + i * (laneH + 10);

  const nodes = prepared.map(({n, w, titleLines, subLines}) => {
    const x = x0 + n.col * colW;
    const y = bandY(n.lane) + (laneH - cardH) / 2;
    const block = titleLines.length * 19 + (subLines.length ? 6 + subLines.length * 16 : 0);
    const top = y + (cardH - block) / 2;
    return {
      id: n.id, x, y, w, h: cardH, tone: n.tone ?? '', dashed: !!n.dashed,
      titleLines, subLines,
      ty: top + 14,
      sy: top + titleLines.length * 19 + 6 + 12,
    };
  });
  const byId = new Map(nodes.map((n) => [n.id, n]));

  const edges = spec.edges.map((e) => {
    const a = byId.get(e.from)!;
    const b = byId.get(e.to)!;
    const acy = a.y + a.h / 2;
    const bcy = b.y + b.h / 2;
    let pts: P[];
    let lx: number;
    let ly: number;
    const overlapL = Math.max(a.x, b.x) + 14;
    const overlapR = Math.min(a.x + a.w, b.x + b.w) - 14;
    if (Math.abs(acy - bcy) < 1) {
      const right = b.x >= a.x + a.w;
      const fx = right ? a.x + a.w : a.x;
      const tx = right ? b.x : b.x + b.w;
      pts = [[fx, acy], [tx, acy]];
      lx = (fx + tx) / 2;
      ly = acy;
    } else if (overlapR > overlapL) {
      const x = (overlapL + overlapR) / 2;
      const down = bcy > acy;
      const y1 = down ? a.y + a.h : a.y;
      const y2 = down ? b.y : b.y + b.h;
      pts = [[x, y1], [x, y2]];
      lx = x;
      ly = (y1 + y2) / 2;
    } else {
      const right = b.x > a.x;
      if (e.vfirst) {
        const cx = a.x + a.w / 2;
        const down = bcy > a.y;
        const y1 = down ? a.y + a.h : a.y;
        const tx = right ? b.x : b.x + b.w;
        pts = [[cx, y1], [cx, bcy], [tx, bcy]];
        lx = (cx + tx) / 2;
        ly = bcy;
        return {d: rounded(pts), dashed: !!e.dashed, label: e.label ?? '', lx, ly, lw: (e.label?.length ?? 0) * 6.6 + 18};
      }
      const fx = right ? a.x + a.w : a.x;
      const tx = right ? b.x : b.x + b.w;
      const mx = fx + (tx - fx) * (e.mid ?? 0.5);
      pts = [[fx, acy], [mx, acy], [mx, bcy], [tx, bcy]];
      lx = mx;
      ly = (acy + bcy) / 2;
    }
    return {
      d: rounded(pts), dashed: !!e.dashed, label: e.label ?? '', lx, ly,
      lw: (e.label?.length ?? 0) * 6.6 + 18,
    };
  });

  const lanes = spec.lanes.map((label, i) => ({
    y: bandY(i), h: laneH, cy: bandY(i) + laneH / 2, lines: wrap(label, 11),
  }));
  return {width: WIDTH, height: bandY(spec.lanes.length) , lanes, nodes, edges, cardW};
}
