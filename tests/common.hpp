#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <string_bimap/string_bimap.hpp>

namespace string_bimap_test {

using string_bimap::BackendProfile;
using string_bimap::StringBimap;
using string_bimap::StringId;

[[nodiscard]] inline std::string make_string(std::mt19937& rng, std::size_t max_len = 24) {
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

inline void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test failure: " << message << '\n';
        std::abort();
    }
}

inline void remove_all_sidecars(const std::string& path) {
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".compact.xcdat");
    std::filesystem::remove(path + ".compact.marisa");
    std::filesystem::remove(path + ".compact.keyvi");
    std::filesystem::remove(path + ".compact.fst");
    std::filesystem::remove(path + ".compact.ids");
    std::filesystem::remove(path + ".native.state");
    std::filesystem::remove(path + ".native.base");
    std::filesystem::remove(path + ".native.delta");
}

[[nodiscard]] inline std::string compact_sidecar_path(BackendProfile profile, const std::string& path) {
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

[[nodiscard]] inline bool compact_backend_uses_ids_sidecar(BackendProfile profile) {
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

}  // namespace string_bimap_test
