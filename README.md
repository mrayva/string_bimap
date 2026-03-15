# string_bimap

Header-only C++17 string dictionary for:

- `string -> stable id`
- `id -> string`
- iteration over live `(id, string)` pairs
- prefix-query traversal over live entries
- mostly-static datasets with a mutable overlay
- binary save/load with stable IDs preserved

The design is:

- static base segment optimized for compact lookup, with optional `xcdat`
- mutable delta segment backed by a packed arena plus a `hat-trie` index
- exact tombstones keyed by stable global IDs

## Invariants

- IDs are assigned monotonically and are stable for the lifetime of the dictionary.
- IDs are never reused. If a string is deleted and later reinserted, it receives a new ID.
- Deleting a string creates a permanent logical hole in the ID space.
- `compact()` preserves all live IDs and only rewrites internal storage.
- `save()`/`load()` preserve the logical dictionary state, including stable live IDs and deleted holes.

## Lifetime And Safety Rules

- `get_string()` returns a `std::string_view` into internal storage.
- `for_each_live()` and `for_each_with_prefix()` also pass `std::string_view`s into internal storage.
- Any mutating operation, including `insert()`, `erase()`, `load()`, and `compact()`, may invalidate previously returned views.
- Do not mutate the dictionary from inside `for_each_live()` or `for_each_with_prefix()` callbacks.
- The library is not internally synchronized. External locking is required for concurrent access if mutation is possible.
- `get_string(id)` returns an empty view for a missing or deleted ID. That means an empty return value alone does not distinguish a stored empty string from an absent ID; use `contains_id(id)` when that distinction matters.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

If `xcdat.hpp` is available through your toolchain or include path, the build enables the `xcdat` static index automatically. Otherwise it falls back to a standard-library static index.

If `tsl/htrie_map.h` is available through your toolchain or include path, the build enables the `hat-trie` mutable delta index automatically. Otherwise it falls back to `std::unordered_map`.

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

## Example

```cpp
#include <string_bimap/string_bimap.hpp>

int main() {
    string_bimap::StringBimap dict;

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

    dict.save("dict.bin");
    auto restored = string_bimap::StringBimap::load("dict.bin");

    dict.erase(banana);
    dict.compact();
}
```
