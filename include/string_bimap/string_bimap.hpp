#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <istream>
#include <ostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base_segment.hpp"
#include "delta_segment.hpp"
#include "serialization.hpp"
#include "tombstones.hpp"
#include "types.hpp"

namespace string_bimap {

// StringBimap is an in-memory dictionary with stable IDs.
//
// Contract:
// - IDs are monotonically assigned and never reused.
// - Deleting a string removes it logically; the old ID becomes a permanent hole.
// - compact() preserves all live IDs and may rewrite internal storage.
// - get_string(), for_each_live(), and for_each_with_prefix() return string_views
//   into internal storage. Any mutating operation or compact() invalidates them.
// - Thread safety is external. Concurrent mutation or mutation during iteration is unsupported.
class StringBimap {
public:
    explicit StringBimap(std::size_t delta_reserve_bytes = 0,
                         BackendProfile profile = BackendProfile::FastLookup)
        : base_(profile), delta_(delta_reserve_bytes, profile), profile_(profile) {}

    [[nodiscard]] BackendProfile backend_profile() const noexcept {
        return profile_;
    }

    // Returns the stable ID of a live string if present.
    [[nodiscard]] std::optional<StringId> find_id(std::string_view value) const {
        if (auto id = delta_.find_id(value)) {
            if (!tombstones_.contains(*id)) {
                return id;
            }
        }
        if (auto id = base_.find_id(value)) {
            if (!tombstones_.contains(*id)) {
                return id;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool contains(std::string_view value) const {
        return find_id(value).has_value();
    }

    // Returns a view to the live string stored under id, or an empty view if the
    // id is absent or deleted. The returned view is invalidated by any mutation
    // or by compact().
    [[nodiscard]] std::string_view get_string(StringId id) const {
        if (tombstones_.contains(id)) {
            return {};
        }
        const auto delta_value = delta_.get_string(id);
        if (!delta_value.empty() || delta_.contains_id(id)) {
            return delta_value;
        }
        return base_.get_string(id);
    }

    [[nodiscard]] bool contains_id(StringId id) const {
        if (tombstones_.contains(id)) {
            return false;
        }
        return delta_.contains_id(id) || base_.contains_id(id);
    }

    // Inserts a string if it is not already live and returns its stable ID.
    // If the string already exists live, the existing ID is returned.
    // IDs are never reused after deletion.
    [[nodiscard]] StringId insert(std::string_view value) {
        if (const auto existing = find_id(value)) {
            return *existing;
        }
        if (next_id_ == kInvalidId) {
            throw std::overflow_error("StringId space exhausted");
        }
        const auto id = next_id_++;
        delta_.insert(id, value);
        ++live_size_;
        return id;
    }

    // Logically deletes a live ID. The ID is never reused.
    bool erase(StringId id) {
        if (!contains_id(id)) {
            return false;
        }
        if (tombstones_.add(id)) {
            if (delta_.contains_id(id)) {
                delta_.erase(id);
            }
            --live_size_;
            return true;
        }
        return false;
    }

    bool erase(std::string_view value) {
        const auto id = find_id(value);
        return id.has_value() && erase(*id);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return next_id_;
    }

    [[nodiscard]] std::size_t live_size() const noexcept {
        return live_size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return live_size_ == 0;
    }

    [[nodiscard]] StringBimapMemoryUsage memory_usage() const noexcept {
        StringBimapMemoryUsage usage;
        usage.base = base_.memory_usage();
        usage.delta = delta_.memory_usage();
        usage.tombstone_bytes = tombstones_.memory_usage_bytes();
        usage.bookkeeping_bytes = sizeof(*this);
        return usage;
    }

    // Iterates over live entries in increasing stable ID order.
    // The callback receives (StringId, std::string_view).
    // Mutating the dictionary from inside the callback is unsupported.
    template <class Func>
    void for_each_live(Func&& func) const {
        for (StringId id = 0; id < next_id_; ++id) {
            if (!contains_id(id)) {
                continue;
            }
            func(id, get_string(id));
        }
    }

    // Iterates over live entries whose value starts with prefix, in increasing
    // stable ID order. Mutating the dictionary from inside the callback is unsupported.
    template <class Func>
    void for_each_with_prefix(std::string_view prefix, Func&& func) const {
        std::vector<StringId> ids;
        base_.for_each_with_prefix(prefix, [&](StringId id, std::string_view) {
            if (!tombstones_.contains(id)) {
                ids.push_back(id);
            }
        });
        delta_.for_each_with_prefix(prefix, [&](StringId id, std::string_view) {
            if (!tombstones_.contains(id)) {
                ids.push_back(id);
            }
        });
        std::sort(ids.begin(), ids.end());
        for (const auto id : ids) {
            func(id, get_string(id));
        }
    }

    // Iterates over live entries whose value starts with prefix, but does not
    // guarantee stable ID order. This may be faster than for_each_with_prefix()
    // when the underlying trie backends can enumerate matches directly.
    // Mutating the dictionary from inside the callback is unsupported.
    template <class Func>
    void for_each_with_prefix_unordered(std::string_view prefix, Func&& func) const {
        base_.for_each_with_prefix_unordered(prefix, [&](StringId id, std::string_view value) {
            if (!tombstones_.contains(id)) {
                func(id, value);
            }
        });
        delta_.for_each_with_prefix_unordered(prefix, [&](StringId id, std::string_view value) {
            if (!tombstones_.contains(id)) {
                func(id, value);
            }
        });
    }

    // Serializes the logical dictionary state. The format preserves stable IDs,
    // deleted holes, and live strings, but not the exact internal base/delta split.
    void save(std::ostream& out) const {
        detail::write_bytes(out, detail::kFileMagic.data(), detail::kFileMagic.size());
        detail::write_pod(out, detail::kFormatVersion);
        detail::write_pod(out, next_id_);
        detail::write_pod(out, profile_);
        detail::write_pod(out, static_cast<std::uint64_t>(live_size_));

        for_each_live([&](StringId id, std::string_view value) {
            detail::write_pod(out, id);
            detail::write_pod(out, static_cast<std::uint64_t>(value.size()));
            detail::write_bytes(out, value.data(), value.size());
        });
    }

    void save(const std::string& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            throw std::runtime_error("failed to open file for dictionary serialization: " + path);
        }
        save(out);

#if defined(STRING_BIMAP_HAS_XCDAT)
        const auto trie_path = detail::compact_trie_sidecar_path(path);
        const auto ids_path = detail::compact_ids_sidecar_path(path);
        if (base_.has_native_compact_index() && delta_.size() == 0 && tombstones_.empty()) {
            base_.save_native_compact_index(trie_path, ids_path);
        } else {
            std::error_code ec;
            std::filesystem::remove(trie_path, ec);
            std::filesystem::remove(ids_path, ec);
        }
#endif
    }

    // Saves a compacted snapshot to path without mutating the original dictionary.
    // This is the easiest way to guarantee native compact sidecars for file persistence.
    void save_compacted(const std::string& path) const {
        StringBimap snapshot(0, profile_);
        snapshot.next_id_ = next_id_;
        snapshot.live_size_ = live_size_;
        std::vector<BaseSegment::BuildItem> items;
        items.reserve(live_size_);
        for_each_live([&](StringId id, std::string_view value) {
            items.push_back(BaseSegment::BuildItem{id, std::string(value)});
        });
        snapshot.base_.rebuild(std::move(items));
        snapshot.delta_.clear();
        snapshot.tombstones_.clear();
        snapshot.save(path);
    }

    // Loads a dictionary previously saved with save(). The loaded dictionary is
    // logically equivalent to the saved one, but its internal storage layout may differ.
    [[nodiscard]] static StringBimap load(std::istream& in) {
        std::array<char, detail::kFileMagic.size()> magic{};
        in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (!in || magic != detail::kFileMagic) {
            throw std::runtime_error("invalid serialized dictionary header");
        }

        const auto version = detail::read_pod<std::uint32_t>(in);
        if (version != 1 && version != detail::kFormatVersion) {
            throw std::runtime_error("unsupported serialized dictionary version");
        }

        const auto next_id = detail::read_pod<StringId>(in);
        BackendProfile profile = BackendProfile::FastLookup;
        if (version >= 2) {
            profile = detail::read_pod<BackendProfile>(in);
        }

        StringBimap dict(0, profile);
        dict.next_id_ = next_id;

        const auto live_count = detail::read_pod<std::uint64_t>(in);
        std::vector<BaseSegment::BuildItem> items;
        items.reserve(static_cast<std::size_t>(live_count));

        for (std::uint64_t i = 0; i < live_count; ++i) {
            const auto id = detail::read_pod<StringId>(in);
            const auto size = detail::read_pod<std::uint64_t>(in);
            const auto value = detail::read_string(in, static_cast<std::size_t>(size));
            items.push_back(BaseSegment::BuildItem{id, value});
        }

        dict.base_.rebuild(std::move(items));
        dict.delta_.clear();
        dict.tombstones_.clear();
        dict.live_size_ = static_cast<std::size_t>(live_count);
        return dict;
    }

    [[nodiscard]] static StringBimap load(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("failed to open file for dictionary deserialization: " + path);
        }
        std::array<char, detail::kFileMagic.size()> magic{};
        in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (!in || magic != detail::kFileMagic) {
            throw std::runtime_error("invalid serialized dictionary header");
        }

        const auto version = detail::read_pod<std::uint32_t>(in);
        if (version != 1 && version != detail::kFormatVersion) {
            throw std::runtime_error("unsupported serialized dictionary version");
        }

        const auto next_id = detail::read_pod<StringId>(in);
        BackendProfile profile = BackendProfile::FastLookup;
        if (version >= 2) {
            profile = detail::read_pod<BackendProfile>(in);
        }

        StringBimap dict(0, profile);
        dict.next_id_ = next_id;

        const auto live_count = detail::read_pod<std::uint64_t>(in);
        std::vector<BaseSegment::BuildItem> items;
        items.reserve(static_cast<std::size_t>(live_count));

        for (std::uint64_t i = 0; i < live_count; ++i) {
            const auto id = detail::read_pod<StringId>(in);
            const auto size = detail::read_pod<std::uint64_t>(in);
            const auto value = detail::read_string(in, static_cast<std::size_t>(size));
            items.push_back(BaseSegment::BuildItem{id, value});
        }

#if defined(STRING_BIMAP_HAS_XCDAT)
        bool loaded_native_compact = false;
        const auto trie_path = detail::compact_trie_sidecar_path(path);
        const auto ids_path = detail::compact_ids_sidecar_path(path);
        if (profile == BackendProfile::CompactMemory) {
            if (std::filesystem::exists(trie_path) && std::filesystem::exists(ids_path)) {
                loaded_native_compact = dict.base_.load_native_compact_index(std::move(items), trie_path, ids_path);
            }
        }
        if (!loaded_native_compact) {
            dict.base_.rebuild(std::move(items));
            if (profile == BackendProfile::CompactMemory && dict.base_.has_native_compact_index()) {
                dict.base_.save_native_compact_index(trie_path, ids_path);
            }
        }
#else
        dict.base_.rebuild(std::move(items));
#endif
        dict.delta_.clear();
        dict.tombstones_.clear();
        dict.live_size_ = static_cast<std::size_t>(live_count);
        return dict;
    }

    // Rebuilds internal storage while preserving all live IDs.
    // All string_views previously returned by this object are invalidated.
    void compact() {
        std::vector<BaseSegment::BuildItem> items;
        items.reserve(live_size_);

        for (StringId id = 0; id < next_id_; ++id) {
            if (tombstones_.contains(id)) {
                continue;
            }
            const auto value = get_string(id);
            if (!value.empty() || contains_id(id)) {
                items.push_back(BaseSegment::BuildItem{id, std::string(value)});
            }
        }

        base_.rebuild(std::move(items));
        delta_.clear();
        tombstones_.clear();
    }

private:
    BaseSegment base_;
    DeltaSegment delta_;
    Tombstones tombstones_;
    BackendProfile profile_ = BackendProfile::FastLookup;
    StringId next_id_ = 0;
    std::size_t live_size_ = 0;
};

}  // namespace string_bimap
