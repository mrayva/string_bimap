#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "detail/string_map.hpp"
#include "packed_string_arena.hpp"
#include "types.hpp"

#if defined(STRING_BIMAP_HAS_XCDAT)
#include <xcdat.hpp>
#endif

namespace string_bimap {

class BaseSegment {
public:
    struct BuildItem {
        StringId id;
        std::string value;
    };

    [[nodiscard]] std::optional<StringId> find_id(std::string_view value) const {
#if defined(STRING_BIMAP_HAS_XCDAT)
        if (trie_) {
            const auto local = trie_->lookup(value);
            if (!local.has_value()) {
                return std::nullopt;
            }
            return local_to_global_[static_cast<std::size_t>(*local)];
        }
#endif
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

    [[nodiscard]] std::size_t size() const noexcept {
        return live_size_;
    }

    void rebuild(std::vector<BuildItem> items) {
        std::sort(items.begin(), items.end(), [](const BuildItem& lhs, const BuildItem& rhs) {
            if (lhs.id != rhs.id) {
                return lhs.id < rhs.id;
            }
            return lhs.value < rhs.value;
        });

        std::size_t reserve_bytes = 0;
        StringId max_id = 0;
        for (const auto& item : items) {
            reserve_bytes += item.value.size() + 1;
            max_id = std::max(max_id, item.id);
        }

        arena_.clear(reserve_bytes);
        entries_by_id_.assign(items.empty() ? 0 : static_cast<std::size_t>(max_id) + 1, EntryLocation{});
        live_size_ = 0;

        for (const auto& item : items) {
            entries_by_id_[item.id] = arena_.append(item.value);
            ++live_size_;
        }

        build_lookup(items);
    }

private:
    void build_lookup(const std::vector<BuildItem>& items) {
        fallback_index_.clear();
        detail::map_reserve(fallback_index_, items.size());
        for (const auto& item : items) {
            detail::map_insert_or_assign(fallback_index_, item.value, item.id);
        }

#if defined(STRING_BIMAP_HAS_XCDAT)
        std::vector<std::pair<std::string, StringId>> sorted_keys;
        sorted_keys.reserve(items.size());
        for (const auto& item : items) {
            sorted_keys.emplace_back(item.value, item.id);
        }
        std::sort(sorted_keys.begin(), sorted_keys.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });
        sorted_keys.erase(std::unique(sorted_keys.begin(), sorted_keys.end(), [](const auto& lhs, const auto& rhs) {
                             return lhs.first == rhs.first;
                         }),
                         sorted_keys.end());

        std::vector<std::string> keys;
        keys.reserve(sorted_keys.size());
        for (const auto& item : sorted_keys) {
            keys.push_back(item.first);
        }

        trie_ = TrieType(keys);
        local_to_global_.assign(keys.size(), kInvalidId);
        for (std::size_t local_id = 0; local_id < keys.size(); ++local_id) {
            const auto key = trie_->decode(local_id);
            const auto global = detail::map_find(fallback_index_, key);
            if (global != fallback_index_.end()) {
                local_to_global_[local_id] = detail::map_value(global);
            }
        }
#endif
    }

    PackedStringArena arena_;
    std::vector<EntryLocation> entries_by_id_;
    detail::StringIdMap fallback_index_;
    std::size_t live_size_ = 0;

#if defined(STRING_BIMAP_HAS_XCDAT)
    using TrieType = xcdat::trie_8_type;
    std::optional<TrieType> trie_;
    std::vector<StringId> local_to_global_;
#endif
};

}  // namespace string_bimap
