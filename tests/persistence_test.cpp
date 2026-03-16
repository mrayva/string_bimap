#include <sstream>
#include <string>
#include <unordered_map>

#include "common.hpp"

namespace {

using namespace string_bimap_test;

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
    expect(restored.find_id("alpha").value() == alpha_id, "alpha id should round-trip");
    expect(restored.find_id("gamma").value() == gamma_id, "gamma id should round-trip");
    expect(restored.find_id("delta").value() == delta_id, "delta id should round-trip");
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
    remove_all_sidecars(path);
    dict.save(path);
    auto restored = StringBimap::load(path);

    expect(restored.backend_profile() == profile, "file round-trip should preserve profile");
    expect(restored.size() == dict.size(), "file round-trip should preserve next_id");
    expect(restored.live_size() == ids.size(), "file round-trip should preserve live count");
    for (const auto& [key, id] : ids) {
        expect(restored.find_id(key).value() == id, "file round-trip should preserve ids");
        expect(restored.get_string(id) == key, "file round-trip should preserve decoded values");
        expect(restored.try_get_string(id).value() == key, "try_get_string should preserve decoded values");
    }
    remove_all_sidecars(path);
}

void test_native_snapshot_round_trip_file(BackendProfile profile) {
    StringBimap dict(0, profile);

    const auto alpha = dict.insert("alpha");
    const auto beta = dict.insert("beta");
    const auto gamma = dict.insert("gamma");
    dict.compact();
    expect(dict.erase(beta), "erase after compaction should succeed");
    const auto delta = dict.insert("delta");

    const std::string path = "/tmp/string_bimap_native_snapshot.bin";
    remove_all_sidecars(path);
    dict.save(path);

    expect(std::filesystem::exists(path + ".native.state"), "native state sidecar should be written");
    expect(std::filesystem::exists(path + ".native.base"), "native base sidecar should be written");
    expect(std::filesystem::exists(path + ".native.delta"), "native delta sidecar should be written");

    auto restored = StringBimap::load(path);
    expect(restored.backend_profile() == profile, "native snapshot load should preserve profile");
    expect(restored.size() == dict.size(), "native snapshot load should preserve next_id");
    expect(restored.live_size() == dict.live_size(), "native snapshot load should preserve live size");
    expect(restored.find_id("alpha").value() == alpha, "native snapshot load should preserve base id");
    expect(restored.find_id("gamma").value() == gamma, "native snapshot load should preserve compacted base id");
    expect(restored.find_id("delta").value() == delta, "native snapshot load should preserve delta id");
    expect(!restored.contains("beta"), "native snapshot load should preserve tombstones");
    expect(!restored.contains_id(beta), "native snapshot load should preserve deleted hole");

    remove_all_sidecars(path);
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
        remove_all_sidecars(path);

        dict.save(path);

        expect(std::filesystem::exists(trie_path), "compact trie sidecar should be written");
        if (compact_backend_uses_ids_sidecar(profile)) {
            expect(std::filesystem::exists(ids_path), "compact id sidecar should be written");
        } else {
            expect(!std::filesystem::exists(ids_path), "compact fst backend should not emit id sidecar");
        }

        auto restored = StringBimap::load(path);
        expect(restored.find_id("alpha").value() == alpha, "native compact load should preserve alpha");
        expect(restored.find_id("beta").value() == beta, "native compact load should preserve beta");
        remove_all_sidecars(path);
    };
#if defined(STRING_BIMAP_HAS_XCDAT)
    run(BackendProfile::CompactMemory);
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
    run(BackendProfile::CompactMemoryMarisa);
#endif
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
        remove_all_sidecars(path);

        dict.save_compacted(path);

        expect(std::filesystem::exists(trie_path), "save_compacted should emit compact trie sidecar");
        if (compact_backend_uses_ids_sidecar(profile)) {
            expect(std::filesystem::exists(ids_path), "save_compacted should emit compact id sidecar");
        } else {
            expect(!std::filesystem::exists(ids_path), "save_compacted fst backend should not emit id sidecar");
        }

        auto restored = StringBimap::load(path);
        expect(restored.find_id("alpha").value() == alpha, "save_compacted should preserve alpha id");
        expect(restored.find_id("gamma").value() == gamma, "save_compacted should preserve gamma id");
        expect(!restored.contains("beta"), "save_compacted should preserve deleted beta");
        expect(!restored.contains_id(beta), "save_compacted should preserve deleted beta hole");

        remove_all_sidecars(path);
    };
#if defined(STRING_BIMAP_HAS_XCDAT)
    run(BackendProfile::CompactMemory);
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
    run(BackendProfile::CompactMemoryMarisa);
#endif
#endif
}

}  // namespace

int main() {
    for_each_profile([](BackendProfile profile) {
        test_serialization_round_trip_stream(profile);
        test_serialization_round_trip_file(profile);
        test_native_snapshot_round_trip_file(profile);
    });
    test_compact_native_sidecars();
    test_save_compacted_preserves_ids_and_sidecars();
}
