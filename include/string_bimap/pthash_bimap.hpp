#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
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

#include <essentials.hpp>
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

struct PthashIdInfo {
    PthashIdWidth width = PthashIdWidth::U8;
    std::size_t width_bytes = 1;
    std::uint32_t max_dense_id = 0;
    std::size_t cardinality = 0;
};

namespace detail {

inline constexpr std::string_view kPthashFileMagic = "STRBMPH1";
inline constexpr std::uint32_t kPthashFormatVersion = 1;

inline std::string native_pthash_sidecar_path(const std::string& path) {
    return path + ".native.pthash";
}

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

inline std::string trim_ascii(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && static_cast<unsigned char>(value[begin]) <= 0x20) {
        ++begin;
    }
    while (end > begin && static_cast<unsigned char>(value[end - 1]) <= 0x20) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

inline void skip_json_ws(std::istream& in) {
    while (true) {
        const int ch = in.peek();
        if (ch == EOF) {
            return;
        }
        if (static_cast<unsigned char>(ch) > 0x20) {
            return;
        }
        in.get();
    }
}

inline std::string parse_json_string(std::istream& in) {
    if (in.get() != '"') {
        throw std::runtime_error("expected JSON string");
    }
    std::string out;
    while (true) {
        const int ch = in.get();
        if (ch == EOF) {
            throw std::runtime_error("unterminated JSON string");
        }
        if (ch == '"') {
            return out;
        }
        if (ch != '\\') {
            out.push_back(static_cast<char>(ch));
            continue;
        }
        const int esc = in.get();
        if (esc == EOF) {
            throw std::runtime_error("unterminated JSON escape");
        }
        switch (esc) {
            case '"':
            case '\\':
            case '/':
                out.push_back(static_cast<char>(esc));
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                char hex[4];
                if (!in.read(hex, 4)) {
                    throw std::runtime_error("invalid JSON unicode escape");
                }
                unsigned codepoint = 0;
                for (char digit : hex) {
                    codepoint <<= 4;
                    if (digit >= '0' && digit <= '9') {
                        codepoint |= static_cast<unsigned>(digit - '0');
                    } else if (digit >= 'a' && digit <= 'f') {
                        codepoint |= static_cast<unsigned>(digit - 'a' + 10);
                    } else if (digit >= 'A' && digit <= 'F') {
                        codepoint |= static_cast<unsigned>(digit - 'A' + 10);
                    } else {
                        throw std::runtime_error("invalid JSON unicode escape");
                    }
                }
                if (codepoint <= 0x7F) {
                    out.push_back(static_cast<char>(codepoint));
                } else if (codepoint <= 0x7FF) {
                    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
                break;
            }
            default:
                throw std::runtime_error("unsupported JSON escape");
        }
    }
}

inline std::vector<std::string> parse_json_string_array_body(std::istream& in) {
    std::vector<std::string> values;
    skip_json_ws(in);
    if (in.peek() == ']') {
        in.get();
        return values;
    }
    while (true) {
        skip_json_ws(in);
        values.push_back(parse_json_string(in));
        skip_json_ws(in);
        const int ch = in.get();
        if (ch == ']') {
            break;
        }
        if (ch != ',') {
            throw std::runtime_error("expected ',' or ']' in JSON array");
        }
    }
    return values;
}

inline std::vector<std::string> parse_json_string_array(std::istream& in) {
    skip_json_ws(in);
    const int first = in.peek();
    if (first == '[') {
        in.get();
        auto values = parse_json_string_array_body(in);
        skip_json_ws(in);
        if (in.peek() != EOF) {
            throw std::runtime_error("unexpected trailing bytes after JSON array");
        }
        return values;
    }

    if (first != '{') {
        throw std::runtime_error("expected JSON array or object");
    }
    in.get();
    while (true) {
        skip_json_ws(in);
        if (in.peek() == '}') {
            break;
        }
        const auto key = parse_json_string(in);
        skip_json_ws(in);
        if (in.get() != ':') {
            throw std::runtime_error("expected ':' in JSON object");
        }
        skip_json_ws(in);
        if (key == "values") {
            if (in.get() != '[') {
                throw std::runtime_error("expected '[' after values key");
            }
            auto values = parse_json_string_array_body(in);
            skip_json_ws(in);
            if (in.peek() == ',') {
                in.get();
                skip_json_ws(in);
                if (in.peek() != '}') {
                    throw std::runtime_error("unexpected extra fields after values array");
                }
            }
            if (in.get() != '}') {
                throw std::runtime_error("expected closing '}' after values array");
            }
            skip_json_ws(in);
            if (in.peek() != EOF) {
                throw std::runtime_error("unexpected trailing bytes after JSON object");
            }
            return values;
        }
        throw std::runtime_error("unsupported JSON object: expected only 'values'");
    }
    throw std::runtime_error("JSON object did not contain 'values'");
}

inline std::vector<std::string> parse_csv_row(std::string_view line) {
    std::vector<std::string> fields;
    std::string current;
    bool in_quotes = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (in_quotes) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    current.push_back('"');
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                current.push_back(ch);
            }
            continue;
        }
        if (ch == ',') {
            fields.push_back(std::move(current));
            current.clear();
        } else if (ch == '"') {
            in_quotes = true;
        } else {
            current.push_back(ch);
        }
    }
    if (in_quotes) {
        throw std::runtime_error("unterminated quoted CSV field");
    }
    fields.push_back(std::move(current));
    return fields;
}

