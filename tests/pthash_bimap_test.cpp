#include "common.hpp"

#include <filesystem>
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
    expect(bimap.fits_in_uint8(), "small sets should report uint8 fit");
    expect(bimap.fits_in_uint16(), "small sets should report uint16 fit");
    expect(bimap.can_represent_ids_as<std::uint8_t>(), "small sets should fit in uint8_t");
    expect(bimap.max_id() == values.size() - 1, "max_id should match dense cardinality");

    const auto info = bimap.id_info();
    expect(info.width == PthashIdWidth::U8, "id_info should expose width");
    expect(info.width_bytes == 1, "id_info should expose width bytes");
    expect(info.cardinality == values.size(), "id_info should expose cardinality");
    expect(bimap.contains("ETF"), "contains should report live members");
    expect(!bimap.contains("missing"), "contains should reject non-members");
    expect(bimap.contains_id(0), "contains_id should accept dense ids");
    expect(!bimap.contains_id(9999), "contains_id should reject invalid ids");

    for (const auto& value : values) {
        const auto id = bimap.find(value);
        expect(id.has_value(), "live value should resolve to an id");
        expect(bimap.by_id(*id) == value, "reverse lookup should round-trip");
        expect(bimap.try_by_id(*id).value() == value, "try_by_id should round-trip");
        expect(bimap.at(*id) == value, "at should round-trip");
        const auto compact = bimap.find_compact(value);
        expect(compact.has_value(), "compact lookup should succeed");
        expect(std::holds_alternative<std::uint8_t>(*compact), "small sets should return uint8 compact ids");
        expect(PthashBimap::widen_compact_id(*compact) == *id, "compact ids should widen losslessly");
        expect(bimap.by_compact_id(*compact) == value, "compact reverse lookup should round-trip");
        expect(bimap.try_by_compact_id(*compact).value() == value,
               "try_by_compact_id should round-trip");
    }

    expect(!bimap.find("missing").has_value(), "non-members should be rejected");
    expect(bimap.by_id(9999).empty(), "invalid reverse lookup should return empty view");
    expect(!bimap.try_by_id(9999).has_value(), "try_by_id should reject invalid ids");
    expect(!bimap.compact_id_for(9999).has_value(), "invalid ids should not compact");

    bool threw = false;
    try {
        (void)bimap.at(9999);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    expect(threw, "at should throw on invalid ids");
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
    expect(!built.fits_in_uint8(), "300 values should not fit in uint8");
    expect(built.fits_in_uint16(), "300 values should fit in uint16");
    expect(built.can_represent_ids_as<std::uint16_t>(), "300 values should fit in uint16_t");

    const auto compact_id = built.find_as<std::uint16_t>("key_42");
    expect(compact_id.has_value(), "typed lookup should succeed");
    expect(built.by_id(*compact_id) == "key_42", "typed reverse lookup should round-trip");
    const auto compact_variant = built.compact_id_for(*compact_id);
    expect(compact_variant.has_value(), "compact_id_for should succeed");
    expect(std::holds_alternative<std::uint16_t>(*compact_variant), "300 values should yield uint16 compact ids");

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

void test_native_file_sidecar_round_trip() {
    const std::string path = "/tmp/string_bimap_pthash_bimap_test.bin";
    const std::string native_sidecar = path + ".native.pthash";
    std::filesystem::remove(path);
    std::filesystem::remove(native_sidecar);

    std::vector<std::string> values;
    for (int i = 0; i < 64; ++i) {
        values.push_back("value_" + std::to_string(i));
    }

    PthashBimap built(values);
    built.save(path);
    expect(std::filesystem::exists(path), "logical pthash file should be written");
    expect(std::filesystem::exists(native_sidecar), "native pthash sidecar should be written");

    const auto restored = PthashBimap::load(path);
    expect(restored.size() == built.size(), "native sidecar load should preserve size");
    for (const auto& value : values) {
        expect(restored.find(value) == built.find(value), "native sidecar load should preserve ids");
    }

    std::filesystem::remove(path);
    std::filesystem::remove(native_sidecar);
}

void test_csv_loader_and_builder() {
    std::stringstream csv;
    csv << "Symbol,Company Name,Type\n";
    csv << "SPY,\"SPDR S&P 500 ETF Trust\",ETF\n";
    csv << "QQQ,Invesco QQQ Trust,ETF\n";
    csv << "IWM,iShares Russell 2000 ETF,ETF\n";

    const auto values = PthashBimap::load_values_from_csv(csv, "Company Name");
    expect(values.size() == 3, "CSV loader should read the selected column");
    expect(values[0] == "SPDR S&P 500 ETF Trust", "CSV loader should decode quoted fields");

    csv.clear();
    csv.seekg(0);
    const auto bimap = PthashBimap::from_csv(csv, "Symbol");
    expect(bimap.size() == 3, "CSV builder should build from selected column");
    expect(bimap.find("QQQ").has_value(), "CSV-built bimap should resolve values");
}

void test_json_array_loader_and_builder() {
    std::stringstream json;
    json << "[\"Bond\", \"Common Stock\", \"Depositary Receipt\", \"ETF\", \"Line\\nBreak\"]";

    const auto values = PthashBimap::load_values_from_json_array(json);
    expect(values.size() == 5, "JSON loader should read all strings");
    expect(values[4] == std::string("Line\nBreak"), "JSON loader should decode escapes");

    json.clear();
    json.seekg(0);
    const auto bimap = PthashBimap::from_json_array(json);
    expect(bimap.find("Common Stock").has_value(), "JSON-built bimap should resolve values");
    expect(bimap.find("missing") == std::nullopt, "JSON-built bimap should reject non-members");

    std::stringstream wrapped;
    wrapped << "{\"values\":[\"Bond\",\"ETF\",\"Common Stock\"]}";
    const auto wrapped_values = PthashBimap::load_values_from_json_array(wrapped);
    expect(wrapped_values.size() == 3, "wrapped OpenFIGI-style JSON should be accepted");
    expect(wrapped_values[1] == "ETF", "wrapped OpenFIGI-style JSON should preserve order");
}

}  // namespace

int main() {
    expect(PthashBimap::available(), "pthash test target should only build when pthash is available");
    test_basic_lookup_and_reverse();
    test_deterministic_ids_ignore_input_order_and_duplicates();
    test_required_id_width_thresholds();
    test_find_as_and_round_trip_persistence();
    test_native_file_sidecar_round_trip();
    test_csv_loader_and_builder();
    test_json_array_loader_and_builder();
    return 0;
}
