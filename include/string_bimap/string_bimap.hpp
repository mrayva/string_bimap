#pragma once

#include <cstddef>
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
    explicit StringBimap(std::size_t delta_reserve_bytes = 0)
        : delta_(delta_reserve_bytes) {}

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
        for_each_live([&](StringId id, std::string_view value) {
            if (value.substr(0, prefix.size()) == prefix) {
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
        if (version != detail::kFormatVersion) {
            throw std::runtime_error("unsupported serialized dictionary version");
        }

        StringBimap dict;
        dict.next_id_ = detail::read_pod<StringId>(in);

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
        return load(in);
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
    StringId next_id_ = 0;
    std::size_t live_size_ = 0;
};

}  // namespace string_bimap
