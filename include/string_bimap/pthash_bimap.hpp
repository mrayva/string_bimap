#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "types.hpp"

#if defined(STRING_BIMAP_HAS_PTHASH)
#include <xxh3.h>

#include <pthash.hpp>
#endif

namespace string_bimap {

enum class PthashIdWidth : std::uint8_t {
    U8 = 1,
    U16 = 2,
    U32 = 4,
};

using PthashCompactId = std::variant<std::uint8_t, std::uint16_t, std::uint32_t>;

struct PthashBuildOptions {
    std::uint64_t initial_seed = 0;
    std::uint64_t max_seed_attempts = 1024;
    double lambda = 4.5;
};

struct PthashBimapMemoryUsage {
    std::size_t string_bytes = 0;
    std::size_t string_vector_bytes = 0;
    std::size_t mph_bytes = 0;
    std::size_t bookkeeping_bytes = 0;

    [[nodiscard]] std::size_t total_bytes() const noexcept {
        return string_bytes + string_vector_bytes + mph_bytes + bookkeeping_bytes;
    }
};

namespace detail {

inline constexpr std::string_view kPthashFileMagic = "STRBMPH1";
inline constexpr std::uint32_t kPthashFormatVersion = 1;

template <class T>
inline void write_pod_local(std::ostream& out, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>, "POD required");
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!out) {
        throw std::runtime_error("failed to write pthash bimap data");
    }
}

template <class T>
inline T read_pod_local(std::istream& in) {
    static_assert(std::is_trivially_copyable_v<T>, "POD required");
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!in) {
        throw std::runtime_error("failed to read pthash bimap data");
    }
    return value;
}

inline void write_string(std::ostream& out, std::string_view value) {
    const auto length = static_cast<std::uint32_t>(value.size());
    write_pod_local(out, length);
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!out) {
        throw std::runtime_error("failed to write pthash bimap string");
    }
}

inline std::string read_string(std::istream& in) {
    const auto length = read_pod_local<std::uint32_t>(in);
    std::string value(length, '\0');
    in.read(value.data(), static_cast<std::streamsize>(length));
    if (!in) {
        throw std::runtime_error("failed to read pthash bimap string");
    }
    return value;
}

constexpr PthashIdWidth select_pthash_id_width(std::size_t size) {
    if (size <= static_cast<std::size_t>(std::numeric_limits<std::uint8_t>::max()) + 1U) {
        return PthashIdWidth::U8;
    }
    if (size <= static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1ULL) {
        return PthashIdWidth::U16;
    }
    return PthashIdWidth::U32;
}

#if defined(STRING_BIMAP_HAS_PTHASH)
struct pthash_string_view_xxhash_128 {
    using hash_type = pthash::hash128;

    static inline hash_type hash(std::string_view value, std::uint64_t seed) {
        return XXH128(value.data(), value.size(), seed);
    }

    static inline hash_type hash(const std::string& value, std::uint64_t seed) {
        return XXH128(value.data(), value.size(), seed);
    }
};

using PthashFunction =
    pthash::single_phf<pthash_string_view_xxhash_128, pthash::range_bucketer,
                       pthash::dictionary_dictionary, true>;
#endif

}  // namespace detail

class PthashBimap {
public:
    PthashBimap() = default;

    explicit PthashBimap(const std::vector<std::string>& values,
                         const PthashBuildOptions& options = {}) {
        rebuild(values, options);
    }

    explicit PthashBimap(const std::vector<std::string_view>& values,
                         const PthashBuildOptions& options = {}) {
        rebuild(values, options);
    }

    template <class InputIt>
    PthashBimap(InputIt first, InputIt last, const PthashBuildOptions& options = {}) {
        rebuild(first, last, options);
    }

    [[nodiscard]] static constexpr bool available() noexcept {
#if defined(STRING_BIMAP_HAS_PTHASH)
        return true;
#else
        return false;
#endif
    }

    [[nodiscard]] static constexpr PthashIdWidth required_id_width(std::size_t size) noexcept {
        return detail::select_pthash_id_width(size);
    }

