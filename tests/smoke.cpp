#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <string_bimap/string_bimap.hpp>

namespace {

using string_bimap::StringBimap;
using string_bimap::BackendProfile;
using string_bimap::StringId;

[[nodiscard]] std::string make_string(std::mt19937& rng, std::size_t max_len = 24) {
    static constexpr char alphabet[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789_-:/.";

    std::uniform_int_distribution<std::size_t> length_dist(1, max_len);
    std::uniform_int_distribution<std::size_t> char_dist(0, sizeof(alphabet) - 2);

    const std::size_t len = length_dist(rng);
    std::string out;
    out.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(alphabet[char_dist(rng)]);
    }
    return out;
}

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test failure: " << message << '\n';
        std::abort();
    }
}

[[nodiscard]] std::string compact_sidecar_path(BackendProfile profile, const std::string& path) {
    switch (profile) {
        case BackendProfile::CompactMemory:
            return path + ".compact.xcdat";
        case BackendProfile::CompactMemoryMarisa:
        case BackendProfile::CompactMemoryMarisaArrayMap:
            return path + ".compact.marisa";
        case BackendProfile::CompactMemoryMarisaFsst:
            return {};
        case BackendProfile::CompactMemoryKeyvi:
            return path + ".compact.keyvi";
        case BackendProfile::CompactMemoryFst:
            return path + ".compact.fst";
        case BackendProfile::FastLookupArrayMap:
            return {};
        case BackendProfile::FastLookup:
            return {};
    }
    return {};
}

[[nodiscard]] bool compact_backend_uses_ids_sidecar(BackendProfile profile) {
    return profile == BackendProfile::CompactMemory ||
           profile == BackendProfile::CompactMemoryMarisa ||
           profile == BackendProfile::CompactMemoryMarisaArrayMap;
}

template <class Fn>
void for_each_profile(Fn&& fn) {
    fn(BackendProfile::FastLookup);
    fn(BackendProfile::FastLookupArrayMap);
    fn(BackendProfile::CompactMemory);
    fn(BackendProfile::CompactMemoryMarisa);
    fn(BackendProfile::CompactMemoryMarisaArrayMap);
#if defined(STRING_BIMAP_HAS_FSST)
    fn(BackendProfile::CompactMemoryMarisaFsst);
#endif
    fn(BackendProfile::CompactMemoryKeyvi);
    fn(BackendProfile::CompactMemoryFst);
}

void test_empty_strings_are_ignored(BackendProfile profile) {
    StringBimap dict(0, profile);

    expect(!dict.find_id("").has_value(), "empty string should never be found");
    expect(!dict.contains(""), "empty string should never be contained");
    expect(dict.insert("") == string_bimap::kInvalidId, "empty insert should return invalid id");
    expect(!dict.contains_id(string_bimap::kInvalidId), "invalid id should not become live");
    expect(!dict.erase(""), "erase empty string should fail");
    expect(dict.live_size() == 0, "empty string should not change live size");

    dict.compact();

    expect(!dict.find_id("").has_value(), "empty string should remain absent after compaction");
}

void test_basic_insert_erase_compact(BackendProfile profile) {
    StringBimap dict(0, profile);
    expect(dict.backend_profile() == profile, "backend profile should round-trip through construction");

    const auto apple_id = dict.insert("apple");
    const auto banana_id = dict.insert("banana");
    const auto duplicate_apple = dict.insert("apple");

    expect(apple_id == duplicate_apple, "duplicate insert should reuse id");
    expect(dict.contains("apple"), "apple should exist");
    {
        const auto id = dict.find_id("banana");
        expect(id.has_value() && *id == banana_id, "banana id mismatch");
    }
    expect(dict.get_string(apple_id) == std::string_view("apple"), "apple value mismatch");
    expect(dict.live_size() == 2, "live size should count unique live strings");

    expect(dict.erase(apple_id), "erase by id should succeed");
    expect(!dict.contains("apple"), "apple should be absent after erase");
    expect(dict.get_string(apple_id).empty(), "erased id should decode to empty view");
    expect(dict.live_size() == 1, "live size should shrink after erase");

    const auto carrot_id = dict.insert("carrot");
    expect(dict.contains("carrot"), "carrot should exist");
    expect(dict.get_string(carrot_id) == std::string_view("carrot"), "carrot value mismatch");

    dict.compact();

    {
        const auto id = dict.find_id("banana");
        expect(id.has_value() && *id == banana_id, "banana id should survive compaction");
    }
    expect(dict.get_string(banana_id) == std::string_view("banana"), "banana value should survive compaction");
    {
        const auto id = dict.find_id("carrot");
        expect(id.has_value() && *id == carrot_id, "carrot id should survive compaction");
    }
    expect(dict.get_string(carrot_id) == std::string_view("carrot"), "carrot value should survive compaction");
    expect(!dict.contains("apple"), "apple should remain absent after compaction");
    expect(dict.live_size() == 2, "live size should remain correct after compaction");
}

