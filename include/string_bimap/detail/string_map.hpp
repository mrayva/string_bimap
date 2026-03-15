#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "../types.hpp"

#if defined(STRING_BIMAP_HAS_ARRAY_HASH)
#include <tsl/array_map.h>
#endif

namespace string_bimap::detail {

struct TransparentHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }

    std::size_t operator()(const std::string& value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }

    std::size_t operator()(const char* value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

struct TransparentEq {
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
        return lhs == rhs;
    }
};

using StringIdMap = std::unordered_map<std::string, StringId, TransparentHash, TransparentEq>;

inline auto map_find(const StringIdMap& map, std::string_view key) {
#if defined(__cpp_lib_generic_unordered_lookup) && __cpp_lib_generic_unordered_lookup >= 201811L
    return map.find(key);
#else
    return map.find(std::string(key));
#endif
}

inline auto map_find(StringIdMap& map, std::string_view key) {
#if defined(__cpp_lib_generic_unordered_lookup) && __cpp_lib_generic_unordered_lookup >= 201811L
    return map.find(key);
#else
    return map.find(std::string(key));
#endif
}

inline StringId map_value(const StringIdMap::const_iterator& it) {
    return it->second;
}

inline StringId map_value(const StringIdMap::iterator& it) {
    return it->second;
}

inline void map_insert_or_assign(StringIdMap& map, std::string_view key, StringId value) {
    map[std::string(key)] = value;
}

inline void map_reserve(StringIdMap& map, std::size_t size) {
    map.reserve(size);
}

[[nodiscard]] inline std::size_t estimate_map_memory_bytes(const StringIdMap& map) {
    std::size_t total = sizeof(map);
    total += map.bucket_count() * sizeof(void*);
    for (const auto& entry : map) {
        total += sizeof(decltype(entry));
        total += entry.first.capacity();
    }
    return total;
}

#if defined(STRING_BIMAP_HAS_ARRAY_HASH)
using ArrayStringIdMap = tsl::array_map<char, StringId>;

class CountingSerializer {
public:
    template <class U>
    void operator()(const U&) {
        bytes_ += sizeof(U);
    }

    void operator()(const char*, std::size_t value_size) {
        bytes_ += value_size;
    }

    [[nodiscard]] std::size_t bytes() const noexcept {
        return bytes_;
    }

private:
    std::size_t bytes_ = 0;
};

[[nodiscard]] inline std::size_t estimate_map_memory_bytes(const ArrayStringIdMap& map) {
    CountingSerializer serializer;
    map.serialize(serializer);
    return serializer.bytes();
}
#endif

}  // namespace string_bimap::detail
