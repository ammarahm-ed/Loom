# Large updates and transitions

With JavaScript on the main thread, rendering competes with drawing. Loom keeps frames
flowing for work that can be split:

- **`startTransition` updates** are rendered in slices of a few milliseconds. Between slices,
  Loom hands the main thread back to Android so a pending frame can draw, then continues. A
  1000-cell update that would freeze animations for 50 ms instead lands over a few frames
  while animations keep running.
- **Urgent updates** (a plain `setState` outside a transition, event handlers) are never split:
  the user is waiting for their result.

```tsx
import {startTransition} from 'react';

function onFilterChange(query: string) {
  setQuery(query); // urgent: the input updates immediately
  startTransition(() => {
    setResults(filter(allItems, query)); // large: rendered in slices
  });
}
```

## Tuning

The share of each frame reserved for drawing is a build-time constant
(`kFrameReservePercent`, 35% by default). A higher value drops fewer frames but lands large
updates later.