void test_delete_reinsert_gets_new_id(BackendProfile profile) {
    StringBimap dict(0, profile);

    const auto id1 = dict.insert("alpha");
    expect(dict.erase("alpha"), "erase by value should succeed");
    expect(!dict.contains("alpha"), "alpha should be absent after erase");

    const auto id2 = dict.insert("alpha");
    expect(id2 != id1, "reinserted key should get a new stable id");
    expect(dict.get_string(id1).empty(), "old id should remain tombstoned");
    expect(dict.get_string(id2) == std::string_view("alpha"), "new id should decode");

    dict.compact();

    {
        const auto id = dict.find_id("alpha");
        expect(id.has_value() && *id == id2, "latest id should survive compaction");
    }
    expect(dict.get_string(id1).empty(), "old tombstoned id should remain absent after compaction");
    expect(dict.get_string(id2) == std::string_view("alpha"), "new id should still decode after compaction");
}

void test_contains_id_and_invalid_erases(BackendProfile profile) {
    StringBimap dict(0, profile);

    const auto a = dict.insert("a");
    const auto b = dict.insert("b");

    expect(dict.contains_id(a), "a id should exist");
    expect(dict.contains_id(b), "b id should exist");
    expect(!dict.contains_id(12345), "arbitrary invalid id should not exist");
    expect(!dict.erase(12345), "erase invalid id should fail");

    expect(dict.erase(a), "erase a should succeed");
    expect(!dict.erase(a), "erase same id twice should fail");
    expect(!dict.contains_id(a), "erased id should not be contained");
    expect(dict.contains_id(b), "other id should stay live");
}

void test_compaction_preserves_live_ids_over_many_cycles(BackendProfile profile) {
    StringBimap dict(0, profile);
    std::unordered_map<std::string, StringId> ids;

    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 25; ++i) {
            const auto key = "k_" + std::to_string(round) + "_" + std::to_string(i);
            const auto id = dict.insert(key);
            ids.emplace(key, id);
        }

        for (int i = 0; i < 25; i += 3) {
            const auto key = "k_" + std::to_string(round) + "_" + std::to_string(i);
            expect(dict.erase(key), "scheduled erase should succeed");
            ids.erase(key);
        }

        dict.compact();

        for (const auto& [key, id] : ids) {
            {
                const auto found = dict.find_id(key);
                expect(found.has_value() && *found == id, "live id should remain stable across compaction");
            }
            expect(dict.get_string(id) == key, "live string should remain decodable across compaction");
        }
    }
}

