#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace string_bimap {

using StringId = std::uint32_t;

inline constexpr StringId kInvalidId = std::numeric_limits<StringId>::max();

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

using StringView = std::string_view;

}  // namespace string_bimap
