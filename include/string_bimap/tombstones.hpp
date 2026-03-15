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

    [[nodiscard]] std::size_t count() const noexcept {
        std::size_t total = 0;
        for (const auto word : words_) {
            total += static_cast<std::size_t>(popcount(word));
        }
        return total;
    }

    [[nodiscard]] const std::vector<std::uint64_t>& words() const noexcept {
        return words_;
    }

    void restore_words(std::vector<std::uint64_t> words) {
        words_ = std::move(words);
    }

private:
    static unsigned popcount(std::uint64_t value) noexcept {
#if defined(__clang__) || defined(__GNUC__)
        return static_cast<unsigned>(__builtin_popcountll(value));
#else
        unsigned count = 0;
        while (value != 0) {
            value &= (value - 1);
            ++count;
        }
        return count;
#endif
    }

    std::vector<std::uint64_t> words_;
};

}  // namespace string_bimap
