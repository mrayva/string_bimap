# string_bimap

Header-only C++17 string dictionary for:

- `string -> stable id`
- `id -> string`
- iteration over live `(id, string)` pairs
- prefix-query traversal over live entries
- faster native-order prefix traversal when stable ID ordering is not required
- mostly-static datasets with a mutable overlay
- binary save/load with stable IDs preserved

The design is:

- static base segment with explicit backend selection:
  - `FastLookup`: fallback map-based indexing
  - `CompactMemory`: optional `xcdat` for the base when compiled in
  - `CompactMemoryMarisa`: optional `marisa-trie` for the base when compiled in
  - `CompactMemoryMarisaFsst`: experimental `marisa-trie` base index plus FSST-compressed base payload
  - `CompactMemoryKeyvi`: optional `keyvi` for the base when compiled in
- mutable delta segment backed by a packed arena plus:
  - `FastLookup`: `std::unordered_map`
  - compact profiles: optional `hat-trie` when compiled in
- exact tombstones keyed by stable global IDs

The backend is selected per dictionary:

```cpp
string_bimap::StringBimap fast(0, string_bimap::BackendProfile::FastLookup);
string_bimap::StringBimap compact(0, string_bimap::BackendProfile::CompactMemory);
string_bimap::StringBimap marisa(0, string_bimap::BackendProfile::CompactMemoryMarisa);
string_bimap::StringBimap marisa_fsst(0, string_bimap::BackendProfile::CompactMemoryMarisaFsst);
string_bimap::StringBimap keyvi(0, string_bimap::BackendProfile::CompactMemoryKeyvi);
```

If optional dependencies are not compiled in, the compact profiles degrade gracefully to the fallback structures for the missing parts.

## Invariants

- IDs are assigned monotonically and are stable for the lifetime of the dictionary.
- IDs are never reused. If a string is deleted and later reinserted, it receives a new ID.
- Deleting a string creates a permanent logical hole in the ID space.
- `compact()` preserves all live IDs and only rewrites internal storage.
- `save()`/`load()` preserve the logical dictionary state, including stable live IDs and deleted holes.
- `save()`/`load()` preserve the selected backend profile.
- `save(path)` may emit compact sidecars for `BackendProfile::CompactMemory` when the on-disk snapshot is fully compacted.
- `load(path)` will use those sidecars when present; otherwise it falls back to logical rebuild and may generate the sidecars for later loads.
- Empty strings are ignored and never stored.

## Lifetime And Safety Rules

- `get_string()` returns a `std::string_view` into internal storage.
- `for_each_live()` and `for_each_with_prefix()` also pass `std::string_view`s into internal storage.
- Any mutating operation, including `insert()`, `erase()`, `load()`, and `compact()`, may invalidate previously returned views.
- Do not mutate the dictionary from inside `for_each_live()` or `for_each_with_prefix()` callbacks.
- `for_each_with_prefix()` returns results in stable ID order.
- `for_each_with_prefix_unordered()` may return results in backend/native order and is intended for faster prefix scans.
- The library is not internally synchronized. External locking is required for concurrent access if mutation is possible.
- `get_string(id)` returns an empty view for a missing or deleted ID.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

If `xcdat.hpp` is available through your toolchain or include path, the build enables the `CompactMemory` static backend. Otherwise that profile falls back to the standard-library static index.

If `marisa.h` and `libmarisa` are available through your toolchain or include path, the build enables the `CompactMemoryMarisa` static backend. Otherwise that profile falls back to the standard-library static index.

If `fsst.h` plus the local FSST source tree are available, the build can enable the experimental `CompactMemoryMarisaFsst` backend. This repo currently expects a local DuckDB/FSST checkout via `-DSTRING_BIMAP_FSST_ROOT=/path/to/fsst`.

If `keyvi` headers and dependencies are available, the build enables the `CompactMemoryKeyvi` static backend. This repo supports pointing CMake at a local checkout with `-DSTRING_BIMAP_KEYVI_ROOT=/path/to/keyvi`.

If `tsl/htrie_map.h` is available through your toolchain or include path, the build enables the optional compact-memory delta backend. Otherwise that profile falls back to `std::unordered_map`.

## Build With vcpkg

The repository includes local overlay ports for `xcdat` and `hat-trie` under `vcpkg_ports/`.

