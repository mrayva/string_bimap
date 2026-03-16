#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"

namespace {

using namespace string_bimap_test;

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
    expect((seen == std::vector<std::pair<StringId, std::string>>{{alpha, "alpha"}, {gamma, "gamma"}}),
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
    expect((seen == std::vector<std::pair<StringId, std::string>>{{app, "app"}, {apple, "apple"}}),
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

}  // namespace

int main() {
    for_each_profile([](BackendProfile profile) {
        test_iteration_api(profile);
        test_prefix_query_api(profile);
    });
}
