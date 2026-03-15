#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace string_bimap::detail {

inline constexpr std::array<char, 8> kFileMagic = {'F', 'S', 'S', 'T', 'P', 'L', 'U', 'S'};
inline constexpr std::uint32_t kFormatVersion = 2;

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

inline std::string compact_trie_sidecar_path(const std::string& path) {
    return path + ".compact.xcdat";
}

inline std::string compact_marisa_sidecar_path(const std::string& path) {
    return path + ".compact.marisa";
}

inline std::string compact_fst_sidecar_path(const std::string& path) {
    return path + ".compact.fst";
}

inline std::string compact_ids_sidecar_path(const std::string& path) {
    return path + ".compact.ids";
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

}  // namespace string_bimap::detail
