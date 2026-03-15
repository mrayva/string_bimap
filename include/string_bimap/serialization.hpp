#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace string_bimap::detail {

inline constexpr std::array<char, 8> kFileMagic = {'F', 'S', 'S', 'T', 'P', 'L', 'U', 'S'};
inline constexpr std::uint32_t kFormatVersion = 1;

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

}  // namespace string_bimap::detail