inline std::vector<std::string> parse_csv_column(std::istream& in,
                                                 std::optional<std::string_view> column_name,
                                                 std::size_t column_index,
                                                 bool has_header) {
    std::vector<std::string> values;
    std::string line;
    bool first_line = true;
    std::size_t resolved_index = column_index;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        auto row = parse_csv_row(line);
        if (first_line && has_header) {
            if (column_name.has_value()) {
                bool found = false;
                for (std::size_t i = 0; i < row.size(); ++i) {
                    if (trim_ascii(row[i]) == *column_name) {
                        resolved_index = i;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    throw std::runtime_error("CSV column name not found");
                }
            } else if (resolved_index >= row.size()) {
                throw std::runtime_error("CSV column index out of range");
            }
            first_line = false;
            continue;
        }
        first_line = false;
        if (resolved_index >= row.size()) {
            throw std::runtime_error("CSV row is missing requested column");
        }
        const auto value = trim_ascii(row[resolved_index]);
        if (!value.empty()) {
            values.push_back(value);
        }
    }
    return values;
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

    [[nodiscard]] static std::vector<std::string> load_values_from_json_array(std::istream& in) {
        return detail::parse_json_string_array(in);
    }

    [[nodiscard]] static std::vector<std::string> load_values_from_json_array_file(const std::string& path) {
        std::ifstream in(path);
        if (!in) {
            throw std::runtime_error("failed to open JSON array file");
        }
        return load_values_from_json_array(in);
    }

    [[nodiscard]] static std::vector<std::string> load_values_from_csv(
        std::istream& in, std::optional<std::string_view> column_name = std::nullopt,
        std::size_t column_index = 0, bool has_header = true) {
        return detail::parse_csv_column(in, column_name, column_index, has_header);
    }

    [[nodiscard]] static std::vector<std::string> load_values_from_csv_file(
        const std::string& path, std::optional<std::string_view> column_name = std::nullopt,
        std::size_t column_index = 0, bool has_header = true) {
        std::ifstream in(path);
        if (!in) {
            throw std::runtime_error("failed to open CSV file");
        }
        return load_values_from_csv(in, column_name, column_index, has_header);
    }

    static PthashBimap from_json_array(std::istream& in, const PthashBuildOptions& options = {}) {
        return PthashBimap(load_values_from_json_array(in), options);
    }

    static PthashBimap from_json_array_file(const std::string& path,
                                            const PthashBuildOptions& options = {}) {
        return PthashBimap(load_values_from_json_array_file(path), options);
    }

    static PthashBimap from_csv(std::istream& in, std::optional<std::string_view> column_name = std::nullopt,
                                std::size_t column_index = 0, bool has_header = true,
                                const PthashBuildOptions& options = {}) {
        return PthashBimap(load_values_from_csv(in, column_name, column_index, has_header), options);
    }

    static PthashBimap from_csv_file(const std::string& path,
                                     std::optional<std::string_view> column_name = std::nullopt,
                                     std::size_t column_index = 0, bool has_header = true,
                                     const PthashBuildOptions& options = {}) {
        return PthashBimap(load_values_from_csv_file(path, column_name, column_index, has_header),
                           options);
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

    [[nodiscard]] PthashIdInfo id_info() const noexcept {
        return PthashIdInfo{
            id_width_,
            id_width_bytes(),
            max_id(),
            size(),
        };
    }

    [[nodiscard]] bool fits_in_uint8() const noexcept {
        return id_width_ == PthashIdWidth::U8;
    }

    [[nodiscard]] bool fits_in_uint16() const noexcept {
        return id_width_ == PthashIdWidth::U8 || id_width_ == PthashIdWidth::U16;
    }

    [[nodiscard]] bool fits_in_uint32() const noexcept {
        return true;
    }

    template <class Id>
    [[nodiscard]] bool can_represent_ids_as() const noexcept {
        static_assert(std::is_integral_v<Id> && std::is_unsigned_v<Id>, "Id must be an unsigned integer");
        return size() <= static_cast<std::size_t>(std::numeric_limits<Id>::max()) + 1ULL;
    }

    [[nodiscard]] std::uint64_t seed() const noexcept {
        return seed_;
    }

    [[nodiscard]] std::uint32_t max_id() const noexcept {
        return empty() ? 0U : static_cast<std::uint32_t>(size() - 1U);
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

    [[nodiscard]] bool contains(std::string_view value) const noexcept {
        return find(value).has_value();
    }

    [[nodiscard]] bool contains_id(StringId id) const noexcept {
        return id < size();
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
        return compact_id_for(*id);
    }

    [[nodiscard]] std::optional<PthashCompactId> compact_id_for(StringId id) const noexcept {
        if (id >= size()) {
            return std::nullopt;
        }
        switch (id_width_) {
            case PthashIdWidth::U8:
                return PthashCompactId{static_cast<std::uint8_t>(id)};
            case PthashIdWidth::U16:
                return PthashCompactId{static_cast<std::uint16_t>(id)};
            case PthashIdWidth::U32:
                return PthashCompactId{static_cast<std::uint32_t>(id)};
        }
        return std::nullopt;
    }

    [[nodiscard]] static StringId widen_compact_id(const PthashCompactId& id) noexcept {
        return std::visit([](auto value) { return static_cast<StringId>(value); }, id);
    }

    [[nodiscard]] std::string_view by_id(StringId id) const noexcept {
        return id < strings_by_id_.size() ? strings_by_id_[id] : std::string_view{};
    }

    [[nodiscard]] std::optional<std::string_view> try_by_id(StringId id) const noexcept {
        if (!contains_id(id)) {
            return std::nullopt;
        }
        return strings_by_id_[id];
    }

    [[nodiscard]] std::string_view at(StringId id) const {
        const auto value = try_by_id(id);
        if (!value.has_value()) {
            throw std::out_of_range("PthashBimap id out of range");
        }
        return *value;
    }

    [[nodiscard]] std::string_view by_compact_id(const PthashCompactId& id) const noexcept {
        return by_id(widen_compact_id(id));
    }

    [[nodiscard]] std::optional<std::string_view> try_by_compact_id(
        const PthashCompactId& id) const noexcept {
        return try_by_id(widen_compact_id(id));
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
#if defined(STRING_BIMAP_HAS_PTHASH)
        save_native_sidecar(path);
#endif
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

        PthashBimap bimap;
        bimap.strings_by_id_ = std::move(values);
        bimap.id_width_ = static_cast<PthashIdWidth>(stored_width);
        bimap.seed_ = stored_seed;

#if defined(STRING_BIMAP_HAS_PTHASH)
        if (!bimap.try_load_native_sidecar(path)) {
            PthashBuildOptions options;
            options.initial_seed = stored_seed;
            options.max_seed_attempts = 1;
            bimap.build_from_id_order_strings(options);
        }
#else
        (void)path;
        throw std::logic_error("PthashBimap requires STRING_BIMAP_HAS_PTHASH");
#endif

        if (static_cast<std::uint8_t>(bimap.id_width_) != stored_width) {
            throw std::runtime_error("pthash bimap id-width mismatch during load");
        }
        return bimap;
    }

private:
#if defined(STRING_BIMAP_HAS_PTHASH)
    void save_native_sidecar(const std::string& path) const {
        if (strings_by_id_.empty()) {
            std::error_code ec;
            std::filesystem::remove(detail::native_pthash_sidecar_path(path), ec);
            return;
        }
        essentials::save(mphf_, detail::native_pthash_sidecar_path(path).c_str());
    }

    [[nodiscard]] bool try_load_native_sidecar(const std::string& path) {
        const auto sidecar = detail::native_pthash_sidecar_path(path);
        if (!std::filesystem::exists(sidecar)) {
            return false;
        }
        detail::PthashFunction loaded;
        essentials::load(loaded, sidecar.c_str());
        if (loaded.num_keys() != strings_by_id_.size()) {
            return false;
        }
        mphf_ = std::move(loaded);
        return validate_loaded_native_index();
    }

    [[nodiscard]] bool validate_loaded_native_index() const {
        if (strings_by_id_.empty()) {
            return true;
        }
        for (std::size_t id = 0; id < strings_by_id_.size(); ++id) {
            const auto candidate = static_cast<std::size_t>(mphf_(strings_by_id_[id]));
            if (candidate != id) {
                return false;
            }
        }
        return true;
    }

    void build_from_id_order_strings(const PthashBuildOptions& options) {
        if (strings_by_id_.empty()) {
            mphf_ = detail::PthashFunction{};
            id_width_ = PthashIdWidth::U8;
            seed_ = options.initial_seed;
            return;
        }
        std::vector<std::string> canonical = strings_by_id_;
        std::sort(canonical.begin(), canonical.end());
        canonical.erase(std::unique(canonical.begin(), canonical.end()), canonical.end());
        if (canonical.size() != strings_by_id_.size()) {
            throw std::runtime_error("stored pthash bimap strings are not unique");
        }
        build_native_index(canonical, options);

        std::vector<std::string> reordered(strings_by_id_.size());
        for (const auto& value : canonical) {
            const auto id = static_cast<std::size_t>(mphf_(value));
            if (id >= reordered.size()) {
                throw std::runtime_error("pthash produced out-of-range id");
            }
            reordered[id] = value;
        }
        if (reordered != strings_by_id_) {
            throw std::runtime_error("stored pthash bimap strings do not match deterministic ids");
        }
    }

    void build_native_index(const std::vector<std::string>& canonical, const PthashBuildOptions& options) {
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
    }

    void build_from_values(const std::vector<std::string>& values, const PthashBuildOptions& options) {
        std::vector<std::string> canonical(values.begin(), values.end());
        std::sort(canonical.begin(), canonical.end());
        canonical.erase(std::unique(canonical.begin(), canonical.end()), canonical.end());

        strings_by_id_.clear();
        build_native_index(canonical, options);

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
