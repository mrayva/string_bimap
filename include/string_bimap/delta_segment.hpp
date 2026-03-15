#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "detail/string_map.hpp"
#include "packed_string_arena.hpp"
#include "types.hpp"

#if defined(STRING_BIMAP_HAS_HAT_TRIE)
#include <tsl/htrie_map.h>
#endif

namespace string_bimap {

class DeltaSegment {
public:
    explicit DeltaSegment(std::size_t reserve_bytes = 0, BackendProfile profile = BackendProfile::FastLookup)
        : arena_(reserve_bytes), profile_(profile) {}

    void set_backend_profile(BackendProfile profile) {
        profile_ = profile;
    }

    [[nodiscard]] std::optional<StringId> find_id(std::string_view value) const {
        if (use_compact_index()) {
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
            const auto it = compact_index_.find(value);
            if (it == compact_index_.end()) {
                return std::nullopt;
            }
            return it.value();
#endif
        }

        const auto it = detail::map_find(fallback_index_, value);
        if (it == fallback_index_.end()) {
            return std::nullopt;
        }
        return detail::map_value(it);
    }

    [[nodiscard]] bool contains_id(StringId id) const noexcept {
        return id < entries_by_id_.size() && entries_by_id_[id].live();
    }

    [[nodiscard]] std::string_view get_string(StringId id) const noexcept {
        if (!contains_id(id)) {
            return {};
        }
        return arena_.view(entries_by_id_[id]);
    }

    void insert(StringId id, std::string_view value) {
        if (id >= entries_by_id_.size()) {
            entries_by_id_.resize(static_cast<std::size_t>(id) + 1);
        }
        const auto location = arena_.append(value);
        entries_by_id_[id] = location;
        if (use_compact_index()) {
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
            const auto it = compact_index_.find(value);
            if (it == compact_index_.end()) {
                compact_index_.insert(value, id);
            } else {
                it.value() = id;
            }
#endif
        } else {
            detail::map_insert_or_assign(fallback_index_, value, id);
        }
        ++live_size_;
    }

    bool erase(StringId id) {
        if (!contains_id(id)) {
            return false;
        }
        const auto value = get_string(id);
        if (use_compact_index()) {
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
            const auto it = compact_index_.find(value);
            if (it != compact_index_.end() && it.value() == id) {
                compact_index_.erase(value);
            }
#endif
        } else {
            const auto it = detail::map_find(fallback_index_, value);
            if (it != fallback_index_.end() && detail::map_value(it) == id) {
                fallback_index_.erase(it);
            }
        }
        entries_by_id_[id] = {};
        --live_size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return live_size_;
    }

    [[nodiscard]] std::size_t bytes_used() const noexcept {
        return arena_.bytes_used();
    }

    [[nodiscard]] SegmentMemoryUsage memory_usage() const noexcept {
        SegmentMemoryUsage usage;
        usage.arena_bytes = arena_.bytes_reserved();
        usage.entry_table_bytes = entries_by_id_.capacity() * sizeof(EntryLocation);
        usage.fallback_index_bytes = detail::estimate_map_memory_bytes(fallback_index_);
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
        if (use_compact_index()) {
            usage.compact_index_bytes += compact_index_serialized_bytes();
        }
#endif
        return usage;
    }

    template <class Func>
    void for_each_with_prefix(std::string_view prefix, Func&& func) const {
        if (use_compact_index()) {
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
            std::vector<StringId> ids;
            const auto range = compact_index_.equal_prefix_range(prefix);
            for (auto it = range.first; it != range.second; ++it) {
                ids.push_back(it.value());
            }
            std::sort(ids.begin(), ids.end());
            for (const auto id : ids) {
                if (contains_id(id)) {
                    func(id, get_string(id));
                }
            }
            return;
#endif
        }
        for (StringId id = 0; id < entries_by_id_.size(); ++id) {
            if (!entries_by_id_[id].live()) {
                continue;
            }
            const auto value = arena_.view(entries_by_id_[id]);
            if (value.substr(0, prefix.size()) == prefix) {
                func(id, value);
            }
        }
    }

    template <class Func>
    void for_each_with_prefix_unordered(std::string_view prefix, Func&& func) const {
        if (use_compact_index()) {
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
            const auto range = compact_index_.equal_prefix_range(prefix);
            for (auto it = range.first; it != range.second; ++it) {
                const auto id = it.value();
                if (contains_id(id)) {
                    func(id, get_string(id));
                }
            }
            return;
#endif
        }
        for_each_with_prefix(prefix, std::forward<Func>(func));
    }

    void clear(std::size_t reserve_bytes = 0) {
        arena_.clear(reserve_bytes);
        entries_by_id_.clear();
        fallback_index_.clear();
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
        compact_index_.clear();
#endif
        live_size_ = 0;
    }

private:
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
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

    [[nodiscard]] std::size_t compact_index_serialized_bytes() const {
        CountingSerializer serializer;
        compact_index_.serialize(serializer);
        return serializer.bytes();
    }
#endif

    [[nodiscard]] bool use_compact_index() const noexcept {
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
        return profile_ != BackendProfile::FastLookup;
#else
        return false;
#endif
    }

    PackedStringArena arena_;
    std::vector<EntryLocation> entries_by_id_;
    detail::StringIdMap fallback_index_;
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
    tsl::htrie_map<char, StringId> compact_index_;
#endif
    BackendProfile profile_;
    std::size_t live_size_ = 0;
};

}  // namespace string_bimap