```sh
cmake -S . -B build-vcpkg \
  -DCMAKE_TOOLCHAIN_FILE=/home/mrayva/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DSTRING_BIMAP_USE_XCDAT=ON \
  -DSTRING_BIMAP_USE_MARISA=ON \
  -DSTRING_BIMAP_USE_HAT_TRIE=ON
cmake --build build-vcpkg
ctest --test-dir build-vcpkg --output-on-failure
```

This path installs all optional dependencies through the manifest and enables the real `xcdat`, `marisa-trie`, and `hat-trie` code paths.

## Benchmark

Build the benchmark executable:

```sh
cmake -S . -B build
cmake --build build --target string_bimap_bench
./build/string_bimap_bench --profile fast 20000 256 12345
```

With the dependency-backed configuration:

```sh
cmake -S . -B build-vcpkg \
  -DCMAKE_TOOLCHAIN_FILE=/home/mrayva/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DSTRING_BIMAP_USE_XCDAT=ON \
  -DSTRING_BIMAP_USE_HAT_TRIE=ON
cmake --build build-vcpkg --target string_bimap_bench
./build-vcpkg/string_bimap_bench --profile compact 20000 256 12345
./build-vcpkg/string_bimap_bench --profile marisa 20000 256 12345
```

The benchmark reports bulk insert, exact lookup, reverse lookup, prefix traversal, compaction, save, and load timings on a synthetic prefix-heavy dataset. It also prints both the compiled-in optional backends and the selected runtime profile.

### Current Summary

On the benchmarked real datasets so far:

- `FastLookup` is still the default recommendation for point-query-heavy workloads.
- `CompactMemoryMarisa` currently looks like the stronger experimental compact backend overall.
- `CompactMemory` (`xcdat`) is still competitive on shorter identifier-like corpora where it can be slightly smaller.
- `CompactMemoryMarisaFsst` is kept as an experimental payload-compression backend only. In the current implementation it underperformed plain `marisa` on both the stock/SEC corpora and an attempted Wikipedia run, and it can be removed later without affecting the mainline design.
- `CompactMemoryKeyvi` is kept as an experimental backend only. On the completed stock/SEC runs it underperformed both `marisa` and usually `xcdat`, and an attempted Wikipedia compact run was still running after `4m19s` at roughly `7.0 GB RSS`.
- `CompactMemoryFst` is now available as an experimental FST backend, but current results do not make it a preferred choice.

The table below summarizes the stock-style real-dataset results from the dependency-backed build. Times are nanoseconds per operation; memory is the internal post-load estimate reported by `memory_usage()`.

