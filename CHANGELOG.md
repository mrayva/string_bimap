# Changelog

## Unreleased

- Removed experimental transducer/FST-style backends:
  - `CompactMemoryFst`
  - `CompactMemoryKeyvi`
  - `CompactMemoryMarisaFsst`
- Simplified the static base implementation to the supported backends:
  - fallback map
  - `xcdat`
  - `marisa-trie`
- Changed serialized file magic from the old repo-era value to `STRBMAP1`.
- Added explicit benchmark-sidecar reporting and mixed-vs-compacted snapshot comparison.
- Added compaction policy APIs and benchmark visibility for them.
- Added `try_get_string(id)` to disambiguate missing/deleted IDs.
- Split the old monolithic smoke test into focused core, persistence, and query tests.
- Added benchmark matrix automation.
- Added richer internal memory accounting.

## 0.1.0

- Initial public repository version of `string_bimap`.
- Stable-ID string dictionary with:
  - mutable delta overlay
  - compact static base backends
  - iteration and prefix queries
  - binary save/load
  - compaction support
