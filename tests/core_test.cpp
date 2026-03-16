#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.hpp"

namespace {

using namespace string_bimap_test;

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
    expect(dict.find_id("banana").value() == banana_id, "banana id mismatch");
    expect(dict.try_get_string(apple_id).value() == std::string_view("apple"), "try_get_string should decode");
    expect(dict.get_string(apple_id) == std::string_view("apple"), "apple value mismatch");
    expect(dict.live_size() == 2, "live size should count unique live strings");

    expect(dict.erase(apple_id), "erase by id should succeed");
    expect(!dict.contains("apple"), "apple should be absent after erase");
    expect(!dict.try_get_string(apple_id).has_value(), "try_get_string should report deleted ids as missing");
    expect(dict.get_string(apple_id).empty(), "erased id should decode to empty view");
    expect(dict.live_size() == 1, "live size should shrink after erase");

    const auto carrot_id = dict.insert("carrot");
    expect(dict.contains("carrot"), "carrot should exist");
    expect(dict.get_string(carrot_id) == std::string_view("carrot"), "carrot value mismatch");

    dict.compact();
    expect(dict.find_id("banana").value() == banana_id, "banana id should survive compaction");
    expect(dict.find_id("carrot").value() == carrot_id, "carrot id should survive compaction");
    expect(dict.get_string(banana_id) == std::string_view("banana"), "banana value should survive compaction");
    expect(dict.get_string(carrot_id) == std::string_view("carrot"), "carrot value should survive compaction");
    expect(!dict.contains("apple"), "apple should remain absent after compaction");
}

void test_delete_reinsert_gets_new_id(BackendProfile profile) {
    StringBimap dict(0, profile);

    const auto id1 = dict.insert("alpha");
    expect(dict.erase("alpha"), "erase by value should succeed");
    const auto id2 = dict.insert("alpha");
    expect(id2 != id1, "reinserted key should get a new stable id");
    expect(dict.get_string(id1).empty(), "old id should remain tombstoned");
    expect(dict.get_string(id2) == std::string_view("alpha"), "new id should decode");

    dict.compact();
    expect(dict.find_id("alpha").value() == id2, "latest id should survive compaction");
    expect(dict.get_string(id1).empty(), "old tombstoned id should remain absent");
}

void test_contains_id_and_invalid_erases(BackendProfile profile) {
    StringBimap dict(0, profile);

    const auto a = dict.insert("a");
    const auto b = dict.insert("b");
    expect(dict.contains_id(a), "a id should exist");
    expect(dict.contains_id(b), "b id should exist");
    expect(!dict.contains_id(12345), "arbitrary invalid id should not exist");
    expect(!dict.erase(12345), "erase invalid id should fail");
    expect(!dict.try_get_string(12345).has_value(), "try_get_string should return nullopt for invalid ids");

    expect(dict.erase(a), "erase a should succeed");
    expect(!dict.erase(a), "erase same id twice should fail");
    expect(!dict.contains_id(a), "erased id should not be contained");
    expect(!dict.try_get_string(a).has_value(), "try_get_string should return nullopt for erased ids");
    expect(dict.contains_id(b), "other id should stay live");
}

void test_compaction_preserves_live_ids_over_many_cycles(BackendProfile profile) {
    StringBimap dict(0, profile);
    std::unordered_map<std::string, StringId> ids;

    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 25; ++i) {
            const auto key = "k_" + std::to_string(round) + "_" + std::to_string(i);
            ids.emplace(key, dict.insert(key));
        }
        for (int i = 0; i < 25; i += 3) {
            const auto key = "k_" + std::to_string(round) + "_" + std::to_string(i);
            expect(dict.erase(key), "scheduled erase should succeed");
            ids.erase(key);
        }
        dict.compact();
        for (const auto& [key, id] : ids) {
            expect(dict.find_id(key).value() == id, "live id should remain stable");
            expect(dict.get_string(id) == key, "live string should remain decodable");
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
            expect(dict.find_id(key).value() == id, "live key id should match model");
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

void test_compaction_policy_api(BackendProfile profile) {
    StringBimap dict(0, profile);
    const string_bimap::CompactionPolicy conservative{};
    expect(!dict.should_compact(conservative), "empty dictionary should not compact");

    for (int i = 0; i < 8; ++i) {
        expect(dict.insert("base_" + std::to_string(i)) != string_bimap::kInvalidId, "base insert should succeed");
    }
    dict.compact();
    const auto before_delta = dict.compaction_stats();
    expect(before_delta.base_live_ids == 8, "base ids should be counted after compaction");
    expect(before_delta.delta_live_ids == 0, "delta ids should be empty after compaction");
    expect(!dict.should_compact(conservative), "freshly compacted dictionary should not compact again");

    for (int i = 0; i < 3; ++i) {
        expect(dict.insert("delta_" + std::to_string(i)) != string_bimap::kInvalidId, "delta insert should succeed");
    }

    string_bimap::CompactionPolicy delta_policy;
    delta_policy.min_delta_ids = 2;
    delta_policy.max_delta_fraction = 0.20;
    delta_policy.min_tombstone_ids = 100;
    delta_policy.max_tombstone_fraction = 1.0;
    delta_policy.min_delta_bytes = 0;
    expect(dict.compaction_stats().delta_live_ids == 3, "delta ids should be counted");
    expect(dict.should_compact(delta_policy), "delta-heavy dictionary should trigger compaction");
    expect(dict.compact_if_needed(delta_policy), "compact_if_needed should perform compaction");
    expect(dict.compaction_stats().delta_live_ids == 0, "compaction should clear the delta");

    StringBimap tombstone_dict(0, profile);
    for (int i = 0; i < 10; ++i) {
        expect(tombstone_dict.insert("item_" + std::to_string(i)) != string_bimap::kInvalidId,
               "tombstone insert should succeed");
    }
    for (int i = 0; i < 4; ++i) {
        expect(tombstone_dict.erase("item_" + std::to_string(i)), "scheduled tombstone erase should succeed");
    }
    string_bimap::CompactionPolicy tombstone_policy;
    tombstone_policy.min_delta_ids = 100;
    tombstone_policy.max_delta_fraction = 1.0;
    tombstone_policy.min_tombstone_ids = 3;
    tombstone_policy.max_tombstone_fraction = 0.20;
    tombstone_policy.min_delta_bytes = std::numeric_limits<std::size_t>::max();
    expect(tombstone_dict.compaction_stats().tombstone_ids == 4, "tombstones should be counted");
    expect(tombstone_dict.should_compact(tombstone_policy), "tombstone-heavy dictionary should trigger compaction");
    expect(tombstone_dict.compact_if_needed(tombstone_policy),
           "compact_if_needed should compact tombstone-heavy dictionaries");
    expect(tombstone_dict.compaction_stats().tombstone_ids == 0, "compaction should clear tombstones");
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
        test_compaction_policy_api(profile);
    });
}