| Dataset | Column | Profile | Insert ns/op | Find ns/op | Get ns/op | Compact ns/op | Load ns/op | Internal After Load |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `StockETFList` | `Symbol` | `fast` | 733.7 | 207.8 | 227.0 | 1061.6 | 984.8 | 2.55 MB |
| `StockETFList` | `Symbol` | `compact` | 1686.08 | 372.21 | 399.25 | 3620.47 | 3491.85 | 0.76 MB |
| `StockETFList` | `Symbol` | `marisa` | 1656.90 | 366.58 | 391.55 | 1443.93 | 1329.13 | 0.83 MB |
| `StockETFList` | `Symbol` | `fst` | 1746.49 | 390.56 | 423.77 | 4111.11 | 3607.70 | 0.73 MB |
| `StockETFList` | `Company Name` | `fast` | 843.3 | 315.9 | 316.0 | 1345.2 | 1162.3 | 4.20 MB |
| `StockETFList` | `Company Name` | `compact` | 1748.42 | 440.15 | 457.36 | 11702.67 | 10347.20 | 2.79 MB |
| `StockETFList` | `Company Name` | `marisa` | 1921.39 | 542.60 | 590.74 | 3230.05 | 2602.86 | 2.44 MB |
| `StockETFList` | `Company Name` | `fst` | 2174.68 | 449.83 | 496.79 | 1412.74 | 1827.70 | 4.20 MB |
| `CUSIP.csv` | `cusip` | `fast` | 721.46 | 233.72 | 266.03 | 1158.54 | 1092.01 | 4.32 MB |
| `CUSIP.csv` | `cusip` | `compact` | 1733.84 | 394.13 | 416.34 | 6478.76 | 5872.09 | 1.54 MB |
| `CUSIP.csv` | `cusip` | `marisa` | 1521.21 | 393.58 | 416.67 | 1650.35 | 1895.64 | 1.63 MB |
| `CUSIP.csv` | `cusip` | `fst` | 1607.32 | 400.41 | 433.99 | 5618.53 | 5372.11 | 1.34 MB |
| `CUSIP.csv` | `symbol` | `fast` | 768.97 | 265.62 | 264.38 | 1163.94 | 1588.19 | 4.10 MB |
| `CUSIP.csv` | `symbol` | `compact` | 1580.97 | 397.22 | 429.00 | 4716.56 | 4619.25 | 1.26 MB |
| `CUSIP.csv` | `symbol` | `marisa` | 1589.89 | 380.06 | 436.36 | 1480.84 | 1428.64 | 1.36 MB |
| `CUSIP.csv` | `symbol` | `fst` | 1572.81 | 375.02 | 397.25 | 3861.01 | 3629.19 | 1.21 MB |
| `CUSIP.csv` | `description` | `fast` | 945.83 | 449.80 | 514.25 | 1425.14 | 1719.44 | 5.79 MB |
| `CUSIP.csv` | `description` | `compact` | 1764.58 | 470.88 | 529.30 | 7541.33 | 7269.51 | 3.19 MB |
| `CUSIP.csv` | `description` | `marisa` | 1804.52 | 502.10 | 572.40 | 2557.44 | 2403.66 | 2.81 MB |
| `CUSIP.csv` | `description` | `fst` | 2026.04 | 457.99 | 514.62 | 1308.83 | 1235.61 | 5.79 MB |
| `SEC_CIKs_Symbols.csv` | `cik` | `compact` | 1318.99 | 277.08 | 309.29 | 4715.56 | 4620.10 | 191 KB |
| `SEC_CIKs_Symbols.csv` | `cik` | `marisa` | 1370.18 | 281.97 | 310.85 | 1449.85 | 1375.40 | 207 KB |
| `SEC_CIKs_Symbols.csv` | `cik` | `fst` | 1333.96 | 274.67 | 313.03 | 5426.63 | 5242.71 | 180 KB |
| `SEC_CIKs_Symbols.csv` | `symbol` | `compact` | 1372.31 | 294.07 | 329.76 | 3129.22 | 3064.90 | 189 KB |
| `SEC_CIKs_Symbols.csv` | `symbol` | `marisa` | 1369.09 | 296.37 | 330.67 | 1349.17 | 1269.30 | 209 KB |
| `SEC_CIKs_Symbols.csv` | `symbol` | `fst` | 1373.88 | 294.28 | 323.92 | 3530.42 | 3400.80 | 175 KB |
| `SEC_CIKs_Symbols.csv` | `Name` | `compact` | 1481.12 | 303.77 | 332.48 | 7272.04 | 7031.88 | 375 KB |
| `SEC_CIKs_Symbols.csv` | `Name` | `marisa` | 1444.89 | 298.93 | 328.26 | 2136.58 | 1953.43 | 339 KB |
| `SEC_CIKs_Symbols.csv` | `Name` | `fst` | 1459.07 | 307.27 | 355.33 | 27648.42 | 26505.80 | 353 KB |

At this point the tradeoff is consistent:

- `FastLookup` wins clearly on insert, exact lookup, compaction, and load latency.
- Among the compact backends, `CompactMemoryMarisa` is usually much faster on compaction and load.
- `CompactMemory` can still be slightly smaller on shorter identifier-like columns.
- `CompactMemoryMarisa` is often smaller on longer text columns.
- `CompactMemoryFst` is occasionally compact on short-key corpora, but it is not competitive overall on large compact/save/load runs.
- If point queries are the dominant operation, start with `FastLookup`.
- If you want a compact backend, start by evaluating `CompactMemoryMarisa` on your real corpus.
- Keep `CompactMemory` in the mix if minimum footprint on short code/ticker keys matters most.
- Treat `CompactMemoryFst` as experimental only.

`CompactMemoryMarisaFsst` is intentionally not included in the main comparison tables. The current implementation keeps `marisa` for `string -> id` and stores the compact base payload with FSST, but the auxiliary state needed to support `get_string(id) -> std::string_view` erased the expected memory win.

Representative results from the dedicated FSST build:

| Dataset | Column | Profile | Load ns/op | Find Loaded ns/op | Get Loaded ns/op | Internal After Load |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| `StockETFList` | `Symbol` | `marisa` | 523.61 | n/a | n/a | 0.80 MB |
| `StockETFList` | `Symbol` | `marisa_fsst` | 3170.95 | n/a | n/a | 2.76 MB |
| `CUSIP.csv` | `description` | `marisa` | 589.22 | 3161.42 | 32.49 | 3.10 MB |
| `CUSIP.csv` | `description` | `marisa_fsst` | 8486.21 | 3239.05 | 29.12 | 5.40 MB |

