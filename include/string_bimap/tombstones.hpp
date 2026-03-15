#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "types.hpp"

namespace string_bimap {

class Tombstones {
public:
    [[nodiscard]] bool contains(StringId id) const noexcept {
        const auto word_index = static_cast<std::size_t>(id / 64);
        const auto bit_index = static_cast<std::size_t>(id % 64);
        if (word_index >= words_.size()) {
            return false;
        }
        return (words_[word_index] & (std::uint64_t{1} << bit_index)) != 0;
    }

    bool add(StringId id) {
        const auto word_index = static_cast<std::size_t>(id / 64);
        const auto bit_index = static_cast<std::size_t>(id % 64);
        if (word_index >= words_.size()) {
            words_.resize(word_index + 1, 0);
        }
        const auto mask = std::uint64_t{1} << bit_index;
        const bool already_set = (words_[word_index] & mask) != 0;
        words_[word_index] |= mask;
        return !already_set;
    }

    bool remove(StringId id) noexcept {
        const auto word_index = static_cast<std::size_t>(id / 64);
        const auto bit_index = static_cast<std::size_t>(id % 64);
        if (word_index >= words_.size()) {
            return false;
        }
        const auto mask = std::uint64_t{1} << bit_index;
        const bool already_set = (words_[word_index] & mask) != 0;
        words_[word_index] &= ~mask;
        return already_set;
    }

    void clear() noexcept {
        words_.clear();
    }

    [[nodiscard]] bool empty() const noexcept {
        return std::all_of(words_.begin(), words_.end(), [](std::uint64_t word) { return word == 0; });
    }

    [[nodiscard]] std::size_t memory_usage_bytes() const noexcept {
        return sizeof(*this) + words_.capacity() * sizeof(std::uint64_t);
    }

private:
    std::vector<std::uint64_t> words_;
};

}  // namespace string_bimap
