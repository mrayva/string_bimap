# string_bimap

Header-only C++17 string dictionary for:

- `string -> stable id`
- `id -> string`
- live iteration and prefix scans
- mostly-static datasets with a mutable overlay
- binary save/load with stable IDs preserved
- deterministic runtime-built static bimaps for fixed vocabularies

## Profiles

Static base:

- `FastLookup`: fallback map-based indexing
- `CompactMemory`: optional `xcdat`
- `CompactMemoryMarisa`: optional `marisa-trie`

Mutable delta:

- `FastLookup`: `std::unordered_map`
- `FastLookupArrayMap`: optional `tsl::array_map`
- compact profiles: optional `hat-trie`
- `CompactMemoryMarisaArrayMap`: `marisa-trie` base plus `tsl::array_map` delta

Recommended starting points:

- `FastLookup`: simplest baseline for point-query-heavy use
- `CompactMemoryMarisaArrayMap`: the default serious profile for read-heavy use when you want a compact base plus a lean mutable overlay

Also keep `CompactMemory` (`xcdat`) in the evaluation set. It remains a first-class supported profile and is still useful for some short-key exact-lookup and footprint cases.

Other useful profiles:

- `FastLookupArrayMap`: stronger mutable-layer profile when lookup and memory matter more than insert speed, but without a compact base
- `CompactMemoryMarisa`: strongest compact static backend if you want to evaluate the base without the `array_map` delta variant

Example:

```cpp
string_bimap::StringBimap fast(0, string_bimap::BackendProfile::FastLookup);
string_bimap::StringBimap array_map(0, string_bimap::BackendProfile::FastLookupArrayMap);
string_bimap::StringBimap compact(0, string_bimap::BackendProfile::CompactMemory);
string_bimap::StringBimap marisa(0, string_bimap::BackendProfile::CompactMemoryMarisa);
string_bimap::StringBimap marisa_array_map(
    0, string_bimap::BackendProfile::CompactMemoryMarisaArrayMap);
```

If optional dependencies are not compiled in, compact profiles fall back to the standard map-based structures for the missing parts.

In practice, start with:

- `FastLookup` if simplicity and raw point-query speed dominate
- `CompactMemoryMarisaArrayMap` if this is a real deployment candidate and you care about steady-state memory

## Invariants

- IDs are monotonic and never reused.
- Deleting a string creates a permanent logical hole.
- `compact()` preserves all live IDs.
- Empty strings are ignored and never stored.
- `save()`/`load()` preserve logical state and the selected backend profile.
- `save(path)` writes native sidecars for the current in-memory state.
- `load(path)` prefers native sidecars and falls back to logical rebuild.
- Stream `save(std::ostream&)` / `load(std::istream&)` remain logical-only.

## Lifetime And Safety

- `get_string()` returns a `std::string_view` into internal storage.
- `try_get_string()` returns `std::nullopt` for missing or deleted IDs.
- Any mutation, `load()`, or `compact()` may invalidate previously returned views.
- Do not mutate the dictionary from inside iteration callbacks.
- `for_each_with_prefix()` yields stable ID order.
- `for_each_with_prefix_unordered()` may use backend/native order.
- The library is not internally synchronized.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Optional backends:

- `xcdat.hpp` enables `CompactMemory`
- `marisa.h` + `libmarisa` enable `CompactMemoryMarisa`
- `tsl/htrie_map.h` enables compact-profile trie prefix indexing
- `tsl/array_map.h` enables `FastLookupArrayMap` and `CompactMemoryMarisaArrayMap`
- a local `pthash` checkout enables `string_bimap::PthashBimap`

For `array-hash`, point CMake at a local checkout if needed:

```sh
cmake -S . -B build-array \
  -DSTRING_BIMAP_USE_ARRAY_HASH=ON \
  -DSTRING_BIMAP_ARRAY_HASH_ROOT=/path/to/array-hash
```

For `pthash`, point CMake at a local checkout with hydrated submodules:

```sh
git -C /path/to/pthash submodule update --init --recursive
cmake -S . -B build-pthash \
  -DSTRING_BIMAP_USE_PTHASH=ON \
  -DSTRING_BIMAP_PTHASH_ROOT=/path/to/pthash
cmake --build build-pthash
ctest --test-dir build-pthash --output-on-failure
```

## Build With vcpkg

The repository includes local overlay ports for `xcdat` and `hat-trie`.

```sh
cmake -S . -B build-vcpkg \
  -DCMAKE_TOOLCHAIN_FILE=/home/mrayva/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DSTRING_BIMAP_USE_XCDAT=ON \
  -DSTRING_BIMAP_USE_MARISA=ON \
  -DSTRING_BIMAP_USE_HAT_TRIE=ON
cmake --build build-vcpkg
ctest --test-dir build-vcpkg --output-on-failure
```

