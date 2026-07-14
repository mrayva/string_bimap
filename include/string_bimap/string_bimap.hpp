#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <ostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
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
// - Empty strings are ignored and never stored.
// - get_string(), for_each_live(), and for_each_with_prefix() return string_views
//   into internal storage. Any mutating operation or compact() invalidates them.
// - Thread safety is external. Concurrent mutation or mutation during iteration is unsupported.
class StringBimap {
public:
    explicit StringBimap(std::size_t delta_reserve_bytes = 0,
                         BackendProfile profile = BackendProfile::FastLookup)
        : base_(profile), delta_(delta_reserve_bytes, profile), profile_(profile) {}

    StringBimap(const StringBimap&) = delete;
    StringBimap& operator=(const StringBimap&) = delete;
    StringBimap(StringBimap&&) noexcept = default;
    StringBimap& operator=(StringBimap&&) noexcept = default;

    [[nodiscard]] BackendProfile backend_profile() const noexcept {
        return profile_;
    }

    // Returns the stable ID of a live string if present.
    [[nodiscard]] std::optional<StringId> find_id(std::string_view value) const {
        if (value.empty()) {
            return std::nullopt;
        }
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
        const auto value = try_get_string(id);
        return value.has_value() ? *value : std::string_view{};
    }

    // Returns a view to the live string stored under id, or std::nullopt if the
    // id is absent or deleted. The returned view is invalidated by any mutation
    // or by compact().
    [[nodiscard]] std::optional<std::string_view> try_get_string(StringId id) const {
        if (tombstones_.contains(id)) {
            return std::nullopt;
        }
        const auto delta_value = delta_.get_string(id);
        if (!delta_value.empty() || delta_.contains_id(id)) {
            return delta_value;
        }
        const auto base_value = base_.get_string(id);
        if (!base_value.empty() || base_.contains_id(id)) {
            return base_value;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool contains_id(StringId id) const {
        if (tombstones_.contains(id)) {
            return false;
        }
        return delta_.contains_id(id) || base_.contains_id(id);
    }

    // Inserts a non-empty string if it is not already live and returns its stable ID.
    // If the string already exists live, the existing ID is returned.
    // Empty strings are ignored and return kInvalidId.
    // IDs are never reused after deletion.
    [[nodiscard]] StringId insert(std::string_view value) {
        if (value.empty()) {
            return kInvalidId;
        }
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
        if (value.empty()) {
            return false;
        }
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

    [[nodiscard]] CompactionStats compaction_stats() const noexcept {
        return CompactionStats{
            next_id_,
            live_size_,
            base_.size(),
            delta_.size(),
            tombstones_.count(),
            delta_.bytes_used(),
        };
    }

    // Returns true if the current mixed base+delta state exceeds the supplied
    // heuristic thresholds and should be rewritten into a compacted base snapshot.
    [[nodiscard]] bool should_compact(const CompactionPolicy& policy = {}) const noexcept {
        const auto stats = compaction_stats();
        const bool delta_triggered =
            stats.delta_live_ids >= policy.min_delta_ids &&
            (stats.delta_fraction() >= policy.max_delta_fraction ||
             stats.delta_bytes >= policy.min_delta_bytes);
        const bool tombstone_triggered =
            stats.tombstone_ids >= policy.min_tombstone_ids &&
            stats.tombstone_fraction() >= policy.max_tombstone_fraction;
        return delta_triggered || tombstone_triggered;
    }

    // Compacts only if should_compact(policy) is true and returns whether a
    // compaction was performed.
    bool compact_if_needed(const CompactionPolicy& policy = {}) {
        if (!should_compact(policy)) {
            return false;
        }
        compact();
        return true;
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
        detail::write_pod(out, logical_fingerprint());

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
        out.close();
        if (!out) {
            throw std::runtime_error("failed to close dictionary serialization file: " + path);
        }

        if (base_.has_native_compact_index()) {
            base_.save_native_compact_index(path);
        } else {
            remove_compact_sidecars(path);
        }

        save_native_snapshot(path, logical_fingerprint());
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
        auto snapshot = load_logical_snapshot(in);
        StringBimap dict(0, snapshot.profile);
        dict.next_id_ = snapshot.next_id;
        dict.base_.rebuild(std::move(snapshot.items));
        dict.delta_.clear();
        dict.tombstones_.clear();
        dict.live_size_ = snapshot.live_size;
        return dict;
    }

    [[nodiscard]] static StringBimap load(const std::string& path) {
        if (auto native = try_load_native_snapshot(path)) {
            return std::move(*native);
        }

        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("failed to open file for dictionary deserialization: " + path);
        }
        auto snapshot = load_logical_snapshot(in);
        StringBimap dict(0, snapshot.profile);
        dict.next_id_ = snapshot.next_id;

        bool loaded_native_compact = false;
        const bool has_xcdat_sidecars =
            std::filesystem::exists(detail::compact_trie_sidecar_path(path)) &&
            std::filesystem::exists(detail::compact_ids_sidecar_path(path));
        const bool has_marisa_sidecars =
            std::filesystem::exists(detail::compact_marisa_sidecar_path(path)) &&
            std::filesystem::exists(detail::compact_ids_sidecar_path(path));
        if ((snapshot.profile == BackendProfile::CompactMemory && has_xcdat_sidecars) ||
            ((snapshot.profile == BackendProfile::CompactMemoryMarisa ||
              snapshot.profile == BackendProfile::CompactMemoryMarisaArrayMap) &&
             has_marisa_sidecars)) {
            loaded_native_compact = dict.base_.load_native_compact_index(snapshot.items, path);
        }
        if (!loaded_native_compact) {
            dict.base_.rebuild(std::move(snapshot.items));
            if ((snapshot.profile == BackendProfile::CompactMemory ||
                 snapshot.profile == BackendProfile::CompactMemoryMarisa ||
                 snapshot.profile == BackendProfile::CompactMemoryMarisaArrayMap) &&
                dict.base_.has_native_compact_index()) {
                dict.base_.save_native_compact_index(path);
            }
        }
        dict.delta_.clear();
        dict.tombstones_.clear();
        dict.live_size_ = snapshot.live_size;
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
    struct LogicalSnapshot {
        StringId next_id = 0;
        BackendProfile profile = BackendProfile::FastLookup;
        std::size_t live_size = 0;
        std::vector<BaseSegment::BuildItem> items;
    };

    [[nodiscard]] static LogicalSnapshot load_logical_snapshot(std::istream& in) {
        std::array<char, detail::kFileMagic.size()> magic{};
        in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (!in || magic != detail::kFileMagic) {
            throw std::runtime_error("invalid serialized dictionary header");
        }

        const auto version = detail::read_pod<std::uint32_t>(in);
        if (version < 1 || version > detail::kFormatVersion) {
            throw std::runtime_error("unsupported serialized dictionary version");
        }

        LogicalSnapshot snapshot;
        snapshot.next_id = detail::read_pod<StringId>(in);
        if (version >= 2) {
            snapshot.profile = require_profile(detail::read_pod<std::uint8_t>(in));
        }

        const auto live_count = detail::read_pod<std::uint64_t>(in);
        if (live_count > snapshot.next_id) {
            throw std::runtime_error("serialized dictionary live count exceeds its ID range");
        }
        snapshot.live_size = static_cast<std::size_t>(live_count);
        const auto stored_fingerprint =
            version >= 3 ? detail::read_pod<std::uint64_t>(in) : 0;

        auto remaining = detail::remaining_bytes(in);
        constexpr std::uint64_t kMinimumSerializedEntrySize =
            sizeof(StringId) + sizeof(std::uint64_t) + 1;
        if (remaining && live_count > *remaining / kMinimumSerializedEntrySize) {
            throw std::runtime_error("serialized dictionary entry count exceeds remaining payload");
        }

        constexpr std::size_t kMaximumInitialReserve = 65536;
        const auto reserve_count = static_cast<std::size_t>(
            std::min<std::uint64_t>(live_count, kMaximumInitialReserve));
        snapshot.items.reserve(reserve_count);
        std::unordered_set<std::string> seen_values;
        seen_values.reserve(reserve_count);
        std::unordered_set<StringId> seen_ids;
        seen_ids.reserve(reserve_count);

        for (std::uint64_t i = 0; i < live_count; ++i) {
            constexpr std::uint64_t kSerializedEntryHeaderSize =
                sizeof(StringId) + sizeof(std::uint64_t);
            if (remaining && *remaining < kSerializedEntryHeaderSize) {
                throw std::runtime_error("serialized dictionary entry header is truncated");
            }
            const auto id = detail::read_pod<StringId>(in);
            const auto size = detail::read_pod<std::uint64_t>(in);
            if (remaining) {
                *remaining -= kSerializedEntryHeaderSize;
            }
            if (id >= snapshot.next_id || size == 0 ||
                size > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("invalid serialized dictionary entry");
            }
            if (remaining && size > *remaining) {
                throw std::runtime_error("serialized dictionary string exceeds remaining payload");
            }
            const auto value = remaining
                                   ? detail::read_string(in, static_cast<std::size_t>(size))
                                   : detail::read_string_incremental(in, static_cast<std::size_t>(size));
            if (remaining) {
                *remaining -= size;
            }
            if (!seen_ids.insert(id).second || !seen_values.insert(value).second) {
                throw std::runtime_error("duplicate serialized dictionary entry");
            }
            snapshot.items.push_back(BaseSegment::BuildItem{id, value});
        }
        if (version >= 3 &&
            fingerprint(snapshot.next_id, snapshot.profile, snapshot.items) != stored_fingerprint) {
            throw std::runtime_error("serialized dictionary fingerprint mismatch");
        }
        return snapshot;
    }

    [[nodiscard]] static BackendProfile normalize_profile(std::uint8_t raw) noexcept {
        switch (static_cast<BackendProfile>(raw)) {
            case BackendProfile::FastLookup:
            case BackendProfile::CompactMemory:
            case BackendProfile::CompactMemoryMarisa:
            case BackendProfile::FastLookupArrayMap:
            case BackendProfile::CompactMemoryMarisaArrayMap:
                return static_cast<BackendProfile>(raw);
        }
        return BackendProfile::CompactMemoryMarisa;
    }

    [[nodiscard]] static BackendProfile require_profile(std::uint8_t raw) {
        const auto profile = normalize_profile(raw);
        if (static_cast<std::uint8_t>(profile) != raw) {
            throw std::runtime_error("invalid serialized dictionary backend profile");
        }
        return profile;
    }

    static void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
    }

    template <class T>
    static void hash_value(std::uint64_t& hash, const T& value) noexcept {
        hash_bytes(hash, &value, sizeof(value));
    }

    [[nodiscard]] static std::uint64_t fingerprint(
        StringId next_id, BackendProfile profile,
        const std::vector<BaseSegment::BuildItem>& items) noexcept {
        std::uint64_t hash = 14695981039346656037ULL;
        hash_value(hash, next_id);
        hash_value(hash, profile);
        const auto count = static_cast<std::uint64_t>(items.size());
        hash_value(hash, count);
        for (const auto& item : items) {
            hash_value(hash, item.id);
            const auto size = static_cast<std::uint64_t>(item.value.size());
            hash_value(hash, size);
            hash_bytes(hash, item.value.data(), item.value.size());
        }
        return hash;
    }

    [[nodiscard]] std::uint64_t logical_fingerprint() const noexcept {
        std::uint64_t hash = 14695981039346656037ULL;
        hash_value(hash, next_id_);
        hash_value(hash, profile_);
        const auto count = static_cast<std::uint64_t>(live_size_);
        hash_value(hash, count);
        for_each_live([&](StringId id, std::string_view value) {
            hash_value(hash, id);
            const auto size = static_cast<std::uint64_t>(value.size());
            hash_value(hash, size);
            hash_bytes(hash, value.data(), value.size());
        });
        return hash;
    }

    struct NativeSnapshotHeader {
        std::array<char, detail::kNativeStateMagic.size()> magic = detail::kNativeStateMagic;
        std::uint32_t version = detail::kNativeStateVersion;
        StringId next_id = 0;
        BackendProfile profile = BackendProfile::FastLookup;
        std::uint64_t live_size = 0;
        std::uint64_t tombstone_word_count = 0;
        std::uint64_t logical_fingerprint = 0;
    };

    void save_native_snapshot(const std::string& path, std::uint64_t logical_fingerprint) const {
        try {
            base_.save_native_storage(path);
            if (delta_.size() != 0) {
                delta_.save_native_storage(path);
            } else {
                std::error_code ec;
                std::filesystem::remove(detail::native_delta_storage_sidecar_path(path), ec);
            }

            std::ofstream out(detail::native_state_sidecar_path(path), std::ios::binary);
            if (!out) {
                throw std::runtime_error("failed to open native snapshot metadata for writing");
            }

            NativeSnapshotHeader header;
            header.next_id = next_id_;
            header.profile = profile_;
            header.live_size = static_cast<std::uint64_t>(live_size_);
            header.tombstone_word_count =
                static_cast<std::uint64_t>(tombstones_.words().size());
            header.logical_fingerprint = logical_fingerprint;

            detail::write_bytes(out, header.magic.data(), header.magic.size());
            detail::write_pod(out, header.version);
            detail::write_pod(out, header.next_id);
            detail::write_pod(out, header.profile);
            detail::write_pod(out, header.live_size);
            detail::write_pod(out, header.tombstone_word_count);
            detail::write_pod(out, header.logical_fingerprint);
            if (!tombstones_.words().empty()) {
                out.write(reinterpret_cast<const char*>(tombstones_.words().data()),
                          static_cast<std::streamsize>(tombstones_.words().size() * sizeof(std::uint64_t)));
                if (!out) {
                    throw std::runtime_error("failed to write native snapshot tombstones");
                }
            }
        } catch (...) {
            remove_native_snapshot_sidecars(path);
            throw;
        }
    }

    [[nodiscard]] static std::optional<StringBimap> try_load_native_snapshot(const std::string& path) {
        if (!std::filesystem::exists(detail::native_state_sidecar_path(path)) ||
            !std::filesystem::exists(detail::native_base_storage_sidecar_path(path))) {
            return std::nullopt;
        }

        try {
            std::ifstream logical(path, std::ios::binary);
            if (!logical) {
                return std::nullopt;
            }
            std::array<char, detail::kFileMagic.size()> logical_magic{};
            logical.read(logical_magic.data(), static_cast<std::streamsize>(logical_magic.size()));
            if (!logical || logical_magic != detail::kFileMagic ||
                detail::read_pod<std::uint32_t>(logical) != detail::kFormatVersion) {
                return std::nullopt;
            }
            const auto logical_next_id = detail::read_pod<StringId>(logical);
            const auto logical_profile = require_profile(detail::read_pod<std::uint8_t>(logical));
            const auto logical_live_size = detail::read_pod<std::uint64_t>(logical);
            const auto logical_fingerprint = detail::read_pod<std::uint64_t>(logical);

            std::ifstream in(detail::native_state_sidecar_path(path), std::ios::binary);
            if (!in) {
                return std::nullopt;
            }

            std::array<char, detail::kNativeStateMagic.size()> magic{};
            in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
            if (!in || magic != detail::kNativeStateMagic) {
                return std::nullopt;
            }

            const auto version = detail::read_pod<std::uint32_t>(in);
            if (version != detail::kNativeStateVersion) {
                return std::nullopt;
            }

            const auto next_id = detail::read_pod<StringId>(in);
            const auto profile = require_profile(detail::read_pod<std::uint8_t>(in));
            const auto live_size = detail::read_pod<std::uint64_t>(in);
            const auto tombstone_word_count = detail::read_pod<std::uint64_t>(in);
            const auto native_fingerprint = detail::read_pod<std::uint64_t>(in);
            if (next_id != logical_next_id || profile != logical_profile ||
                live_size != logical_live_size || native_fingerprint != logical_fingerprint) {
                return std::nullopt;
            }
            const auto max_tombstone_words =
                (static_cast<std::uint64_t>(next_id) + 63U) / 64U;
            if (tombstone_word_count > max_tombstone_words) {
                return std::nullopt;
            }
            std::vector<std::uint64_t> tombstone_words(static_cast<std::size_t>(tombstone_word_count));
            if (!tombstone_words.empty()) {
                in.read(reinterpret_cast<char*>(tombstone_words.data()),
                        static_cast<std::streamsize>(tombstone_words.size() * sizeof(std::uint64_t)));
                if (!in) {
                    return std::nullopt;
                }
            }

            StringBimap dict(0, profile);
            dict.next_id_ = next_id;
            dict.live_size_ = static_cast<std::size_t>(live_size);
            dict.tombstones_.restore_words(std::move(tombstone_words));

            if (!dict.base_.load_native_storage(path)) {
                return std::nullopt;
            }

            const bool loaded_base_lookup =
                dict.base_.load_native_compact_index_from_storage(path);
            if (!loaded_base_lookup) {
                dict.base_.rebuild_lookup_from_storage();
            }

            if (std::filesystem::exists(detail::native_delta_storage_sidecar_path(path))) {
                if (!dict.delta_.load_native_storage(path)) {
                    return std::nullopt;
                }
            } else {
                dict.delta_.clear();
            }

            if (dict.logical_fingerprint() != logical_fingerprint) {
                return std::nullopt;
            }

            return dict;
        } catch (...) {
            return std::nullopt;
        }
    }

    static void remove_compact_sidecars(const std::string& path) {
        std::error_code ec;
        std::filesystem::remove(detail::compact_trie_sidecar_path(path), ec);
        std::filesystem::remove(detail::compact_marisa_sidecar_path(path), ec);
        std::filesystem::remove(detail::compact_ids_sidecar_path(path), ec);
    }

    static void remove_native_snapshot_sidecars(const std::string& path) {
        std::error_code ec;
        std::filesystem::remove(detail::native_state_sidecar_path(path), ec);
        std::filesystem::remove(detail::native_base_storage_sidecar_path(path), ec);
        std::filesystem::remove(detail::native_delta_storage_sidecar_path(path), ec);
    }

    BaseSegment base_;
    DeltaSegment delta_;
    Tombstones tombstones_;
    BackendProfile profile_ = BackendProfile::FastLookup;
    StringId next_id_ = 0;
    std::size_t live_size_ = 0;
};

}  // namespace string_bimap

#include "pthash_bimap.hpp"
