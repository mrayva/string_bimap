#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace string_bimap {

using StringId = std::uint32_t;

inline constexpr StringId kInvalidId = std::numeric_limits<StringId>::max();

enum class BackendProfile : std::uint8_t {
    FastLookup = 0,
    CompactMemory = 1,
    CompactMemoryMarisa = 2,
    CompactMemoryFst = 3,
};

struct LookupResult {
    std::optional<StringId> id;

    [[nodiscard]] bool found() const noexcept {
        return id.has_value();
    }
};

struct EntryLocation {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;

    [[nodiscard]] bool live() const noexcept {
        return offset != 0;
    }
};

struct SegmentMemoryUsage {
    std::size_t arena_bytes = 0;
    std::size_t entry_table_bytes = 0;
    std::size_t fallback_index_bytes = 0;
    std::size_t compact_index_bytes = 0;
    std::size_t auxiliary_bytes = 0;

    [[nodiscard]] std::size_t total_bytes() const noexcept {
        return arena_bytes + entry_table_bytes + fallback_index_bytes + compact_index_bytes + auxiliary_bytes;
    }
};

struct StringBimapMemoryUsage {
    SegmentMemoryUsage base;
    SegmentMemoryUsage delta;
    std::size_t tombstone_bytes = 0;
    std::size_t bookkeeping_bytes = 0;

    [[nodiscard]] std::size_t total_bytes() const noexcept {
        return base.total_bytes() + delta.total_bytes() + tombstone_bytes + bookkeeping_bytes;
    }
};

using StringView = std::string_view;

}  // namespace string_bimap