## PthashBimap

`string_bimap::PthashBimap` is a separate static bimap for small or medium fixed
string vocabularies when:

- startup is rare
- `string -> id` lookup speed matters more than build time
- you want deterministic dense IDs without compile-time enums

Properties:

- canonicalizes input by sorting and deduplicating it
- builds a deterministic runtime MPHF with `pthash`
- assigns the same dense IDs for the same string set on every session build
- adapts the compact ID width to `uint8_t`, `uint16_t`, or `uint32_t`
- verifies membership by checking `id_to_string[id] == key`

Example:

```cpp
#include <string_bimap/pthash_bimap.hpp>

std::vector<std::string> values = {
    "Bond",
    "Common Stock",
    "Depositary Receipt",
    "ETF",
    "Mutual Fund",
    "RIGHT",
};

string_bimap::PthashBimap bimap(values);

auto id = bimap.find("ETF");
auto compact = bimap.find_compact("ETF");
std::string_view value = bimap.by_id(*id);
```

The class also supports logical `save()` / `load()` round-trips. The serialized
form stores the chosen seed and rebuilds the MPHF deterministically on stream load.

For file-based persistence, `save(path)` also writes a native `pthash` sidecar:

- `path + ".native.pthash"`

and `load(path)` prefers that native sidecar before falling back to deterministic
rebuild from the logical main file.

It also supports deterministic initialization from common vocabulary file formats:

```cpp
auto csv_bimap = string_bimap::PthashBimap::from_csv_file(
    "/tmp/CUSIP.csv", "description");

auto json_bimap = string_bimap::PthashBimap::from_json_array_file(
    "/tmp/securityType.json");
```

Supported inputs:

- CSV files, by header name or column index
- JSON arrays of strings, including payloads shaped like the OpenFIGI
  `mapping/values/securityType` response once saved to disk

## Benchmark

Build:

```sh
cmake --build build-vcpkg --target string_bimap_bench
```

Examples:

```sh
./build-vcpkg/string_bimap_bench --profile fast 20000 256 12345
./build-vcpkg/string_bimap_bench --profile marisa 20000 256 12345
./build-vcpkg/string_bimap_bench --profile marisa_array_map 20000 256 12345
```

For a reproducible matrix over the standard corpora found in `/tmp`:

```sh
./bench/run_benchmark_matrix.sh build-vcpkg bench/results
./bench/summarize_benchmark_matrix.sh bench/results
```

The benchmark reports:

- insert / exact lookup / reverse lookup
- prefix traversal
- compaction and `compact_if_needed`
- mixed-state vs compacted snapshot save/load
- steady-state post-load read loops
- RSS and internal memory accounting
- native sidecar sizes

The summary script emits:

- `bench/results/summary.csv`
- `bench/results/summary.md`

If `pthash` is enabled, there is also a dedicated static-vocabulary benchmark:

```sh
./bench/fetch_openfigi_values.sh /tmp/openfigi-values
./build-pthash/string_bimap_pthash_bench --json-array-file /tmp/openfigi-values/securityType.json
./build-pthash/string_bimap_pthash_bench --json-array-file /tmp/openfigi-values/securityType2.json
./build-pthash/string_bimap_pthash_bench --json-array-file /tmp/openfigi-values/exchCode.json
```

That benchmark compares:

- `PthashBimap`
- sorted vector + binary search
- `std::unordered_map`

## Current Read

- `FastLookup` is still the default recommendation for the simplest point-query-heavy setup.
- `CompactMemoryMarisaArrayMap` is the default serious profile to test first for a real read-heavy deployment.
- `FastLookupArrayMap` materially reduces mutable-index memory and often improves read-heavy delta lookup.
- `CompactMemoryMarisa` is the best overall compact backend on the benchmarked corpora.
- `CompactMemory` (`xcdat`) remains a first-class profile and is still worth checking on short identifier-like keys where it can be slightly smaller.

Datasets used in this repo’s benchmark runs include:

- `/tmp/StockETFList`
- `/tmp/CUSIP.csv`
- `/tmp/SEC_CIKs_Symbols.csv`
- `/tmp/enwiki-latest-all-titles-in-ns0.keys.txt`
- `/tmp/naskitis/distinct_1`
- `/tmp/naskitis/skew1_1`

## Maintenance

- [CHANGELOG.md](/home/mrayva/fsst_plus/CHANGELOG.md) tracks repository-level changes.
- The benchmark scripts live in [bench](/home/mrayva/fsst_plus/bench).
