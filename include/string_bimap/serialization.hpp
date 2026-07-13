#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace string_bimap::detail {

inline constexpr std::array<char, 8> kFileMagic = {'S', 'T', 'R', 'B', 'M', 'A', 'P', '1'};
inline constexpr std::uint32_t kFormatVersion = 3;
inline constexpr std::array<char, 8> kNativeStateMagic = {'S', 'B', 'N', 'A', 'T', 'I', 'V', 'E'};
inline constexpr std::uint32_t kNativeStateVersion = 2;

template <class T>
void write_pod(std::ostream& out, const T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!out) {
        throw std::runtime_error("failed to write serialized dictionary data");
    }
}

template <class T>
T read_pod(std::istream& in) {
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!in) {
        throw std::runtime_error("failed to read serialized dictionary data");
    }
    return value;
}

inline void write_bytes(std::ostream& out, const char* data, std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::length_error("serialized dictionary payload exceeds stream limits");
    }
    out.write(data, static_cast<std::streamsize>(size));
    if (!out) {
        throw std::runtime_error("failed to write serialized dictionary payload");
    }
}

inline std::string read_string(std::istream& in, std::size_t size) {
    std::string value(size, '\0');
    if (size != 0) {
        in.read(&value[0], static_cast<std::streamsize>(size));
        if (!in) {
            throw std::runtime_error("failed to read serialized string payload");
        }
    }
    return value;
}

inline std::string read_string_incremental(std::istream& in, std::size_t size) {
    constexpr std::size_t kChunkSize = 64 * 1024;
    std::array<char, kChunkSize> buffer{};
    std::string value;
    value.reserve(size < kChunkSize ? size : kChunkSize);
    while (value.size() < size) {
        const auto remaining = size - value.size();
        const auto chunk_size = remaining < buffer.size() ? remaining : buffer.size();
        in.read(buffer.data(), static_cast<std::streamsize>(chunk_size));
        if (!in) {
            throw std::runtime_error("failed to read serialized string payload");
        }
        value.append(buffer.data(), chunk_size);
    }
    return value;
}

inline std::optional<std::uint64_t> remaining_bytes(std::istream& in) {
    const auto original_state = in.rdstate();
    const auto current = in.tellg();
    if (current == std::istream::pos_type(-1)) {
        in.clear(original_state);
        return std::nullopt;
    }

    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    in.clear();
    in.seekg(current);
    if (!in || end == std::istream::pos_type(-1) || end < current) {
        in.clear(original_state);
        return std::nullopt;
    }
    in.clear(original_state);
    return static_cast<std::uint64_t>(end - current);
}

inline std::string compact_trie_sidecar_path(const std::string& path) {
    return path + ".compact.xcdat";
}

inline std::string compact_marisa_sidecar_path(const std::string& path) {
    return path + ".compact.marisa";
}

inline std::string compact_ids_sidecar_path(const std::string& path) {
    return path + ".compact.ids";
}

inline std::string native_state_sidecar_path(const std::string& path) {
    return path + ".native.state";
}

inline std::string native_base_storage_sidecar_path(const std::string& path) {
    return path + ".native.base";
}

inline std::string native_delta_storage_sidecar_path(const std::string& path) {
    return path + ".native.delta";
}

template <class T>
void write_vector_file(const std::string& path, const std::vector<T>& values) {
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to open vector sidecar for writing: " + path);
    }
    const std::uint64_t count = static_cast<std::uint64_t>(values.size());
    write_pod(out, count);
    if (!values.empty()) {
        out.write(reinterpret_cast<const char*>(values.data()),
                  static_cast<std::streamsize>(values.size() * sizeof(T)));
        if (!out) {
            throw std::runtime_error("failed to write vector sidecar payload: " + path);
        }
    }
}

template <class T>
std::vector<T> read_vector_file(const std::string& path) {
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open vector sidecar for reading: " + path);
    }
    const auto count = read_pod<std::uint64_t>(in);
    if (count > std::vector<T>{}.max_size() ||
        count > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) / sizeof(T)) {
        throw std::runtime_error("vector sidecar count exceeds platform limits: " + path);
    }
    std::vector<T> values(static_cast<std::size_t>(count));
    if (!values.empty()) {
        in.read(reinterpret_cast<char*>(values.data()),
                static_cast<std::streamsize>(values.size() * sizeof(T)));
        if (!in) {
            throw std::runtime_error("failed to read vector sidecar payload: " + path);
        }
    }
    return values;
}

inline void write_blob_file(const std::string& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to open blob sidecar for writing: " + path);
    }
    const std::uint64_t size = static_cast<std::uint64_t>(bytes.size());
    write_pod(out, size);
    if (!bytes.empty()) {
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            throw std::runtime_error("failed to write blob sidecar payload: " + path);
        }
    }
}

inline std::vector<char> read_blob_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open blob sidecar for reading: " + path);
    }
    const auto size = read_pod<std::uint64_t>(in);
    if (size > std::vector<char>{}.max_size() ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("blob sidecar size exceeds platform limits: " + path);
    }
    std::vector<char> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!in) {
            throw std::runtime_error("failed to read blob sidecar payload: " + path);
        }
    }
    return bytes;
}

}  // namespace string_bimap::detail
