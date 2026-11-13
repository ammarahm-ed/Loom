# Overrides model

Loom is not a fork. `packages/react-native-loom/overrides.json` lists every file Loom touches:

| Type | Meaning |
|---|---|
| `patch` | A full copy of an upstream file with Loom's edits, recorded against the sha256 of the pristine npm file (`baseHash`) |
| `platform` | A Loom-only file that does not exist upstream |
| `delete` | An upstream file Loom removes |
| `test` | A unit test npm doesn't ship, kept for CI |

`loom materialize` builds `node_modules/.loom/react-native`: the pristine package with the
overrides applied. `node_modules/react-native` is never modified; it stays the merge base.

Taking a new React Native release is `loom upgrade --to <version>`: a 3-way merge of each patch
onto the new upstream file. `loom status` reports how much Loom carries.
