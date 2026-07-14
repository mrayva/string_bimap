#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <sstream>
#include <string>
#include <string_view>

#include "string_bimap/string_bimap.hpp"

namespace {

using string_bimap::StringBimap;

std::string make_snapshot(const std::uint8_t* data, std::size_t size) {
    StringBimap dict;
    std::size_t offset = 0;
    while (offset < size && dict.size() < 64) {
        const auto requested_length = 1U + data[offset] % 32U;
        ++offset;
        const auto length = std::min<std::size_t>(requested_length, size - offset);
        if (length == 0) {
            break;
        }
        static_cast<void>(
            dict.insert(std::string_view(reinterpret_cast<const char*>(data + offset), length)));
        offset += length;
    }

    std::ostringstream out(std::ios::binary);
    dict.save(out);
    return out.str();
}

void verify_loaded(const StringBimap& dict) {
    std::size_t count = 0;
    dict.for_each_live([&](string_bimap::StringId id, std::string_view value) {
        const auto found = dict.find_id(value);
        if (!found || *found != id || dict.get_string(id) != value) {
            std::abort();
        }
        ++count;
    });
    if (count != dict.size()) {
        std::abort();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string payload;
    if (size == 0 || data[0] % 3U == 0) {
        payload.assign(reinterpret_cast<const char*>(data), size);
    } else {
        const auto split = 1U + (size - 1U) / 2U;
        payload = make_snapshot(data + 1U, split - 1U);
        if (data[0] % 3U == 2U) {
            auto* payload_bytes = reinterpret_cast<unsigned char*>(payload.data());
            for (std::size_t i = split; i + 1U < size; i += 2U) {
                const auto position = static_cast<std::size_t>(data[i]) % payload.size();
                payload_bytes[position] ^= data[i + 1U];
            }
        }
    }

    std::istringstream in(payload, std::ios::binary);
    try {
        verify_loaded(StringBimap::load(in));
    } catch (const std::exception&) {
        // Invalid snapshots are expected; crashes and invariant failures are not.
        return 0;
    }
    return 0;
}