void test_randomized_model(BackendProfile profile) {
    std::mt19937 rng(0xC0FFEEu);
    StringBimap dict(0, profile);

    std::unordered_map<std::string, StringId> live_by_value;
    std::unordered_map<StringId, std::string> live_by_id;
    std::unordered_set<StringId> deleted_ids;
    std::vector<StringId> all_ids;

    for (int step = 0; step < 2000; ++step) {
        std::uniform_int_distribution<int> op_dist(0, 99);
        const int op = op_dist(rng);

        if (op < 55) {
            auto key = make_string(rng);
            const auto before = live_by_value.find(key);
            const auto id = dict.insert(key);

            if (before != live_by_value.end()) {
                expect(id == before->second, "duplicate live insert should reuse id");
            } else {
                live_by_value[key] = id;
                live_by_id[id] = key;
                all_ids.push_back(id);
                deleted_ids.erase(id);
            }
        } else if (op < 75) {
            if (!live_by_id.empty()) {
                std::uniform_int_distribution<std::size_t> pick(0, live_by_id.size() - 1);
                auto it = live_by_id.begin();
                std::advance(it, static_cast<std::ptrdiff_t>(pick(rng)));

                const auto id = it->first;
                const auto key = it->second;
                expect(dict.erase(id), "erase by id from live set should succeed");
                live_by_value.erase(key);
                live_by_id.erase(id);
                deleted_ids.insert(id);
            } else {
                expect(!dict.erase(static_cast<StringId>(0)), "erase on empty live set should fail");
            }
        } else if (op < 90) {
            if (!live_by_value.empty()) {
                std::uniform_int_distribution<std::size_t> pick(0, live_by_value.size() - 1);
                auto it = live_by_value.begin();
                std::advance(it, static_cast<std::ptrdiff_t>(pick(rng)));

                const auto key = it->first;
                const auto id = it->second;
                expect(dict.erase(key), "erase by value from live set should succeed");
                live_by_id.erase(id);
                live_by_value.erase(it);
                deleted_ids.insert(id);
            } else {
                expect(!dict.erase(std::string_view("missing")), "erase missing value should fail");
            }
        } else {
            dict.compact();
        }

        expect(dict.live_size() == live_by_value.size(), "live size should match model");

        for (const auto& [key, id] : live_by_value) {
            const auto found = dict.find_id(key);
            expect(found.has_value(), "live key should be found");
            expect(*found == id, "live key id should match model");
            expect(dict.contains(key), "contains should agree with model");
        }

        for (const auto& [id, key] : live_by_id) {
            expect(dict.contains_id(id), "live id should exist");
            expect(dict.get_string(id) == key, "live id should decode correctly");
        }

        for (const auto id : deleted_ids) {
            expect(!dict.contains_id(id), "deleted id should not exist");
            expect(dict.get_string(id).empty(), "deleted id should decode to empty");
        }
    }
}

void test_serialization_round_trip_stream(BackendProfile profile) {
    StringBimap dict(0, profile);

    const auto alpha_id = dict.insert("alpha");
    const auto beta_id = dict.insert("beta");
    const auto gamma_id = dict.insert("gamma");
    expect(dict.erase(beta_id), "erase before serialization should succeed");
    dict.compact();
    const auto delta_id = dict.insert("delta");

    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    dict.save(buffer);
    buffer.seekg(0);

    auto restored = StringBimap::load(buffer);

    expect(restored.backend_profile() == profile, "serialized profile should round-trip");
    expect(restored.size() == dict.size(), "serialized next_id should round-trip");
    expect(restored.live_size() == dict.live_size(), "serialized live size should round-trip");
    {
        const auto id = restored.find_id("alpha");
        expect(id.has_value() && *id == alpha_id, "alpha id should round-trip");
    }
    {
        const auto id = restored.find_id("gamma");
        expect(id.has_value() && *id == gamma_id, "gamma id should round-trip");
    }
    {
        const auto id = restored.find_id("delta");
        expect(id.has_value() && *id == delta_id, "delta id should round-trip");
    }
    expect(!restored.contains("beta"), "deleted key should remain absent after load");
    expect(!restored.contains_id(beta_id), "deleted id hole should remain absent after load");
}

