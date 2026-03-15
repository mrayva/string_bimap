#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "detail/string_map.hpp"
#include "packed_string_arena.hpp"
#include "serialization.hpp"
#include "types.hpp"

#if defined(STRING_BIMAP_HAS_HAT_TRIE)
#include <tsl/htrie_map.h>
#endif
#if defined(STRING_BIMAP_HAS_ARRAY_HASH)
#include <tsl/array_map.h>
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
        if (use_array_map_index()) {
#if defined(STRING_BIMAP_HAS_ARRAY_HASH)
            const auto it = array_map_index_.find(value);
            if (it == array_map_index_.end()) {
                return std::nullopt;
            }
            return it.value();
#endif
        }
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
        if (use_array_map_index()) {
#if defined(STRING_BIMAP_HAS_ARRAY_HASH)
            const auto it = array_map_index_.find(value);
            if (it == array_map_index_.end()) {
                array_map_index_.insert(value, id);
            } else {
                it.value() = id;
            }
#endif
        } else if (use_compact_index()) {
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
        if (use_array_map_index()) {
#if defined(STRING_BIMAP_HAS_ARRAY_HASH)
            const auto it = array_map_index_.find(value);
            if (it != array_map_index_.end() && it.value() == id) {
                array_map_index_.erase(value);
            }
#endif
        } else if (use_compact_index()) {
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

    void save_native_storage(const std::string& path) const {
        std::ofstream out(detail::native_delta_storage_sidecar_path(path), std::ios::binary);
        if (!out) {
            throw std::runtime_error("failed to open native delta sidecar for writing");
        }

        detail::write_bytes(out, detail::kNativeStateMagic.data(), detail::kNativeStateMagic.size());
        detail::write_pod(out, detail::kNativeStateVersion);
        detail::write_pod(out, static_cast<std::uint64_t>(live_size_));
        detail::write_pod(out, static_cast<std::uint64_t>(entries_by_id_.size()));
        if (!entries_by_id_.empty()) {
            out.write(reinterpret_cast<const char*>(entries_by_id_.data()),
                      static_cast<std::streamsize>(entries_by_id_.size() * sizeof(EntryLocation)));
            if (!out) {
                throw std::runtime_error("failed to write native delta entry table");
            }
        }
        const auto& bytes = arena_.bytes();
        detail::write_pod(out, static_cast<std::uint64_t>(bytes.size()));
        if (!bytes.empty()) {
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!out) {
                throw std::runtime_error("failed to write native delta arena payload");
            }
        }
    }

    [[nodiscard]] bool load_native_storage(const std::string& path) {
        try {
            std::ifstream in(detail::native_delta_storage_sidecar_path(path), std::ios::binary);
            if (!in) {
                return false;
            }

            std::array<char, detail::kNativeStateMagic.size()> magic{};
            in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
            if (!in || magic != detail::kNativeStateMagic) {
                throw std::runtime_error("invalid native delta sidecar header");
            }

            const auto version = detail::read_pod<std::uint32_t>(in);
            if (version != detail::kNativeStateVersion) {
                throw std::runtime_error("unsupported native delta sidecar version");
            }

            const auto expected_live_size = detail::read_pod<std::uint64_t>(in);
            const auto entry_count = detail::read_pod<std::uint64_t>(in);
            std::vector<EntryLocation> entries(static_cast<std::size_t>(entry_count));
            if (!entries.empty()) {
                in.read(reinterpret_cast<char*>(entries.data()),
                        static_cast<std::streamsize>(entries.size() * sizeof(EntryLocation)));
                if (!in) {
                    throw std::runtime_error("failed to read native delta entry table");
                }
            }
            const auto bytes_size = detail::read_pod<std::uint64_t>(in);
            std::vector<char> bytes(static_cast<std::size_t>(bytes_size));
            if (!bytes.empty()) {
                in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (!in) {
                    throw std::runtime_error("failed to read native delta arena payload");
                }
            }

            arena_.restore_bytes(std::move(bytes));
            entries_by_id_ = std::move(entries);
            rebuild_indexes_from_storage();
            if (live_size_ != static_cast<std::size_t>(expected_live_size)) {
                throw std::runtime_error("native delta live-size mismatch");
            }
            return true;
        } catch (...) {
            clear();
            return false;
        }
    }

    [[nodiscard]] SegmentMemoryUsage memory_usage() const noexcept {
        SegmentMemoryUsage usage;
        usage.live_string_bytes = live_string_bytes();
        usage.arena_bytes = arena_.bytes_reserved();
        usage.arena_slack_bytes = arena_.bytes_reserved() >= arena_.bytes_used()
                                      ? (arena_.bytes_reserved() - arena_.bytes_used())
                                      : 0;
        usage.entry_table_bytes = entries_by_id_.capacity() * sizeof(EntryLocation);
        usage.id_hole_bytes = entries_by_id_.size() >= live_size_
                                  ? (entries_by_id_.size() - live_size_) * sizeof(EntryLocation)
                                  : 0;
        const auto fallback_usage = detail::estimate_map_memory(fallback_index_);
        usage.fallback_index_bytes = fallback_usage.total_bytes;
        usage.fallback_index_bucket_bytes = fallback_usage.bucket_bytes;
        usage.fallback_index_key_bytes = fallback_usage.key_bytes;
        usage.fallback_index_node_bytes = fallback_usage.node_bytes;
#if defined(STRING_BIMAP_HAS_ARRAY_HASH)
        if (use_array_map_index()) {
            usage.compact_index_bytes += detail::estimate_map_memory_bytes(array_map_index_);
            usage.fallback_index_bytes = 0;
            usage.fallback_index_bucket_bytes = 0;
            usage.fallback_index_key_bytes = 0;
            usage.fallback_index_node_bytes = 0;
        }
#endif
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
        clear_indexes();
#if defined(STRING_BIMAP_HAS_ARRAY_HASH)
        array_map_index_.clear();
#endif
        live_size_ = 0;
    }

private:
    [[nodiscard]] std::size_t live_string_bytes() const noexcept {
        std::size_t total = 0;
        for (const auto& entry : entries_by_id_) {
            if (entry.live()) {
                total += entry.length;
            }
        }
        return total;
    }

    void clear_indexes() {
        fallback_index_.clear();
#if defined(STRING_BIMAP_HAS_ARRAY_HASH)
        array_map_index_.clear();
#endif
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
        compact_index_.clear();
#endif
    }

    void rebuild_indexes_from_storage() {
        clear_indexes();
        live_size_ = 0;
        if (!use_compact_index()) {
            detail::map_reserve(fallback_index_, entries_by_id_.size());
        }
        for (StringId id = 0; id < entries_by_id_.size(); ++id) {
            if (!entries_by_id_[id].live()) {
                continue;
            }
            const auto value = arena_.view(entries_by_id_[id]);
            if (use_array_map_index()) {
#if defined(STRING_BIMAP_HAS_ARRAY_HASH)
                array_map_index_.insert(value, id);
#endif
            } else if (use_compact_index()) {
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
                compact_index_.insert(value, id);
#endif
            } else {
                detail::map_insert_or_assign(fallback_index_, value, id);
            }
            ++live_size_;
        }
    }

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
        return profile_ != BackendProfile::FastLookup &&
               profile_ != BackendProfile::FastLookupArrayMap &&
               profile_ != BackendProfile::CompactMemoryMarisaArrayMap;
#else
        return false;
#endif
    }

    [[nodiscard]] bool use_array_map_index() const noexcept {
#if defined(STRING_BIMAP_HAS_ARRAY_HASH)
        return profile_ == BackendProfile::FastLookupArrayMap ||
               profile_ == BackendProfile::CompactMemoryMarisaArrayMap;
#else
        return false;
#endif
    }

    PackedStringArena arena_;
    std::vector<EntryLocation> entries_by_id_;
    detail::StringIdMap fallback_index_;
#if defined(STRING_BIMAP_HAS_ARRAY_HASH)
    detail::ArrayStringIdMap array_map_index_;
#endif
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
    tsl::htrie_map<char, StringId> compact_index_;
#endif
    BackendProfile profile_;
    std::size_t live_size_ = 0;
};

}  // namespace string_bimap