An attempted Wikipedia `marisa_fsst` compact/save run in the dedicated FSST build was stopped after `4m10s` at roughly `8.4 GB RSS`, which was already materially worse than the completed `marisa` compact run. The backend remains in-tree only as an experiment and is a candidate for future removal if it does not improve.

Wikipedia titles tell the same story at larger scale. Using `/tmp/enwiki-latest-all-titles-in-ns0.keys.txt` with compact snapshots:

| Corpus | Profile | Insert | Find | Get | Compact | Save | Load-only | Find Loaded | Get Loaded | Internal After Load |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `enwiki-latest-all-titles-in-ns0` | `compact` | 63.4 s | 28.0 s | 33.4 s | 210.2 s | 187.1 s | 8.95 s | 3975.81 ns/op | 32.28 ns/op | 708 MB |
| `enwiki-latest-all-titles-in-ns0` | `marisa` | 63.2 s | 29.2 s | 34.9 s | 86.3 s | 72.3 s | 8.91 s | 2144.53 ns/op | 32.25 ns/op | 688 MB |
| `enwiki-latest-all-titles-in-ns0` | `fst` | 61.3 s | 28.6 s | 33.1 s | 327.2 s | 310.8 s | 16.83 s | 2555.44 ns/op | 34.95 ns/op | 715 MB |

The Naskitis `distinct_1` text corpus shows a similar tradeoff on a large compacted dictionary:

| Corpus | Profile | Insert | Find | Get | Compact | Save | Load-only | Find Loaded | Get Loaded | Internal After Load |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `distinct_1` | `compact` | 73.1 s | 41.1 s | 48.9 s | 232.0 s | 218.1 s | 15.09 s | 2828.26 ns/op | 33.36 ns/op | 830 MB |
| `distinct_1` | `marisa` | 72.9 s | 40.5 s | 49.3 s | 102.0 s | 93.0 s | 14.99 s | 954.74 ns/op | 33.95 ns/op | 856 MB |
| `distinct_1` | `fst` | 73.7 s | 42.2 s | 52.1 s | 237.1 s | 234.8 s | 24.59 s | 1743.71 ns/op | 34.38 ns/op | 802 MB |

The table above was produced with the dependency-backed benchmark binary:

```sh
./build-vcpkg/string_bimap_bench --csv /tmp/StockETFList --column Symbol --profile fast
./build-vcpkg/string_bimap_bench --csv /tmp/StockETFList --column Symbol --profile compact
./build-vcpkg/string_bimap_bench --csv /tmp/StockETFList --column Symbol --profile marisa
./build-vcpkg/string_bimap_bench --csv /tmp/StockETFList --column Symbol --profile fst
./build-vcpkg/string_bimap_bench --csv /tmp/StockETFList --column "Company Name" --profile fast
./build-vcpkg/string_bimap_bench --csv /tmp/StockETFList --column "Company Name" --profile compact
./build-vcpkg/string_bimap_bench --csv /tmp/StockETFList --column "Company Name" --profile marisa
./build-vcpkg/string_bimap_bench --csv /tmp/StockETFList --column "Company Name" --profile fst

./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column cusip --profile fast
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column cusip --profile compact
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column cusip --profile marisa
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column cusip --profile fst
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column symbol --profile fast
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column symbol --profile compact
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column symbol --profile marisa
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column symbol --profile fst
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column description --profile fast
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column description --profile compact
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column description --profile marisa
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column description --profile fst

./build-vcpkg/string_bimap_bench --csv /tmp/SEC_CIKs_Symbols.csv --column cik --profile compact
./build-vcpkg/string_bimap_bench --csv /tmp/SEC_CIKs_Symbols.csv --column cik --profile marisa
./build-vcpkg/string_bimap_bench --csv /tmp/SEC_CIKs_Symbols.csv --column cik --profile fst
./build-vcpkg/string_bimap_bench --csv /tmp/SEC_CIKs_Symbols.csv --column symbol --profile compact
./build-vcpkg/string_bimap_bench --csv /tmp/SEC_CIKs_Symbols.csv --column symbol --profile marisa
./build-vcpkg/string_bimap_bench --csv /tmp/SEC_CIKs_Symbols.csv --column symbol --profile fst
./build-vcpkg/string_bimap_bench --csv /tmp/SEC_CIKs_Symbols.csv --column Name --profile compact
./build-vcpkg/string_bimap_bench --csv /tmp/SEC_CIKs_Symbols.csv --column Name --profile marisa
./build-vcpkg/string_bimap_bench --csv /tmp/SEC_CIKs_Symbols.csv --column Name --profile fst
```