void test_serialization_round_trip_file(BackendProfile profile) {
    StringBimap dict(0, profile);

    std::unordered_map<std::string, StringId> ids;
    for (int i = 0; i < 40; ++i) {
        const auto key = "file_" + std::to_string(i);
        ids.emplace(key, dict.insert(key));
    }

    for (int i = 0; i < 40; i += 4) {
        const auto key = "file_" + std::to_string(i);
        expect(dict.erase(key), "scheduled erase before file serialization should succeed");
        ids.erase(key);
    }

    dict.compact();
    const auto tail_id = dict.insert("tail");
    ids.emplace("tail", tail_id);

    const std::string path = "/tmp/string_bimap_roundtrip.bin";
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".compact.xcdat");
    std::filesystem::remove(path + ".compact.marisa");
    std::filesystem::remove(path + ".compact.keyvi");
    std::filesystem::remove(path + ".compact.fst");
    std::filesystem::remove(path + ".compact.ids");
    dict.save(path);
    auto restored = StringBimap::load(path);

    expect(restored.backend_profile() == profile, "file round-trip should preserve profile");
    expect(restored.size() == dict.size(), "file round-trip should preserve next_id");
    expect(restored.live_size() == ids.size(), "file round-trip should preserve live count");
    for (const auto& [key, id] : ids) {
        {
            const auto found = restored.find_id(key);
            expect(found.has_value() && *found == id, "file round-trip should preserve ids");
        }
        expect(restored.get_string(id) == key, "file round-trip should preserve decoded values");
    }

    std::filesystem::remove(path);
    std::filesystem::remove(path + ".compact.xcdat");
    std::filesystem::remove(path + ".compact.marisa");
    std::filesystem::remove(path + ".compact.keyvi");
    std::filesystem::remove(path + ".compact.fst");
    std::filesystem::remove(path + ".compact.ids");
}

void test_compact_native_sidecars() {
#if defined(STRING_BIMAP_HAS_XCDAT) || defined(STRING_BIMAP_HAS_MARISA)
    auto run = [](BackendProfile profile) {
        StringBimap dict(0, profile);
        const auto alpha = dict.insert("alpha");
        const auto beta = dict.insert("beta");
        dict.compact();

        const std::string path = "/tmp/string_bimap_compact_native.bin";
        const std::string trie_path = compact_sidecar_path(profile, path);
        const std::string ids_path = path + ".compact.ids";
        std::filesystem::remove(path);
        std::filesystem::remove(path + ".compact.xcdat");
        std::filesystem::remove(path + ".compact.marisa");
        std::filesystem::remove(path + ".compact.keyvi");
        std::filesystem::remove(path + ".compact.fst");
        std::filesystem::remove(ids_path);

        dict.save(path);

        expect(std::filesystem::exists(trie_path), "compact trie sidecar should be written");
        if (compact_backend_uses_ids_sidecar(profile)) {
            expect(std::filesystem::exists(ids_path), "compact id sidecar should be written");
        } else {
            expect(!std::filesystem::exists(ids_path), "compact fst backend should not emit id sidecar");
        }

        auto restored = StringBimap::load(path);
        {
            const auto id = restored.find_id("alpha");
            expect(id.has_value() && *id == alpha, "native compact load should preserve alpha");
        }
        {
            const auto id = restored.find_id("beta");
            expect(id.has_value() && *id == beta, "native compact load should preserve beta");
        }

        std::filesystem::remove(path);
        std::filesystem::remove(path + ".compact.xcdat");
        std::filesystem::remove(path + ".compact.marisa");
        std::filesystem::remove(path + ".compact.keyvi");
        std::filesystem::remove(path + ".compact.fst");
        std::filesystem::remove(ids_path);
    };
#if defined(STRING_BIMAP_HAS_XCDAT)
    run(BackendProfile::CompactMemory);
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
    run(BackendProfile::CompactMemoryMarisa);
#endif
#if defined(STRING_BIMAP_HAS_KEYVI)
    run(BackendProfile::CompactMemoryKeyvi);
#endif
    run(BackendProfile::CompactMemoryFst);
#endif
}

