#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
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
using string_bimap::StringId;

[[nodiscard]] std::string make_string(std::mt19937& rng, std::size_t max_len = 24) {
    static constexpr char alphabet[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789_-:/.";

    std::uniform_int_distribution<std::size_t> length_dist(0, max_len);
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

void test_basic_insert_erase_compact() {
    StringBimap dict;

    const auto empty_id = dict.insert("");
    const auto apple_id = dict.insert("apple");
    const auto banana_id = dict.insert("banana");
    const auto duplicate_apple = dict.insert("apple");

    expect(apple_id == duplicate_apple, "duplicate insert should reuse id");
    expect(dict.contains("apple"), "apple should exist");
    expect(dict.find_id("banana").value() == banana_id, "banana id mismatch");
    expect(dict.get_string(apple_id) == std::string_view("apple"), "apple value mismatch");
    expect(dict.get_string(empty_id) == std::string_view(""), "empty string mismatch");
    expect(dict.live_size() == 3, "live size should count unique live strings");

    expect(dict.erase(apple_id), "erase by id should succeed");
    expect(!dict.contains("apple"), "apple should be absent after erase");
    expect(dict.get_string(apple_id).empty(), "erased id should decode to empty view");
    expect(dict.live_size() == 2, "live size should shrink after erase");

    const auto carrot_id = dict.insert("carrot");
    expect(dict.contains("carrot"), "carrot should exist");
    expect(dict.get_string(carrot_id) == std::string_view("carrot"), "carrot value mismatch");

    dict.compact();

    expect(dict.find_id("banana").value() == banana_id, "banana id should survive compaction");
    expect(dict.get_string(banana_id) == std::string_view("banana"), "banana value should survive compaction");
    expect(dict.find_id("carrot").value() == carrot_id, "carrot id should survive compaction");
    expect(dict.get_string(carrot_id) == std::string_view("carrot"), "carrot value should survive compaction");
    expect(!dict.contains("apple"), "apple should remain absent after compaction");
    expect(dict.get_string(empty_id) == std::string_view(""), "empty string should survive compaction");
    expect(dict.live_size() == 3, "live size should remain correct after compaction");
}

void test_delete_reinsert_gets_new_id() {
    StringBimap dict;

    const auto id1 = dict.insert("alpha");
    expect(dict.erase("alpha"), "erase by value should succeed");
    expect(!dict.contains("alpha"), "alpha should be absent after erase");

    const auto id2 = dict.insert("alpha");
    expect(id2 != id1, "reinserted key should get a new stable id");
    expect(dict.get_string(id1).empty(), "old id should remain tombstoned");
    expect(dict.get_string(id2) == std::string_view("alpha"), "new id should decode");

    dict.compact();

    expect(dict.find_id("alpha").value() == id2, "latest id should survive compaction");
    expect(dict.get_string(id1).empty(), "old tombstoned id should remain absent after compaction");
    expect(dict.get_string(id2) == std::string_view("alpha"), "new id should still decode after compaction");
}

void test_contains_id_and_invalid_erases() {
    StringBimap dict;

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

void test_compaction_preserves_live_ids_over_many_cycles() {
    StringBimap dict;
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
            expect(dict.find_id(key).value() == id, "live id should remain stable across compaction");
            expect(dict.get_string(id) == key, "live string should remain decodable across compaction");
        }
    }
}

void test_randomized_model() {
    std::mt19937 rng(0xC0FFEEu);
    StringBimap dict;

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

void test_serialization_round_trip_stream() {
    StringBimap dict;

    const auto empty_id = dict.insert("");
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

    expect(restored.size() == dict.size(), "serialized next_id should round-trip");
    expect(restored.live_size() == dict.live_size(), "serialized live size should round-trip");
    expect(restored.get_string(empty_id) == std::string_view(""), "empty string should round-trip");
    expect(restored.find_id("alpha").value() == alpha_id, "alpha id should round-trip");
    expect(restored.find_id("gamma").value() == gamma_id, "gamma id should round-trip");
    expect(restored.find_id("delta").value() == delta_id, "delta id should round-trip");
    expect(!restored.contains("beta"), "deleted key should remain absent after load");
    expect(!restored.contains_id(beta_id), "deleted id hole should remain absent after load");
}

void test_serialization_round_trip_file() {
    StringBimap dict;

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
    dict.save(path);
    auto restored = StringBimap::load(path);

    expect(restored.size() == dict.size(), "file round-trip should preserve next_id");
    expect(restored.live_size() == ids.size(), "file round-trip should preserve live count");
    for (const auto& [key, id] : ids) {
        expect(restored.find_id(key).value() == id, "file round-trip should preserve ids");
        expect(restored.get_string(id) == key, "file round-trip should preserve decoded values");
    }
}

void test_iteration_api() {
    StringBimap dict;

    const auto alpha = dict.insert("alpha");
    const auto empty = dict.insert("");
    const auto beta = dict.insert("beta");
    expect(dict.erase(beta), "erase before iteration should succeed");
    const auto gamma = dict.insert("gamma");

    std::vector<std::pair<StringId, std::string>> seen;
    dict.for_each_live([&](StringId id, std::string_view value) {
        seen.emplace_back(id, std::string(value));
    });

    expect(seen.size() == 3, "iteration should visit only live entries");
    expect((seen == std::vector<std::pair<StringId, std::string>>{
                        {alpha, "alpha"},
                        {empty, ""},
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

    std::vector<std::pair<StringId, std::string>> after_load;
    restored.for_each_live([&](StringId id, std::string_view value) {
        after_load.emplace_back(id, std::string(value));
    });
    expect(after_load == seen, "iteration should round-trip through serialization");
}

void test_prefix_query_api() {
    StringBimap dict;

    const auto app = dict.insert("app");
    const auto apple = dict.insert("apple");
    const auto apricot = dict.insert("apricot");
    const auto banana = dict.insert("banana");
    const auto empty = dict.insert("");
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
                            {empty, ""},
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

    std::vector<std::pair<StringId, std::string>> after_load;
    restored.for_each_with_prefix("app", [&](StringId id, std::string_view value) {
        after_load.emplace_back(id, std::string(value));
    });
    expect(after_load == seen, "prefix query should round-trip through serialization");
}

}  // namespace

int main() {
    test_basic_insert_erase_compact();
    test_delete_reinsert_gets_new_id();
    test_contains_id_and_invalid_erases();
    test_compaction_preserves_live_ids_over_many_cycles();
    test_randomized_model();
    test_serialization_round_trip_stream();
    test_serialization_round_trip_file();
    test_iteration_api();
    test_prefix_query_api();
}
