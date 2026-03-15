#pragma once

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "detail/string_map.hpp"
#include "packed_string_arena.hpp"
#include "serialization.hpp"
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

    explicit BaseSegment(BackendProfile profile = BackendProfile::FastLookup)
        : profile_(profile) {}

    void set_backend_profile(BackendProfile profile) {
        profile_ = profile;
    }

    [[nodiscard]] std::optional<StringId> find_id(std::string_view value) const {
#if defined(STRING_BIMAP_HAS_XCDAT)
        if (use_compact_index()) {
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

    [[nodiscard]] SegmentMemoryUsage memory_usage() const noexcept {
        SegmentMemoryUsage usage;
        usage.arena_bytes = arena_.bytes_reserved();
        usage.entry_table_bytes = entries_by_id_.capacity() * sizeof(EntryLocation);
        usage.fallback_index_bytes = detail::estimate_map_memory_bytes(fallback_index_);
#if defined(STRING_BIMAP_HAS_XCDAT)
        usage.compact_index_bytes += local_to_global_.capacity() * sizeof(StringId);
        if (trie_.has_value()) {
            usage.compact_index_bytes += static_cast<std::size_t>(xcdat::memory_in_bytes(*trie_));
        }
#endif
        return usage;
    }

    [[nodiscard]] bool has_native_compact_index() const noexcept {
#if defined(STRING_BIMAP_HAS_XCDAT)
        return profile_ == BackendProfile::CompactMemory && trie_.has_value();
#else
        return false;
#endif
    }

    void save_native_compact_index(const std::string& trie_path, const std::string& ids_path) const {
#if defined(STRING_BIMAP_HAS_XCDAT)
        if (!has_native_compact_index()) {
            throw std::runtime_error("compact trie index is not available");
        }
        xcdat::save(*trie_, trie_path);
        detail::write_vector_file(ids_path, local_to_global_);
#else
        (void)trie_path;
        (void)ids_path;
        throw std::runtime_error("compact trie index serialization requires xcdat");
#endif
    }

    [[nodiscard]] bool load_native_compact_index(std::vector<BuildItem> items,
                                                 const std::string& trie_path,
                                                 const std::string& ids_path) {
#if defined(STRING_BIMAP_HAS_XCDAT)
        try {
            rebuild_storage(items);
            fallback_index_.clear();
            trie_ = xcdat::load<TrieType>(trie_path);
            local_to_global_ = detail::read_vector_file<StringId>(ids_path);
            if (!trie_.has_value() || local_to_global_.size() != trie_->num_keys()) {
                throw std::runtime_error("compact trie sidecar size mismatch");
            }
            return true;
        } catch (...) {
            trie_.reset();
            local_to_global_.clear();
            return false;
        }
#else
        (void)items;
        (void)trie_path;
        (void)ids_path;
        return false;
#endif
    }

    template <class Func>
    void for_each_with_prefix(std::string_view prefix, Func&& func) const {
#if defined(STRING_BIMAP_HAS_XCDAT)
        if (use_compact_index()) {
            std::vector<StringId> ids;
            trie_->predictive_search(prefix, [&](std::uint64_t local_id, std::string_view) {
                const auto id = local_to_global_[static_cast<std::size_t>(local_id)];
                if (id != kInvalidId) {
                    ids.push_back(id);
                }
            });
            std::sort(ids.begin(), ids.end());
            for (const auto id : ids) {
                func(id, get_string(id));
            }
            return;
        }
#endif
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
#if defined(STRING_BIMAP_HAS_XCDAT)
        if (use_compact_index()) {
            trie_->predictive_search(prefix, [&](std::uint64_t local_id, std::string_view) {
                const auto id = local_to_global_[static_cast<std::size_t>(local_id)];
                if (id != kInvalidId) {
                    func(id, get_string(id));
                }
            });
            return;
        }
#endif
        for_each_with_prefix(prefix, std::forward<Func>(func));
    }

    void rebuild(std::vector<BuildItem> items) {
        rebuild_storage(items);
        build_lookup(items);
    }

private:
    void release_fallback_index() {
        detail::StringIdMap empty;
        fallback_index_.swap(empty);
    }

    void rebuild_storage(std::vector<BuildItem>& items) {
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
    }
    [[nodiscard]] bool use_compact_index() const noexcept {
#if defined(STRING_BIMAP_HAS_XCDAT)
        return profile_ == BackendProfile::CompactMemory && trie_.has_value();
#else
        return false;
#endif
    }

    void build_lookup(const std::vector<BuildItem>& items) {
        release_fallback_index();
        detail::map_reserve(fallback_index_, items.size());
        for (const auto& item : items) {
            detail::map_insert_or_assign(fallback_index_, item.value, item.id);
        }

#if defined(STRING_BIMAP_HAS_XCDAT)
        trie_.reset();
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

        if (profile_ == BackendProfile::CompactMemory) {
            trie_ = TrieType(keys);
            local_to_global_.assign(keys.size(), kInvalidId);
            for (std::size_t local_id = 0; local_id < keys.size(); ++local_id) {
                const auto key = trie_->decode(local_id);
                const auto global = detail::map_find(fallback_index_, key);
                if (global != fallback_index_.end()) {
                    local_to_global_[local_id] = detail::map_value(global);
                }
            }
            release_fallback_index();
        } else {
            local_to_global_.clear();
        }
#endif
    }

    PackedStringArena arena_;
    std::vector<EntryLocation> entries_by_id_;
    detail::StringIdMap fallback_index_;
    BackendProfile profile_;
    std::size_t live_size_ = 0;

#if defined(STRING_BIMAP_HAS_XCDAT)
    using TrieType = xcdat::trie_8_type;
    std::optional<TrieType> trie_;
    std::vector<StringId> local_to_global_;
#endif
};

}  // namespace string_bimap