void test_save_compacted_preserves_ids_and_sidecars() {
#if defined(STRING_BIMAP_HAS_XCDAT) || defined(STRING_BIMAP_HAS_MARISA)
    auto run = [](BackendProfile profile) {
        StringBimap dict(0, profile);
        const auto alpha = dict.insert("alpha");
        const auto beta = dict.insert("beta");
        expect(dict.erase(beta), "beta erase before save_compacted should succeed");
        const auto gamma = dict.insert("gamma");

        const std::string path = "/tmp/string_bimap_save_compacted.bin";
        const std::string trie_path = compact_sidecar_path(profile, path);
        const std::string ids_path = path + ".compact.ids";
        std::filesystem::remove(path);
        std::filesystem::remove(path + ".compact.xcdat");
        std::filesystem::remove(path + ".compact.marisa");
        std::filesystem::remove(path + ".compact.keyvi");
        std::filesystem::remove(path + ".compact.fst");
        std::filesystem::remove(ids_path);

        dict.save_compacted(path);

        expect(std::filesystem::exists(trie_path), "save_compacted should emit compact trie sidecar");
        if (compact_backend_uses_ids_sidecar(profile)) {
            expect(std::filesystem::exists(ids_path), "save_compacted should emit compact id sidecar");
        } else {
            expect(!std::filesystem::exists(ids_path), "save_compacted fst backend should not emit id sidecar");
        }

        auto restored = StringBimap::load(path);
        {
            const auto id = restored.find_id("alpha");
            expect(id.has_value() && *id == alpha, "save_compacted should preserve alpha id");
        }
        {
            const auto id = restored.find_id("gamma");
            expect(id.has_value() && *id == gamma, "save_compacted should preserve gamma id");
        }
        expect(!restored.contains("beta"), "save_compacted should preserve deleted beta");
        expect(!restored.contains_id(beta), "save_compacted should preserve deleted beta hole");

        std::filesystem::remove(path);
        std::filesystem::remove(path + ".compact.xcdat");
        std::filesystem::remove(path + ".compact.marisa");
        std::filesystem::remove(path + ".compact.keyvi");
        std::filesystem::remove(path + ".compact.fst");
        std::filesystem::remove(ids_path);
    };
#if defined(STRING_BIMAP_HAS_XCDAT)
    run(BackendProfile::CompactMemory);
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
    run(BackendProfile::CompactMemoryMarisa);
#endif
#if defined(STRING_BIMAP_HAS_KEYVI)
    run(BackendProfile::CompactMemoryKeyvi);
#endif
    run(BackendProfile::CompactMemoryFst);
#endif
}

void test_iteration_api(BackendProfile profile) {
    StringBimap dict(0, profile);

    const auto alpha = dict.insert("alpha");
    const auto beta = dict.insert("beta");
    expect(dict.erase(beta), "erase before iteration should succeed");
    const auto gamma = dict.insert("gamma");

    std::vector<std::pair<StringId, std::string>> seen;
    dict.for_each_live([&](StringId id, std::string_view value) {
        seen.emplace_back(id, std::string(value));
    });

    expect(seen.size() == 2, "iteration should visit only live entries");
    expect((seen == std::vector<std::pair<StringId, std::string>>{
                        {alpha, "alpha"},
                        {gamma, "gamma"},
                    }),
           "iteration should be in stable id order");

    dict.compact();

    std::vector<std::pair<StringId, std::string>> after_compact;
    dict.for_each_live([&](StringId id, std::string_view value) {
        after_compact.emplace_back(id, std::string(value));
    });
    expect(after_compact == seen, "iteration should be stable across compaction");

    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    dict.save(buffer);
    buffer.seekg(0);
    auto restored = StringBimap::load(buffer);
    expect(restored.backend_profile() == profile, "iteration load should preserve profile");

    std::vector<std::pair<StringId, std::string>> after_load;
    restored.for_each_live([&](StringId id, std::string_view value) {
        after_load.emplace_back(id, std::string(value));
    });
    expect(after_load == seen, "iteration should round-trip through serialization");
}