The Wikipedia rows were produced with:

```sh
./build-vcpkg/string_bimap_bench \
  --line-file /tmp/enwiki-latest-all-titles-in-ns0.keys.txt \
  --profile compact \
  --prefix A \
  --phases insert,find,get,erase,compact,save \
  --release-inputs-before-compact \
  --serialized-file /tmp/string_bimap_enwiki_xcdat.bin \
  --save-compacted \
  --shuffle \
  --seed 7

./build-vcpkg/string_bimap_bench \
  --line-file /tmp/enwiki-latest-all-titles-in-ns0.keys.txt \
  --profile marisa \
  --prefix A \
  --phases insert,find,get,erase,compact,save \
  --release-inputs-before-compact \
  --serialized-file /tmp/string_bimap_enwiki_marisa.bin \
  --save-compacted \
  --shuffle \
  --seed 7

./build-vcpkg/string_bimap_bench \
  --line-file /tmp/enwiki-latest-all-titles-in-ns0.keys.txt \
  --profile fst \
  --prefix A \
  --phases insert,find,get,erase,compact,save \
  --release-inputs-before-compact \
  --serialized-file /tmp/string_bimap_enwiki_fst.bin \
  --save-compacted \
  --shuffle \
  --seed 7

./build-vcpkg/string_bimap_bench \
  --profile compact \
  --phases load,find_loaded,get_loaded \
  --serialized-file /tmp/string_bimap_enwiki_xcdat.bin

./build-vcpkg/string_bimap_bench \
  --profile marisa \
  --phases load,find_loaded,get_loaded \
  --serialized-file /tmp/string_bimap_enwiki_marisa.bin

./build-vcpkg/string_bimap_bench \
  --profile fst \
  --phases load,find_loaded,get_loaded \
  --serialized-file /tmp/string_bimap_enwiki_fst.bin
```

For `keyvi`, the same Wikipedia command shape was attempted through `build-keyvi` with `--profile keyvi`, but the run was stopped after `4m19s` at roughly `7.0 GB RSS` without reaching the benchmark summary. That result was materially worse than the completed `marisa` and `xcdat` runs, so `keyvi` remains experimental and is not included in the main comparison tables.

The Naskitis `distinct_1` rows were produced with:

```sh
./build-vcpkg/string_bimap_bench \
  --line-file /tmp/naskitis/distinct_1 \
  --profile compact \
  --phases insert,find,get,compact,save \
  --release-inputs-before-compact \
  --serialized-file /tmp/string_bimap_naskitis_distinct1_xcdat.bin \
  --save-compacted

./build-vcpkg/string_bimap_bench \
  --line-file /tmp/naskitis/distinct_1 \
  --profile marisa \
  --phases insert,find,get,compact,save \
  --release-inputs-before-compact \
  --serialized-file /tmp/string_bimap_naskitis_distinct1_marisa.bin \
  --save-compacted

./build-vcpkg/string_bimap_bench \
  --line-file /tmp/naskitis/distinct_1 \
  --profile fst \
  --phases insert,find,get,compact,save \
  --release-inputs-before-compact \
  --serialized-file /tmp/string_bimap_naskitis_distinct1_fst.bin \
  --save-compacted

./build-vcpkg/string_bimap_bench \
  --profile compact \
  --phases load,find_loaded,get_loaded \
  --serialized-file /tmp/string_bimap_naskitis_distinct1_xcdat.bin

./build-vcpkg/string_bimap_bench \
  --profile marisa \
  --phases load,find_loaded,get_loaded \
  --serialized-file /tmp/string_bimap_naskitis_distinct1_marisa.bin

./build-vcpkg/string_bimap_bench \
  --profile fst \
  --phases load,find_loaded,get_loaded \
  --serialized-file /tmp/string_bimap_naskitis_distinct1_fst.bin
```

`keyvi` was also exercised on the same `distinct_1` compact/save pattern and showed the same negative trend as Wikipedia: substantially heavier and slower than `marisa` before completion, so it is not included in the main table.

Those runs assume the datasets have already been downloaded to `/tmp/StockETFList`, `/tmp/CUSIP.csv`, `/tmp/SEC_CIKs_Symbols.csv`, `/tmp/enwiki-latest-all-titles-in-ns0.keys.txt`, and `/tmp/naskitis/distinct_1`.

You can also benchmark a plain text corpus with one key per line:

