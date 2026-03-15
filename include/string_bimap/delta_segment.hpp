#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "detail/string_map.hpp"
#include "packed_string_arena.hpp"
#include "types.hpp"

namespace string_bimap {

class DeltaSegment {
public:
    explicit DeltaSegment(std::size_t reserve_bytes = 0)
        : arena_(reserve_bytes) {}

    [[nodiscard]] std::optional<StringId> find_id(std::string_view value) const {
        const auto it = detail::map_find(index_, value);
        if (it == index_.end()) {
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
        detail::map_insert_or_assign(index_, value, id);
        ++live_size_;
    }

    bool erase(StringId id) {
        if (!contains_id(id)) {
            return false;
        }
        const auto value = get_string(id);
        const auto it = detail::map_find(index_, value);
        if (it != index_.end() && detail::map_value(it) == id) {
            index_.erase(it);
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

    void clear(std::size_t reserve_bytes = 0) {
        arena_.clear(reserve_bytes);
        entries_by_id_.clear();
        index_.clear();
        live_size_ = 0;
    }

private:
    PackedStringArena arena_;
    std::vector<EntryLocation> entries_by_id_;
    detail::StringIdMap index_;
    std::size_t live_size_ = 0;
};

}  // namespace string_bimap
