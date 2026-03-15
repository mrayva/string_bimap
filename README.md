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
- mutable delta segment backed by a packed arena plus:
  - `FastLookup`: `std::unordered_map`
  - `CompactMemory`: optional `hat-trie` when compiled in
- exact tombstones keyed by stable global IDs

The backend is selected per dictionary:

```cpp
string_bimap::StringBimap fast(0, string_bimap::BackendProfile::FastLookup);
string_bimap::StringBimap compact(0, string_bimap::BackendProfile::CompactMemory);
```

If optional dependencies are not compiled in, `CompactMemory` degrades gracefully to the fallback structures for the missing parts.

## Invariants

- IDs are assigned monotonically and are stable for the lifetime of the dictionary.
- IDs are never reused. If a string is deleted and later reinserted, it receives a new ID.
- Deleting a string creates a permanent logical hole in the ID space.
- `compact()` preserves all live IDs and only rewrites internal storage.
- `save()`/`load()` preserve the logical dictionary state, including stable live IDs and deleted holes.
- `save()`/`load()` preserve the selected backend profile.
- `save(path)` may emit compact sidecars for `BackendProfile::CompactMemory` when the on-disk snapshot is fully compacted.
- `load(path)` will use those sidecars when present; otherwise it falls back to logical rebuild and may generate the sidecars for later loads.

## Lifetime And Safety Rules

- `get_string()` returns a `std::string_view` into internal storage.
- `for_each_live()` and `for_each_with_prefix()` also pass `std::string_view`s into internal storage.
- Any mutating operation, including `insert()`, `erase()`, `load()`, and `compact()`, may invalidate previously returned views.
- Do not mutate the dictionary from inside `for_each_live()` or `for_each_with_prefix()` callbacks.
- `for_each_with_prefix()` returns results in stable ID order.
- `for_each_with_prefix_unordered()` may return results in backend/native order and is intended for faster prefix scans.
- The library is not internally synchronized. External locking is required for concurrent access if mutation is possible.
- `get_string(id)` returns an empty view for a missing or deleted ID. That means an empty return value alone does not distinguish a stored empty string from an absent ID; use `contains_id(id)` when that distinction matters.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

If `xcdat.hpp` is available through your toolchain or include path, the build enables the optional compact-memory static backend. Otherwise that profile falls back to the standard-library static index.

If `tsl/htrie_map.h` is available through your toolchain or include path, the build enables the optional compact-memory delta backend. Otherwise that profile falls back to `std::unordered_map`.

## Build With vcpkg

The repository includes local overlay ports for `xcdat` and `hat-trie` under `vcpkg_ports/`.

```sh
cmake -S . -B build-vcpkg \
  -DCMAKE_TOOLCHAIN_FILE=/home/mrayva/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DSTRING_BIMAP_USE_XCDAT=ON \
  -DSTRING_BIMAP_USE_HAT_TRIE=ON
cmake --build build-vcpkg
ctest --test-dir build-vcpkg --output-on-failure
```

This path installs both dependencies through the manifest and enables the real `xcdat` and `hat-trie` code paths.

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
```

The benchmark reports bulk insert, exact lookup, reverse lookup, prefix traversal, compaction, save, and load timings on a synthetic prefix-heavy dataset. It also prints both the compiled-in optional backends and the selected runtime profile.

### Current Summary

On the benchmarked stock-style datasets so far, `FastLookup` is the default recommendation for point-query-heavy workloads. `CompactMemory` is mainly justified when steady-state memory matters more than insert/compact/load latency.

The table below summarizes the current real-dataset results from the dependency-backed build (`xcdat + hat-trie` enabled). Times are nanoseconds per operation; memory is the internal post-load estimate reported by `memory_usage()`.

| Dataset | Column | Profile | Insert ns/op | Find ns/op | Get ns/op | Compact ns/op | Load ns/op | Internal After Load |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `StockETFList` | `Symbol` | `fast` | 733.7 | 207.8 | 227.0 | 1061.6 | 984.8 | 2.55 MB |
| `StockETFList` | `Symbol` | `compact` | 1662.4 | 374.2 | 394.8 | 3491.3 | 3391.9 | 0.76 MB |
| `StockETFList` | `Company Name` | `fast` | 843.3 | 315.9 | 316.0 | 1345.2 | 1162.3 | 4.20 MB |
| `StockETFList` | `Company Name` | `compact` | 1855.4 | 427.8 | 492.5 | 8985.6 | 8725.2 | 2.79 MB |
| `CUSIP.csv` | `cusip` | `fast` | 721.46 | 233.72 | 266.03 | 1158.54 | 1092.01 | 4.32 MB |
| `CUSIP.csv` | `cusip` | `compact` | 1534.01 | 402.67 | 453.57 | 6213.97 | 5613.43 | 1.54 MB |
| `CUSIP.csv` | `symbol` | `fast` | 768.97 | 265.62 | 264.38 | 1163.94 | 1588.19 | 4.10 MB |
| `CUSIP.csv` | `symbol` | `compact` | 1605.37 | 407.96 | 532.04 | 4981.51 | 4419.81 | 1.26 MB |
| `CUSIP.csv` | `description` | `fast` | 945.83 | 449.80 | 514.25 | 1425.14 | 1719.44 | 5.79 MB |
| `CUSIP.csv` | `description` | `compact` | 1784.17 | 530.81 | 569.33 | 7252.44 | 6992.30 | 3.19 MB |

At this point the tradeoff is consistent:

- `FastLookup` wins clearly on insert, exact lookup, compaction, and load latency.
- `CompactMemory` reduces steady-state memory, sometimes substantially for shorter identifiers.
- If point queries are the dominant operation, start with `FastLookup`.
- If memory pressure is more important than write/load cost, evaluate `CompactMemory` on your real corpus.

The table above was produced with the dependency-backed benchmark binary:

```sh
./build-vcpkg/string_bimap_bench --csv /tmp/StockETFList --column Symbol --profile fast
./build-vcpkg/string_bimap_bench --csv /tmp/StockETFList --column Symbol --profile compact
./build-vcpkg/string_bimap_bench --csv /tmp/StockETFList --column "Company Name" --profile fast
./build-vcpkg/string_bimap_bench --csv /tmp/StockETFList --column "Company Name" --profile compact

./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column cusip --profile fast
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column cusip --profile compact
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column symbol --profile fast
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column symbol --profile compact
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column description --profile fast
./build-vcpkg/string_bimap_bench --csv /tmp/CUSIP.csv --column description --profile compact
```

Those runs assume the datasets have already been downloaded to `/tmp/StockETFList` and `/tmp/CUSIP.csv`.

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

For `BackendProfile::CompactMemory`, file-based persistence may create:

- `dict.bin.compact.xcdat`
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
