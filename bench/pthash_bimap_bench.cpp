#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <string_bimap/pthash_bimap.hpp>

namespace {

using Clock = std::chrono::steady_clock;

struct SortedVectorLookup {
    std::vector<std::string> values;

    explicit SortedVectorLookup(std::vector<std::string> input) : values(std::move(input)) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }

    std::optional<std::uint32_t> find(std::string_view value) const {
        const auto it = std::lower_bound(values.begin(), values.end(), value,
                                         [](const std::string& left, std::string_view right) {
                                             return std::string_view(left) < right;
                                         });
        if (it == values.end() || std::string_view(*it) != value) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(std::distance(values.begin(), it));
    }

    std::string_view by_id(std::uint32_t id) const {
        return id < values.size() ? std::string_view(values[id]) : std::string_view{};
    }

    std::size_t memory_bytes() const {
        std::size_t bytes = values.capacity() * sizeof(std::string);
        for (const auto& value : values) {
            bytes += value.capacity();
        }
        return bytes;
    }
};

struct UnorderedMapLookup {
    std::vector<std::string> values;
    std::unordered_map<std::string, std::uint32_t> index;

    explicit UnorderedMapLookup(std::vector<std::string> input) {
        std::sort(input.begin(), input.end());
        input.erase(std::unique(input.begin(), input.end()), input.end());
        values = input;
        index.reserve(values.size());
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            index.emplace(values[i], i);
        }
    }

    std::optional<std::uint32_t> find(std::string_view value) const {
        const auto it = index.find(std::string(value));
        if (it == index.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::string_view by_id(std::uint32_t id) const {
        return id < values.size() ? std::string_view(values[id]) : std::string_view{};
    }

    std::size_t memory_bytes() const {
        std::size_t bytes = values.capacity() * sizeof(std::string);
        for (const auto& value : values) {
            bytes += value.capacity();
        }
        bytes += index.bucket_count() * sizeof(void*);
        bytes += index.size() * (sizeof(std::string) + sizeof(std::uint32_t) + 2 * sizeof(void*));
        return bytes;
    }
};

std::vector<std::string> make_miss_queries(const std::vector<std::string>& values) {
    std::vector<std::string> misses;
    misses.reserve(values.size());
    for (const auto& value : values) {
        misses.push_back(value + "\t");
    }
    return misses;
}

template <class Fn>
double measure_ns_per_op(std::size_t operations, Fn&& fn) {
    const auto start = Clock::now();
    fn();
    const auto elapsed = Clock::now() - start;
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return operations == 0 ? 0.0 : static_cast<double>(nanos) / static_cast<double>(operations);
}

std::vector<std::string> load_values(int argc, char** argv, std::string& dataset_label) {
    std::string json_array_file;
    std::string csv_file;
    std::string csv_column;
    std::size_t csv_index = 0;
    bool csv_has_header = true;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--json-array-file" && i + 1 < argc) {
            json_array_file = argv[++i];
        } else if (arg == "--csv-file" && i + 1 < argc) {
            csv_file = argv[++i];
        } else if (arg == "--csv-column" && i + 1 < argc) {
            csv_column = argv[++i];
        } else if (arg == "--csv-index" && i + 1 < argc) {
            csv_index = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--csv-no-header") {
            csv_has_header = false;
        } else {
            throw std::runtime_error("unknown argument: " + std::string(arg));
        }
    }

    if (!json_array_file.empty()) {
        dataset_label = json_array_file;
        return string_bimap::PthashBimap::load_values_from_json_array_file(json_array_file);
    }
    if (!csv_file.empty()) {
        dataset_label = csv_file;
        if (!csv_column.empty()) {
            return string_bimap::PthashBimap::load_values_from_csv_file(csv_file, csv_column, csv_index,
                                                                        csv_has_header);
        }
        return string_bimap::PthashBimap::load_values_from_csv_file(csv_file, std::nullopt, csv_index,
                                                                    csv_has_header);
    }

    throw std::runtime_error(
        "usage: string_bimap_pthash_bench --json-array-file <path> | "
        "--csv-file <path> [--csv-column <name> | --csv-index <n>] [--csv-no-header]");
}

template <class Lookup>
void bench_lookup(const char* name, const Lookup& lookup, const std::vector<std::string>& hits,
                  const std::vector<std::string>& misses) {
    std::uint64_t hit_sink = 0;
    std::uint64_t miss_sink = 0;
    std::uint64_t reverse_sink = 0;

    const auto hit_ns = measure_ns_per_op(hits.size(), [&] {
        for (const auto& value : hits) {
            const auto id = lookup.find(value);
            hit_sink += id.has_value() ? *id : 0;
        }
    });

    const auto miss_ns = measure_ns_per_op(misses.size(), [&] {
        for (const auto& value : misses) {
            const auto id = lookup.find(value);
            miss_sink += id.has_value() ? 1 : 0;
        }
    });

    const auto reverse_ns = measure_ns_per_op(hits.size(), [&] {
        for (std::uint32_t id = 0; id < hits.size(); ++id) {
            reverse_sink += lookup.by_id(id).size();
        }
    });

    std::cout << name << ".hit_lookup_ns_per_op=" << std::fixed << std::setprecision(2) << hit_ns
              << '\n';
    std::cout << name << ".miss_lookup_ns_per_op=" << miss_ns << '\n';
    std::cout << name << ".reverse_lookup_ns_per_op=" << reverse_ns << '\n';
    std::cout << name << ".hit_sink=" << hit_sink << '\n';
    std::cout << name << ".miss_sink=" << miss_sink << '\n';
    std::cout << name << ".reverse_sink=" << reverse_sink << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string dataset_label;
        auto values = load_values(argc, argv, dataset_label);
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        const auto misses = make_miss_queries(values);

        std::cout << "dataset=" << dataset_label << '\n';
        std::cout << "unique_values=" << values.size() << '\n';

        const auto pthash_build_ns = measure_ns_per_op(values.size(), [&] {
            string_bimap::PthashBimap probe(values);
            (void)probe;
        });
        string_bimap::PthashBimap pthash(values);

        const auto sorted_build_ns = measure_ns_per_op(values.size(), [&] {
            SortedVectorLookup probe(values);
            (void)probe;
        });
        SortedVectorLookup sorted(values);

        const auto unordered_build_ns = measure_ns_per_op(values.size(), [&] {
            UnorderedMapLookup probe(values);
            (void)probe;
        });
        UnorderedMapLookup unordered(values);

        std::cout << "pthash.build_ns_per_input=" << std::fixed << std::setprecision(2)
                  << pthash_build_ns << '\n';
        std::cout << "pthash.memory_bytes=" << pthash.memory_usage().total_bytes() << '\n';
        std::cout << "pthash.id_width_bytes=" << pthash.id_width_bytes() << '\n';

        std::cout << "sorted.build_ns_per_input=" << sorted_build_ns << '\n';
        std::cout << "sorted.memory_bytes=" << sorted.memory_bytes() << '\n';

        std::cout << "unordered.build_ns_per_input=" << unordered_build_ns << '\n';
        std::cout << "unordered.memory_bytes=" << unordered.memory_bytes() << '\n';

        bench_lookup("pthash", pthash, values, misses);
        bench_lookup("sorted", sorted, values, misses);
        bench_lookup("unordered", unordered, values, misses);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