    template <class InputIt>
    void rebuild(InputIt first, InputIt last, const PthashBuildOptions& options = {}) {
        std::vector<std::string> values;
        for (auto it = first; it != last; ++it) {
            values.emplace_back(*it);
        }
        rebuild(values, options);
    }

    void rebuild(const std::vector<std::string_view>& values, const PthashBuildOptions& options = {}) {
        std::vector<std::string> owned;
        owned.reserve(values.size());
        for (const auto value : values) {
            owned.emplace_back(value);
        }
        rebuild(owned, options);
    }

    void rebuild(const std::vector<std::string>& values, const PthashBuildOptions& options = {}) {
#if !defined(STRING_BIMAP_HAS_PTHASH)
        (void)values;
        (void)options;
        throw std::logic_error("PthashBimap requires STRING_BIMAP_HAS_PTHASH");
#else
        PthashBimap next;
        next.build_from_values(values, options);
        *this = std::move(next);
#endif
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return strings_by_id_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return strings_by_id_.empty();
    }

    [[nodiscard]] PthashIdWidth id_width() const noexcept {
        return id_width_;
    }

    [[nodiscard]] std::size_t id_width_bytes() const noexcept {
        return static_cast<std::size_t>(id_width_);
    }

    [[nodiscard]] std::uint64_t seed() const noexcept {
        return seed_;
    }

    [[nodiscard]] std::optional<StringId> find(std::string_view value) const noexcept {
#if !defined(STRING_BIMAP_HAS_PTHASH)
        (void)value;
        return std::nullopt;
#else
        if (strings_by_id_.empty()) {
            return std::nullopt;
        }
        const auto candidate = static_cast<StringId>(mphf_(value));
        if (candidate >= strings_by_id_.size()) {
            return std::nullopt;
        }
        if (strings_by_id_[candidate] != value) {
            return std::nullopt;
        }
        return candidate;
#endif
    }

    template <class Id>
    [[nodiscard]] std::optional<Id> find_as(std::string_view value) const noexcept {
        static_assert(std::is_integral_v<Id> && std::is_unsigned_v<Id>, "Id must be an unsigned integer");
        const auto id = find(value);
        if (!id.has_value()) {
            return std::nullopt;
        }
        if (*id > static_cast<StringId>(std::numeric_limits<Id>::max())) {
            return std::nullopt;
        }
        return static_cast<Id>(*id);
    }

