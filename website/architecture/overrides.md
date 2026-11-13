# Overrides model

Loom is not a fork. `packages/react-native-loom/overrides.json` lists every file Loom touches:

| Type | Meaning |
|---|---|
| `patch` | A full copy of an upstream file with Loom's edits, recorded against the sha256 of the pristine npm file (`baseHash`) |
| `platform` | A Loom-only file that does not exist upstream |
| `delete` | An upstream file Loom removes |
| `test` | A unit test npm doesn't ship, kept for CI |

Loom is maintained out of tree: a package that layers explicit, hash-tracked, 3-way-mergeable
overrides on top of the unmodified `react-native` from npm, instead of a fork of
`facebook/react-native`. Taking an upstream fix is a version bump plus `loom upgrade`, not a fork
rebase.

`loom materialize` compiles the overrides into `node_modules/.loom/react-native`: a copy of the
pristine `node_modules/react-native` with the overrides applied, plus a `.loom-stamp.json` that
makes the step a no-op while it is current. `node_modules/react-native` is never modified; it
stays the 3-way merge base and the tree `loom validate` hashes against.

Materialize refuses to run when validation fails: the installed React Native version is not the
one the overrides target (`overrides.json > upstream.version`), an upstream file no longer
matches its `baseHash`, a `platform` file now also exists upstream, or `src/` has untracked
files.

Because the overrides patch React-Core and ReactAndroid sources, React Native always builds from
source in an app that uses Loom; prebuilt React Native core binaries cannot contain the
overrides.

Taking a new React Native release is `loom upgrade --to <version>`: a 3-way merge of each patch
onto the new upstream file. `loom status` reports how much Loom carries, and is the headline metric for shrinking the patch
surface.

The workflow for changing the overrides is in [Maintaining Loom](/architecture/maintaining).