```sh
./build-vcpkg/string_bimap_bench \
  --line-file /tmp/enwiki-latest-all-titles-in-ns0.keys.txt \
  --profile fast \
  --prefix "A" \
  --shuffle \
  --seed 7
```

`--prefix` selects the prefix used by the prefix-query metrics. `--shuffle` randomizes insertion order while keeping the run reproducible through `--seed`.

For Askitis-style workloads with separate write and read corpora, use:

```sh
./build-vcpkg/string_bimap_bench \
  --line-file-write /tmp/distinct_1.txt \
  --line-file-read /tmp/skew1_1.txt \
  --profile fast \
  --phases insert,find,get \
  --read-limit 1000000
```

The write corpus is deduplicated on load. The read corpus is not deduplicated, so repeated hot queries are preserved.

To reduce harness memory amplification on large corpora, you can limit the benchmark to specific phases and drop the input corpus before compaction:

```sh
./build-vcpkg/string_bimap_bench \
  --line-file /tmp/enwiki-latest-all-titles-in-ns0.keys.txt \
  --profile fast \
  --phases insert,find,get,erase,compact \
  --release-inputs-before-compact \
  --shuffle \
  --seed 7
```

Supported phase names are `insert`, `find`, `get`, `prefix`, `erase`, `compact`, `prefix_compact`, `save`, `load`, `find_loaded`, and `get_loaded`.
The benchmark also prints internal memory accounting for each phase, including base/delta arena bytes, entry-table bytes, index bytes, tombstones, and total estimated in-structure bytes. `xcdat` compact-index bytes come from its native byte-count API; `hat-trie` compact-index bytes are estimated from its serialized representation because the library does not expose a direct in-memory byte counter.

You can also split serialization and deserialization into separate benchmark runs with `--serialized-file`:

```sh
./build-vcpkg/string_bimap_bench \
  --line-file /tmp/enwiki-latest-all-titles-in-ns0.keys.txt \
  --profile fast \
  --phases insert,erase,compact,save \
  --release-inputs-before-compact \
  --serialized-file /tmp/string_bimap_enwiki.bin \
  --shuffle \
  --seed 7

./build-vcpkg/string_bimap_bench \
  --profile fast \
  --phases load \
  --serialized-file /tmp/string_bimap_enwiki.bin
```

If `load` is requested without `save` in the same run, `--serialized-file` is required.

For compact base backends, file-based persistence may create one of:

- `dict.bin.compact.xcdat`
- `dict.bin.compact.marisa`
- `dict.bin.compact.ids`

These sidecars store the native compact base and its local-to-global ID map. They are optional accelerators for `load(path)`. Stream `save(std::ostream&)` and `load(std::istream&)` remain logical-only and never use sidecars.

If you want a file save to always emit compact sidecars without mutating the original dictionary, use:

```cpp
dict.save_compacted("dict.bin");
```

or in the benchmark:

```sh
./build-vcpkg/string_bimap_bench \
  --line-file /tmp/enwiki-latest-all-titles-in-ns0.keys.txt \
  --profile compact \
  --phases insert,erase,save \
  --save-compacted \
  --serialized-file /tmp/string_bimap_enwiki_compact.bin
```

For post-load steady-state read benchmarking, you can add:

- `find_loaded`
- `get_loaded`

and control repetitions with `--read-repeats N`.

Example:

```sh
./build-vcpkg/string_bimap_bench \
  --profile compact \
  --phases load,find_loaded,get_loaded \
  --read-repeats 5 \
  --serialized-file /tmp/string_bimap_enwiki_compact.bin
```

## Example

```cpp
#include <string_bimap/string_bimap.hpp>

int main() {
    string_bimap::StringBimap dict(
        0, string_bimap::BackendProfile::FastLookup);

    auto apple = dict.insert("apple");
    auto banana = dict.insert("banana");

    auto found = dict.find_id("banana");
    auto value = dict.get_string(apple);

    dict.for_each_live([](string_bimap::StringId id, std::string_view value) {
        (void)id;
        (void)value;
    });

    dict.for_each_with_prefix("app", [](string_bimap::StringId id, std::string_view value) {
        (void)id;
        (void)value;
    });

    dict.for_each_with_prefix_unordered("app", [](string_bimap::StringId id, std::string_view value) {
        (void)id;
        (void)value;
    });

    dict.save("dict.bin");
    auto restored = string_bimap::StringBimap::load("dict.bin");

    dict.save_compacted("dict.compacted.bin");

    dict.erase(banana);
    dict.compact();
}
```
