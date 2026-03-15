#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "../types.hpp"

#if defined(STRING_BIMAP_HAS_HAT_TRIE)
#include <tsl/htrie_map.h>
#endif

namespace string_bimap::detail {

#if defined(STRING_BIMAP_HAS_HAT_TRIE)

using StringIdMap = tsl::htrie_map<char, StringId>;

inline auto map_find(const StringIdMap& map, std::string_view key) {
    return map.find(key);
}

inline auto map_find(StringIdMap& map, std::string_view key) {
    return map.find(key);
}

inline StringId map_value(const StringIdMap::const_iterator& it) {
    return it.value();
}

inline StringId map_value(const StringIdMap::iterator& it) {
    return it.value();
}

inline void map_insert_or_assign(StringIdMap& map, std::string_view key, StringId value) {
    const auto it = map.find(key);
    if (it == map.end()) {
        map.insert(key, value);
    } else {
        it.value() = value;
    }
}

inline void map_reserve(StringIdMap&, std::size_t) {}

#else

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

#endif

}  // namespace string_bimap::detail
