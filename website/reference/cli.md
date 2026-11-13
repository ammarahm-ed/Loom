# CLI

## App commands

Run from your app's root.

| Command | What it does |
|---|---|
| `loom materialize [--force] [--out <dir>]` | Builds `node_modules/.loom/react-native`: the installed `react-native` with Loom's overrides applied. Runs automatically from the Gradle, CocoaPods and Metro integrations |
| `loom validate` | Checks Loom's overrides against the installed `react-native` |

## Maintainer commands

Run in the Loom repository.

| Command | What it does |
|---|---|
| `loom status` | Overrides by area, and lines carried |
| `loom diff [path...]` | Unified diff of overrides against upstream |
| `loom add <path> [--area a]` | Start overriding an upstream file |
| `loom add --platform <path>` | Register a new Loom-only file |
| `loom upgrade --to <version>` | 3-way merge the overrides onto a new React Native release |
| `loom overlay --monorepo <dir>` | Write the overrides into a react-native checkout |
| `loom import-fork --fork <dir> --base <sha>` | Capture a checkout's changes back into overrides (the inverse of `overlay`) |

`overlay` also writes the Android unit tests and the rn-tester benchmarks into the checkout.
`--upstream <dir>` makes a command use that `react-native` package instead of fetching one from
npm. `loom status` is the headline metric for shrinking the patch surface.

See [Maintaining Loom](/architecture/maintaining) for the workflow.
