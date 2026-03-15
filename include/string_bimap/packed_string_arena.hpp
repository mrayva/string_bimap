#pragma once

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "types.hpp"

namespace string_bimap {

class PackedStringArena {
public:
    explicit PackedStringArena(std::size_t reserve_bytes = 0) {
        bytes_.reserve(reserve_bytes + 1);
        bytes_.push_back('\0');
    }

    [[nodiscard]] EntryLocation append(std::string_view value) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::length_error("string exceeds 32-bit arena limits");
        }
        if (bytes_.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - value.size() - 1) {
            throw std::length_error("arena exceeds 32-bit offset limits");
        }

        const auto offset = static_cast<std::uint32_t>(bytes_.size());
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        bytes_.push_back('\0');
        return EntryLocation{offset, static_cast<std::uint32_t>(value.size())};
    }

    [[nodiscard]] std::string_view view(const EntryLocation& location) const noexcept {
        if (!location.live()) {
            return {};
        }
        return std::string_view(bytes_.data() + location.offset, location.length);
    }

    [[nodiscard]] const char* c_str(const EntryLocation& location) const noexcept {
        if (!location.live()) {
            return nullptr;
        }
        return bytes_.data() + location.offset;
    }

    [[nodiscard]] std::size_t bytes_used() const noexcept {
        return bytes_.size();
    }

    [[nodiscard]] std::size_t bytes_reserved() const noexcept {
        return bytes_.capacity();
    }

    [[nodiscard]] const std::vector<char>& bytes() const noexcept {
        return bytes_;
    }

    void restore_bytes(std::vector<char> bytes) {
        if (bytes.empty() || bytes.front() != '\0') {
            throw std::runtime_error("packed string arena payload is missing the sentinel byte");
        }
        bytes_ = std::move(bytes);
    }

    void clear(std::size_t reserve_bytes = 0) {
        bytes_.clear();
        bytes_.reserve(reserve_bytes + 1);
        bytes_.push_back('\0');
    }

private:
    std::vector<char> bytes_;
};

}  // namespace string_bimap