    [[nodiscard]] std::optional<PthashCompactId> find_compact(std::string_view value) const noexcept {
        const auto id = find(value);
        if (!id.has_value()) {
            return std::nullopt;
        }
        switch (id_width_) {
            case PthashIdWidth::U8:
                return PthashCompactId{static_cast<std::uint8_t>(*id)};
            case PthashIdWidth::U16:
                return PthashCompactId{static_cast<std::uint16_t>(*id)};
            case PthashIdWidth::U32:
                return PthashCompactId{static_cast<std::uint32_t>(*id)};
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string_view by_id(StringId id) const noexcept {
        return id < strings_by_id_.size() ? strings_by_id_[id] : std::string_view{};
    }

    template <class Id>
    [[nodiscard]] std::string_view by_id(Id id) const noexcept {
        static_assert(std::is_integral_v<Id>, "Id must be an integer");
        return by_id(static_cast<StringId>(id));
    }

    [[nodiscard]] const std::vector<std::string>& strings() const noexcept {
        return strings_by_id_;
    }

    [[nodiscard]] PthashBimapMemoryUsage memory_usage() const noexcept {
        PthashBimapMemoryUsage usage;
        usage.bookkeeping_bytes = sizeof(*this);
        usage.string_vector_bytes = strings_by_id_.capacity() * sizeof(std::string);
        for (const auto& value : strings_by_id_) {
            usage.string_bytes += value.capacity();
        }
#if defined(STRING_BIMAP_HAS_PTHASH)
        usage.mph_bytes = static_cast<std::size_t>((mphf_.num_bits() + 7U) / 8U);
#endif
        return usage;
    }

    void save(std::ostream& out) const {
        out.write(detail::kPthashFileMagic.data(),
                  static_cast<std::streamsize>(detail::kPthashFileMagic.size()));
        if (!out) {
            throw std::runtime_error("failed to write pthash bimap magic");
        }
        detail::write_pod_local(out, detail::kPthashFormatVersion);
        detail::write_pod_local(out, static_cast<std::uint8_t>(id_width_));
        detail::write_pod_local(out, seed_);
        detail::write_pod_local(out, static_cast<std::uint32_t>(strings_by_id_.size()));
        for (const auto& value : strings_by_id_) {
            detail::write_string(out, value);
        }
    }

    void save(const std::string& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            throw std::runtime_error("failed to open pthash bimap output file");
        }
        save(out);
    }

    [[nodiscard]] static PthashBimap load(std::istream& in) {
        std::string magic(detail::kPthashFileMagic.size(), '\0');
        in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (!in || magic != detail::kPthashFileMagic) {
            throw std::runtime_error("invalid pthash bimap file magic");
        }
        const auto version = detail::read_pod_local<std::uint32_t>(in);
        if (version != detail::kPthashFormatVersion) {
            throw std::runtime_error("unsupported pthash bimap format version");
        }
        const auto stored_width = detail::read_pod_local<std::uint8_t>(in);
        const auto stored_seed = detail::read_pod_local<std::uint64_t>(in);
        const auto count = detail::read_pod_local<std::uint32_t>(in);

        std::vector<std::string> values;
        values.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            values.push_back(detail::read_string(in));
        }

        PthashBuildOptions options;
        options.initial_seed = stored_seed;
        options.max_seed_attempts = 1;

        PthashBimap bimap(values, options);
        if (static_cast<std::uint8_t>(bimap.id_width_) != stored_width) {
            throw std::runtime_error("pthash bimap id-width mismatch during load");
        }
        return bimap;
    }

    [[nodiscard]] static PthashBimap load(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("failed to open pthash bimap input file");
        }
        return load(in);
    }

private:
#if defined(STRING_BIMAP_HAS_PTHASH)
    void build_from_values(const std::vector<std::string>& values, const PthashBuildOptions& options) {
        std::vector<std::string> canonical(values.begin(), values.end());
        std::sort(canonical.begin(), canonical.end());
        canonical.erase(std::unique(canonical.begin(), canonical.end()), canonical.end());

        strings_by_id_.clear();
        seed_ = options.initial_seed;
        id_width_ = detail::select_pthash_id_width(canonical.size());

        if (canonical.empty()) {
            mphf_ = detail::PthashFunction{};
            return;
        }

        pthash::build_configuration config;
        config.lambda = options.lambda;
        config.minimal = true;
        config.verbose = false;
        config.num_threads = 1;

        bool built = false;
        std::uint64_t candidate_seed = options.initial_seed;
        for (std::uint64_t attempt = 0; attempt < options.max_seed_attempts; ++attempt, ++candidate_seed) {
            config.seed = candidate_seed;
            detail::PthashFunction candidate;
            try {
                candidate.build_in_internal_memory(canonical.begin(), canonical.size(), config);
                mphf_ = std::move(candidate);
                seed_ = candidate_seed;
                built = true;
                break;
            } catch (const pthash::seed_runtime_error&) {
            }
        }
        if (!built) {
            throw std::runtime_error("failed to build deterministic pthash function");
        }

        strings_by_id_.assign(canonical.size(), std::string{});
        std::vector<bool> assigned(canonical.size(), false);
        for (const auto& value : canonical) {
            const auto id = static_cast<std::size_t>(mphf_(value));
            if (id >= strings_by_id_.size()) {
                throw std::runtime_error("pthash produced out-of-range id");
            }
            if (assigned[id]) {
                throw std::runtime_error("pthash produced duplicate id");
            }
            strings_by_id_[id] = value;
            assigned[id] = true;
        }
    }

    detail::PthashFunction mphf_{};
#endif
    std::vector<std::string> strings_by_id_;
    std::uint64_t seed_ = 0;
    PthashIdWidth id_width_ = PthashIdWidth::U8;
};

}  // namespace string_bimap
