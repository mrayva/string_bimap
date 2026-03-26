#include "common.hpp"

#include <sstream>

#include <string_bimap/pthash_bimap.hpp>

namespace {

using string_bimap::PthashBimap;
using string_bimap::PthashBuildOptions;
using string_bimap::PthashIdWidth;
using string_bimap_test::expect;

void test_basic_lookup_and_reverse() {
    const std::vector<std::string> values = {
        "Common Stock",
        "Mutual Fund",
        "Depositary Receipt",
        "Bond",
        "ETF",
        "RIGHT",
    };

    PthashBimap bimap(values);
    expect(!bimap.empty(), "pthash bimap should build");
    expect(bimap.size() == values.size(), "pthash bimap should preserve cardinality");
    expect(bimap.id_width() == PthashIdWidth::U8, "small sets should use uint8 ids");

    for (const auto& value : values) {
        const auto id = bimap.find(value);
        expect(id.has_value(), "live value should resolve to an id");
        expect(bimap.by_id(*id) == value, "reverse lookup should round-trip");
        const auto compact = bimap.find_compact(value);
        expect(compact.has_value(), "compact lookup should succeed");
        expect(std::holds_alternative<std::uint8_t>(*compact), "small sets should return uint8 compact ids");
    }

    expect(!bimap.find("missing").has_value(), "non-members should be rejected");
    expect(bimap.by_id(9999).empty(), "invalid reverse lookup should return empty view");
}

void test_deterministic_ids_ignore_input_order_and_duplicates() {
    const std::vector<std::string> left = {
        "TERM",
        "REV",
        "LOC",
        "DIP",
        "REV",
        "TERM",
    };
    const std::vector<std::string> right = {
        "DIP",
        "LOC",
        "REV",
        "TERM",
    };

    PthashBimap a(left);
    PthashBimap b(right);

    expect(a.size() == 4, "duplicates should be removed deterministically");
    expect(a.size() == b.size(), "same set should rebuild to same cardinality");
    expect(a.seed() == b.seed(), "same set should converge on the same seed");

    for (const auto& value : right) {
        expect(a.find(value) == b.find(value), "same set should get same ids across sessions");
    }
}

void test_required_id_width_thresholds() {
    expect(PthashBimap::required_id_width(0) == PthashIdWidth::U8, "empty sets fit in uint8");
    expect(PthashBimap::required_id_width(256) == PthashIdWidth::U8, "256 values fit in uint8");
    expect(PthashBimap::required_id_width(257) == PthashIdWidth::U16, "257 values require uint16");
    expect(PthashBimap::required_id_width(65536) == PthashIdWidth::U16, "65536 values fit in uint16");
    expect(PthashBimap::required_id_width(65537) == PthashIdWidth::U32, "65537 values require uint32");
}

void test_find_as_and_round_trip_persistence() {
    std::vector<std::string> values;
    for (int i = 0; i < 300; ++i) {
        values.push_back("key_" + std::to_string(i));
    }

    PthashBuildOptions options;
    options.initial_seed = 0;
    options.max_seed_attempts = 4096;

    PthashBimap built(values, options);
    expect(built.id_width() == PthashIdWidth::U16, "300 values should use uint16 ids");

    const auto compact_id = built.find_as<std::uint16_t>("key_42");
    expect(compact_id.has_value(), "typed lookup should succeed");
    expect(built.by_id(*compact_id) == "key_42", "typed reverse lookup should round-trip");

    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    built.save(buffer);
    buffer.seekg(0);

    const auto restored = PthashBimap::load(buffer);
    expect(restored.size() == built.size(), "load should preserve size");
    expect(restored.id_width() == built.id_width(), "load should preserve id width");
    expect(restored.seed() == built.seed(), "load should preserve deterministic seed");

    for (const auto& value : values) {
        expect(built.find(value) == restored.find(value), "load should preserve ids");
    }
}

}  // namespace

int main() {
    expect(PthashBimap::available(), "pthash test target should only build when pthash is available");
    test_basic_lookup_and_reverse();
    test_deterministic_ids_ignore_input_order_and_duplicates();
    test_required_id_width_thresholds();
    test_find_as_and_round_trip_persistence();
    return 0;
}