void test_prefix_query_api(BackendProfile profile) {
    StringBimap dict(0, profile);

    const auto app = dict.insert("app");
    const auto apple = dict.insert("apple");
    const auto apricot = dict.insert("apricot");
    const auto banana = dict.insert("banana");
    expect(dict.erase(apricot), "erase before prefix query should succeed");

    std::vector<std::pair<StringId, std::string>> seen;
    dict.for_each_with_prefix("app", [&](StringId id, std::string_view value) {
        seen.emplace_back(id, std::string(value));
    });
    expect((seen == std::vector<std::pair<StringId, std::string>>{
                        {app, "app"},
                        {apple, "apple"},
                    }),
           "prefix query should return live matches in stable id order");

    std::vector<std::pair<StringId, std::string>> all_seen;
    dict.for_each_with_prefix("", [&](StringId id, std::string_view value) {
        all_seen.emplace_back(id, std::string(value));
    });
    expect((all_seen == std::vector<std::pair<StringId, std::string>>{
                            {app, "app"},
                            {apple, "apple"},
                            {banana, "banana"},
                        }),
           "empty prefix should visit all live entries");

    dict.compact();

    std::vector<std::pair<StringId, std::string>> after_compact;
    dict.for_each_with_prefix("app", [&](StringId id, std::string_view value) {
        after_compact.emplace_back(id, std::string(value));
    });
    expect(after_compact == seen, "prefix query should be stable across compaction");

    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    dict.save(buffer);
    buffer.seekg(0);
    auto restored = StringBimap::load(buffer);
    expect(restored.backend_profile() == profile, "prefix load should preserve profile");

    std::vector<std::pair<StringId, std::string>> after_load;
    restored.for_each_with_prefix("app", [&](StringId id, std::string_view value) {
        after_load.emplace_back(id, std::string(value));
    });
    expect(after_load == seen, "prefix query should round-trip through serialization");

    std::vector<std::pair<StringId, std::string>> unordered_seen;
    restored.for_each_with_prefix_unordered("app", [&](StringId id, std::string_view value) {
        unordered_seen.emplace_back(id, std::string(value));
    });
    std::sort(unordered_seen.begin(), unordered_seen.end());
    expect(unordered_seen == seen, "unordered prefix query should return the same live matches");
}

void test_backend_profile_explicit_selection() {
    StringBimap fast(0, BackendProfile::FastLookup);
    expect(fast.backend_profile() == BackendProfile::FastLookup, "fast profile should be selectable");

    StringBimap compact(0, BackendProfile::CompactMemory);
    expect(compact.backend_profile() == BackendProfile::CompactMemory, "compact profile should be selectable");

    StringBimap array_map(0, BackendProfile::FastLookupArrayMap);
    expect(array_map.backend_profile() == BackendProfile::FastLookupArrayMap,
           "array_map profile should be selectable");

    StringBimap marisa(0, BackendProfile::CompactMemoryMarisa);
    expect(marisa.backend_profile() == BackendProfile::CompactMemoryMarisa, "marisa profile should be selectable");

    StringBimap marisa_array_map(0, BackendProfile::CompactMemoryMarisaArrayMap);
    expect(marisa_array_map.backend_profile() == BackendProfile::CompactMemoryMarisaArrayMap,
           "marisa_array_map profile should be selectable");

    StringBimap keyvi(0, BackendProfile::CompactMemoryKeyvi);
    expect(keyvi.backend_profile() == BackendProfile::CompactMemoryKeyvi, "keyvi profile should be selectable");

    StringBimap fst(0, BackendProfile::CompactMemoryFst);
    expect(fst.backend_profile() == BackendProfile::CompactMemoryFst, "fst profile should be selectable");
}

}  // namespace

int main() {
    test_backend_profile_explicit_selection();
    for_each_profile([](BackendProfile profile) {
        test_empty_strings_are_ignored(profile);
        test_basic_insert_erase_compact(profile);
        test_delete_reinsert_gets_new_id(profile);
        test_contains_id_and_invalid_erases(profile);
        test_compaction_preserves_live_ids_over_many_cycles(profile);
        test_randomized_model(profile);
        test_serialization_round_trip_stream(profile);
        test_serialization_round_trip_file(profile);
        test_iteration_api(profile);
        test_prefix_query_api(profile);
    });
    test_compact_native_sidecars();
    test_save_compacted_preserves_ids_and_sidecars();
}
