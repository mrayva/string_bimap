#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <string_bimap/string_bimap.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using string_bimap::BackendProfile;
using string_bimap::StringBimap;
using string_bimap::StringId;

struct Config {
    std::size_t num_keys = 50000;
    std::size_t prefix_groups = 256;
    std::uint32_t seed = 12345;
    std::string csv_path;
    std::string column;
    std::string line_file_path;
    std::string line_file_write_path;
    std::string line_file_read_path;
    std::string prefix;
    std::string serialized_file_path;
    std::set<std::string> phases;
    bool shuffle = false;
    bool save_compacted = false;
    bool release_inputs_before_compact = false;
    bool release_serialized_before_load = false;
    std::size_t read_repeats = 1;
    std::size_t read_limit = 0;
    std::size_t loaded_find_ratio = 1;
    std::size_t loaded_get_ratio = 1;
    BackendProfile profile = BackendProfile::FastLookup;
};

[[nodiscard]] std::set<std::string> default_phases() {
    return {"insert", "find", "get", "prefix", "erase", "compact", "prefix_compact", "save", "load"};
}

[[nodiscard]] bool has_phase(const Config& cfg, std::string_view phase) {
    return cfg.phases.find(std::string(phase)) != cfg.phases.end();
}

[[nodiscard]] std::string_view profile_name(BackendProfile profile) {
    switch (profile) {
        case BackendProfile::FastLookup:
            return "fast";
        case BackendProfile::CompactMemory:
            return "compact";
        case BackendProfile::CompactMemoryMarisa:
            return "marisa";
        case BackendProfile::CompactMemoryMarisaFsst:
            return "marisa_fsst";
        case BackendProfile::CompactMemoryKeyvi:
            return "keyvi";
        case BackendProfile::CompactMemoryFst:
            return "fst";
        case BackendProfile::FastLookupArrayMap:
            return "array_map";
        case BackendProfile::CompactMemoryMarisaArrayMap:
            return "marisa_array_map";
    }
    return "unknown";
}

[[nodiscard]] Config parse_args(int argc, char** argv) {
    Config cfg;
    cfg.phases = default_phases();
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--csv" && i + 1 < argc) {
            cfg.csv_path = argv[++i];
        } else if (arg == "--line-file" && i + 1 < argc) {
            cfg.line_file_path = argv[++i];
        } else if (arg == "--line-file-write" && i + 1 < argc) {
            cfg.line_file_write_path = argv[++i];
        } else if (arg == "--line-file-read" && i + 1 < argc) {
            cfg.line_file_read_path = argv[++i];
        } else if (arg == "--prefix" && i + 1 < argc) {
            cfg.prefix = argv[++i];
        } else if (arg == "--serialized-file" && i + 1 < argc) {
            cfg.serialized_file_path = argv[++i];
        } else if (arg == "--phases" && i + 1 < argc) {
            cfg.phases.clear();
            std::string spec = argv[++i];
            std::size_t start = 0;
            while (start <= spec.size()) {
                const auto end = spec.find(',', start);
                const auto token = spec.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!token.empty()) {
                    cfg.phases.insert(token);
                }
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
        } else if (arg == "--release-inputs-before-compact") {
            cfg.release_inputs_before_compact = true;
        } else if (arg == "--save-compacted") {
            cfg.save_compacted = true;
        } else if (arg == "--release-serialized-before-load") {
            cfg.release_serialized_before_load = true;
        } else if (arg == "--read-repeats" && i + 1 < argc) {
            cfg.read_repeats = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--read-limit" && i + 1 < argc) {
            cfg.read_limit = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--loaded-find-ratio" && i + 1 < argc) {
            cfg.loaded_find_ratio = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--loaded-get-ratio" && i + 1 < argc) {
            cfg.loaded_get_ratio = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--column" && i + 1 < argc) {
            cfg.column = argv[++i];
        } else if (arg == "--keys" && i + 1 < argc) {
            cfg.num_keys = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--prefix-groups" && i + 1 < argc) {
            cfg.prefix_groups = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--seed" && i + 1 < argc) {
            cfg.seed = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--shuffle") {
            cfg.shuffle = true;
        } else if (arg == "--profile" && i + 1 < argc) {
            const std::string_view value(argv[++i]);
            if (value == "fast") {
                cfg.profile = BackendProfile::FastLookup;
            } else if (value == "compact") {
                cfg.profile = BackendProfile::CompactMemory;
            } else if (value == "marisa") {
                cfg.profile = BackendProfile::CompactMemoryMarisa;
            } else if (value == "marisa_fsst") {
                cfg.profile = BackendProfile::CompactMemoryMarisaFsst;
            } else if (value == "keyvi") {
                cfg.profile = BackendProfile::CompactMemoryKeyvi;
            } else if (value == "fst") {
                cfg.profile = BackendProfile::CompactMemoryFst;
            } else if (value == "array_map") {
                cfg.profile = BackendProfile::FastLookupArrayMap;
            } else if (value == "marisa_array_map") {
                cfg.profile = BackendProfile::CompactMemoryMarisaArrayMap;
            } else {
                throw std::invalid_argument("invalid profile: " + std::string(value));
            }
        } else if (arg.rfind("--", 0) == 0) {
            throw std::invalid_argument("unknown argument: " + std::string(arg));
        } else if (cfg.csv_path.empty() && cfg.column.empty() && i + 2 < argc) {
            cfg.num_keys = static_cast<std::size_t>(std::stoull(argv[i]));
            cfg.prefix_groups = static_cast<std::size_t>(std::stoull(argv[i + 1]));
            cfg.seed = static_cast<std::uint32_t>(std::stoul(argv[i + 2]));
            break;
        }
    }
    return cfg;
}

[[nodiscard]] std::string random_suffix(std::mt19937& rng, std::size_t len = 10) {
    static constexpr char alphabet[] =
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789"
        "_-";
    std::uniform_int_distribution<std::size_t> dist(0, sizeof(alphabet) - 2);
    std::string out;
    out.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(alphabet[dist(rng)]);
    }
    return out;
}

[[nodiscard]] std::vector<std::string> make_dataset(const Config& cfg) {
    std::mt19937 rng(cfg.seed);
    std::vector<std::string> keys;
    keys.reserve(cfg.num_keys);

    for (std::size_t i = 0; i < cfg.num_keys; ++i) {
        const auto group = i % std::max<std::size_t>(cfg.prefix_groups, 1);
        std::ostringstream key;
        key << "prefix/" << std::setw(4) << std::setfill('0') << group << "/";
        key << random_suffix(rng, 12) << "/";
        key << i;
        keys.push_back(key.str());
    }

    return keys;
}

[[nodiscard]] std::vector<std::string> parse_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool in_quotes = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                current.push_back('"');
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (ch == ',' && !in_quotes) {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }

    fields.push_back(current);
    return fields;
}

[[nodiscard]] std::size_t resolve_column_index(const std::vector<std::string>& header, const std::string& column) {
    if (column.empty()) {
        return 0;
    }

    bool all_digits = !column.empty();
    for (char ch : column) {
        if (ch < '0' || ch > '9') {
            all_digits = false;
            break;
        }
    }

    if (all_digits) {
        const auto idx = static_cast<std::size_t>(std::stoull(column));
        if (idx >= header.size()) {
            throw std::out_of_range("CSV column index out of range");
        }
        return idx;
    }

    for (std::size_t i = 0; i < header.size(); ++i) {
        if (header[i] == column) {
            return i;
        }
    }
    throw std::invalid_argument("CSV column not found: " + column);
}

[[nodiscard]] std::vector<std::string> load_csv_column(const Config& cfg) {
    std::ifstream in(cfg.csv_path);
    if (!in) {
        throw std::runtime_error("failed to open CSV file: " + cfg.csv_path);
    }

    std::string line;
    if (!std::getline(in, line)) {
        throw std::runtime_error("CSV file is empty: " + cfg.csv_path);
    }

    const auto header = parse_csv_line(line);
    const auto column_index = resolve_column_index(header, cfg.column);

    std::vector<std::string> keys;
    std::unordered_set<std::string> seen;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        auto fields = parse_csv_line(line);
        if (column_index >= fields.size()) {
            continue;
        }
        const auto& value = fields[column_index];
        if (seen.insert(value).second) {
            keys.push_back(value);
        }
    }
    return keys;
}

[[nodiscard]] std::vector<std::string> load_line_file(const Config& cfg) {
    std::ifstream in(cfg.line_file_path);
    if (!in) {
        throw std::runtime_error("failed to open line file: " + cfg.line_file_path);
    }

    std::vector<std::string> keys;
    std::unordered_set<std::string> seen;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        if (seen.insert(line).second) {
            keys.push_back(line);
        }
    }
    return keys;
}

[[nodiscard]] std::vector<std::string> load_line_file_path(const std::string& path,
                                                           bool deduplicate,
                                                           std::size_t limit = 0) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open line file: " + path);
    }

    std::vector<std::string> keys;
    std::unordered_set<std::string> seen;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        if (deduplicate) {
            if (seen.insert(line).second) {
                keys.push_back(line);
            }
        } else {
            keys.push_back(line);
        }

        if (limit != 0 && keys.size() >= limit) {
            break;
        }
    }
    return keys;
}

template <class Fn>
[[nodiscard]] double run_ms(Fn&& fn) {
    const auto start = Clock::now();
    fn();
    const auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void print_metric(std::string_view name, double total_ms, std::size_t ops) {
    const double ns_per_op = ops == 0 ? 0.0 : (total_ms * 1'000'000.0) / static_cast<double>(ops);
    std::cout << std::left << std::setw(20) << name
              << " total_ms=" << std::setw(10) << std::fixed << std::setprecision(3) << total_ms
              << " ns_per_op=" << std::setw(12) << std::fixed << std::setprecision(2) << ns_per_op
              << " ops=" << ops << '\n';
}

[[nodiscard]] std::size_t current_rss_bytes() {
#if defined(__linux__)
    long rss_pages = 0;
    FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return 0;
    }
    long ignored = 0;
    const int scanned = std::fscanf(file, "%ld %ld", &ignored, &rss_pages);
    std::fclose(file);
    if (scanned != 2) {
        return 0;
    }
    return static_cast<std::size_t>(rss_pages) * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}

[[nodiscard]] std::size_t peak_rss_bytes() {
#if defined(__linux__)
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024u;
#else
    return 0;
#endif
}

void print_memory(std::string_view label, std::size_t bytes) {
    std::cout << label << "_bytes=" << bytes << '\n';
}

void print_memory_delta(std::string_view label, std::size_t before, std::size_t after) {
    const std::size_t delta = after >= before ? (after - before) : 0;
    std::cout << label << "_delta_bytes=" << delta << '\n';
}

template <class T>
void release_vector(std::vector<T>& values) {
    std::vector<T>().swap(values);
}

[[nodiscard]] std::uintmax_t file_size_or_zero(const std::string& path) {
    if (path.empty()) {
        return 0;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
}

void print_usage(std::string_view prefix, const string_bimap::StringBimapMemoryUsage& usage) {
    print_memory(std::string(prefix) + "_base_live_strings", usage.base.live_string_bytes);
    print_memory(std::string(prefix) + "_base_arena", usage.base.arena_bytes);
    print_memory(std::string(prefix) + "_base_arena_slack", usage.base.arena_slack_bytes);
    print_memory(std::string(prefix) + "_base_entries", usage.base.entry_table_bytes);
    print_memory(std::string(prefix) + "_base_id_holes", usage.base.id_hole_bytes);
    print_memory(std::string(prefix) + "_base_fallback_index", usage.base.fallback_index_bytes);
    print_memory(std::string(prefix) + "_base_fallback_buckets", usage.base.fallback_index_bucket_bytes);
    print_memory(std::string(prefix) + "_base_fallback_keys", usage.base.fallback_index_key_bytes);
    print_memory(std::string(prefix) + "_base_fallback_nodes", usage.base.fallback_index_node_bytes);
    print_memory(std::string(prefix) + "_base_compact_index", usage.base.compact_index_bytes);
    print_memory(std::string(prefix) + "_base_aux", usage.base.auxiliary_bytes);
    print_memory(std::string(prefix) + "_delta_live_strings", usage.delta.live_string_bytes);
    print_memory(std::string(prefix) + "_delta_arena", usage.delta.arena_bytes);
    print_memory(std::string(prefix) + "_delta_arena_slack", usage.delta.arena_slack_bytes);
    print_memory(std::string(prefix) + "_delta_entries", usage.delta.entry_table_bytes);
    print_memory(std::string(prefix) + "_delta_id_holes", usage.delta.id_hole_bytes);
    print_memory(std::string(prefix) + "_delta_fallback_index", usage.delta.fallback_index_bytes);
    print_memory(std::string(prefix) + "_delta_fallback_buckets", usage.delta.fallback_index_bucket_bytes);
    print_memory(std::string(prefix) + "_delta_fallback_keys", usage.delta.fallback_index_key_bytes);
    print_memory(std::string(prefix) + "_delta_fallback_nodes", usage.delta.fallback_index_node_bytes);
    print_memory(std::string(prefix) + "_delta_compact_index", usage.delta.compact_index_bytes);
    print_memory(std::string(prefix) + "_delta_aux", usage.delta.auxiliary_bytes);
    print_memory(std::string(prefix) + "_tombstones", usage.tombstone_bytes);
    print_memory(std::string(prefix) + "_bookkeeping", usage.bookkeeping_bytes);
    print_memory(std::string(prefix) + "_total", usage.total_bytes());
}

}  // namespace

int main(int argc, char** argv) {
    const Config cfg = parse_args(argc, argv);
    const bool input_required = has_phase(cfg, "insert") || has_phase(cfg, "find") || has_phase(cfg, "get") ||
                                has_phase(cfg, "prefix") || has_phase(cfg, "erase") || has_phase(cfg, "compact") ||
                                has_phase(cfg, "prefix_compact") || has_phase(cfg, "save");

    std::vector<std::string> keys;
    std::vector<std::string> read_keys;
    if (input_required) {
        if (!cfg.line_file_write_path.empty()) {
            keys = load_line_file_path(cfg.line_file_write_path, true);
            if (!cfg.line_file_read_path.empty()) {
                read_keys = load_line_file_path(cfg.line_file_read_path, false, cfg.read_limit);
            }
        } else {
            keys = cfg.line_file_path.empty()
                       ? (cfg.csv_path.empty() ? make_dataset(cfg) : load_csv_column(cfg))
                       : load_line_file(cfg);
        }
    }
    if (cfg.shuffle && !keys.empty()) {
        std::mt19937 input_shuffle_rng(cfg.seed ^ 0x5A17A11u);
        std::shuffle(keys.begin(), keys.end(), input_shuffle_rng);
    }
    if (read_keys.empty()) {
        read_keys = keys;
    }
    std::vector<std::size_t> order(read_keys.size());
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 shuffle_rng(cfg.seed ^ 0xA5A5A5A5u);
    std::shuffle(order.begin(), order.end(), shuffle_rng);

    std::cout << "string_bimap benchmark\n";
    if (!input_required) {
        std::cout << "mode=serialized_load"
                  << " profile=" << profile_name(cfg.profile)
                  << " serialized_file=" << (cfg.serialized_file_path.empty() ? "<memory>" : cfg.serialized_file_path)
                  << " read_repeats=" << cfg.read_repeats
                  << " loaded_find_ratio=" << cfg.loaded_find_ratio
                  << " loaded_get_ratio=" << cfg.loaded_get_ratio
                  << " phases=";
        bool first_phase = true;
        for (const auto& phase : cfg.phases) {
            std::cout << (first_phase ? "" : ",") << phase;
            first_phase = false;
        }
        std::cout << '\n';
    } else if (!cfg.line_file_write_path.empty()) {
        std::cout << "mode=line_file_split"
                  << " write_path=" << cfg.line_file_write_path
                  << " read_path=" << (cfg.line_file_read_path.empty() ? "<same-as-write>" : cfg.line_file_read_path)
                  << " write_keys=" << keys.size()
                  << " read_keys=" << read_keys.size()
                  << " profile=" << profile_name(cfg.profile)
                  << " prefix=" << (cfg.prefix.empty() ? "<auto>" : cfg.prefix)
                  << " serialized_file=" << (cfg.serialized_file_path.empty() ? "<memory>" : cfg.serialized_file_path)
                  << " save_compacted=" << (cfg.save_compacted ? "true" : "false")
                  << " read_repeats=" << cfg.read_repeats
                  << " loaded_find_ratio=" << cfg.loaded_find_ratio
                  << " loaded_get_ratio=" << cfg.loaded_get_ratio
                  << " phases=";
        bool first_phase = true;
        for (const auto& phase : cfg.phases) {
            std::cout << (first_phase ? "" : ",") << phase;
            first_phase = false;
        }
        std::cout << " shuffle=" << (cfg.shuffle ? "true" : "false") << '\n';
    } else if (!cfg.line_file_path.empty()) {
        std::cout << "mode=line_file"
                  << " path=" << cfg.line_file_path
                  << " profile=" << profile_name(cfg.profile)
                  << " prefix=" << (cfg.prefix.empty() ? "<auto>" : cfg.prefix)
                  << " serialized_file=" << (cfg.serialized_file_path.empty() ? "<memory>" : cfg.serialized_file_path)
                  << " save_compacted=" << (cfg.save_compacted ? "true" : "false")
                  << " read_repeats=" << cfg.read_repeats
                  << " loaded_find_ratio=" << cfg.loaded_find_ratio
                  << " loaded_get_ratio=" << cfg.loaded_get_ratio
                  << " phases=";
        bool first_phase = true;
        for (const auto& phase : cfg.phases) {
            std::cout << (first_phase ? "" : ",") << phase;
            first_phase = false;
        }
        std::cout
                  << " shuffle=" << (cfg.shuffle ? "true" : "false")
                  << " unique_keys=" << keys.size() << '\n';
    } else if (cfg.csv_path.empty()) {
        std::cout << "mode=synthetic"
                  << " keys=" << cfg.num_keys
                  << " prefix_groups=" << cfg.prefix_groups
                  << " profile=" << profile_name(cfg.profile)
                  << " prefix=" << (cfg.prefix.empty() ? "<auto>" : cfg.prefix)
                  << " serialized_file=" << (cfg.serialized_file_path.empty() ? "<memory>" : cfg.serialized_file_path)
                  << " save_compacted=" << (cfg.save_compacted ? "true" : "false")
                  << " read_repeats=" << cfg.read_repeats
                  << " loaded_find_ratio=" << cfg.loaded_find_ratio
                  << " loaded_get_ratio=" << cfg.loaded_get_ratio
                  << " phases=";
        bool first_phase = true;
        for (const auto& phase : cfg.phases) {
            std::cout << (first_phase ? "" : ",") << phase;
            first_phase = false;
        }
        std::cout
                  << " seed=" << cfg.seed << '\n';
    } else {
        std::cout << "mode=csv"
                  << " path=" << cfg.csv_path
                  << " column=" << (cfg.column.empty() ? "0" : cfg.column)
                  << " profile=" << profile_name(cfg.profile)
                  << " prefix=" << (cfg.prefix.empty() ? "<auto>" : cfg.prefix)
                  << " serialized_file=" << (cfg.serialized_file_path.empty() ? "<memory>" : cfg.serialized_file_path)
                  << " save_compacted=" << (cfg.save_compacted ? "true" : "false")
                  << " read_repeats=" << cfg.read_repeats
                  << " loaded_find_ratio=" << cfg.loaded_find_ratio
                  << " loaded_get_ratio=" << cfg.loaded_get_ratio
                  << " phases=";
        bool first_phase = true;
        for (const auto& phase : cfg.phases) {
            std::cout << (first_phase ? "" : ",") << phase;
            first_phase = false;
        }
        std::cout
                  << " shuffle=" << (cfg.shuffle ? "true" : "false")
                  << " unique_keys=" << keys.size() << '\n';
    }
    std::cout << "compiled_static_index=fallback_map";
#if defined(STRING_BIMAP_HAS_XCDAT)
    std::cout << ",xcdat";
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
    std::cout << ",marisa";
#endif
#if defined(STRING_BIMAP_HAS_KEYVI)
    std::cout << ",keyvi";
#endif
    std::cout << ",fst\n";
    std::cout << "compiled_delta_index=unordered_map";
#if defined(STRING_BIMAP_HAS_ARRAY_HASH)
    std::cout << ",array_map";
#endif
#if defined(STRING_BIMAP_HAS_HAT_TRIE)
    std::cout << ",hat-trie";
#endif
    std::cout << '\n';
    std::cout << "selected_profile=" << profile_name(cfg.profile) << '\n';

    StringBimap dict(0, cfg.profile);
    std::vector<StringId> ids;
    ids.resize(keys.size());

    volatile std::uint64_t sink = 0;
    const auto rss_before = current_rss_bytes();

    double insert_ms = 0.0;
    if (has_phase(cfg, "insert")) {
        insert_ms = run_ms([&] {
            for (std::size_t i = 0; i < keys.size(); ++i) {
                ids[i] = dict.insert(keys[i]);
            }
        });
    }
    print_metric("insert", insert_ms, has_phase(cfg, "insert") ? keys.size() : 0);
    const auto rss_after_insert = current_rss_bytes();
    const auto usage_after_insert = dict.memory_usage();

    double lookup_ms = 0.0;
    if (has_phase(cfg, "find")) {
        lookup_ms = run_ms([&] {
            for (const auto idx : order) {
                const auto id = dict.find_id(read_keys[idx]);
                sink += id.value_or(0);
            }
        });
    }
    print_metric("find_id", lookup_ms, has_phase(cfg, "find") ? read_keys.size() : 0);

    double decode_ms = 0.0;
    if (has_phase(cfg, "get")) {
        decode_ms = run_ms([&] {
            for (const auto idx : order) {
                const auto id = dict.find_id(read_keys[idx]);
                if (id.has_value()) {
                    sink += dict.get_string(*id).size();
                }
            }
        });
    }
    print_metric("get_string", decode_ms, has_phase(cfg, "get") ? read_keys.size() : 0);

    std::string prefix = cfg.prefix;
    if (prefix.empty()) {
        prefix = "prefix/0000/";
    }
    if (cfg.prefix.empty() && !cfg.csv_path.empty() && !keys.empty()) {
        prefix = keys.front().substr(0, std::min<std::size_t>(4, keys.front().size()));
    }
    std::size_t prefix_hits = 0;
    double prefix_ms = 0.0;
    if (has_phase(cfg, "prefix")) {
        prefix_ms = run_ms([&] {
            dict.for_each_with_prefix(prefix, [&](StringId id, std::string_view value) {
                sink += id + value.size();
                ++prefix_hits;
            });
        });
    }
    print_metric("prefix_scan", prefix_ms, prefix_hits);

    if (has_phase(cfg, "erase")) {
        for (std::size_t i = 0; i < ids.size(); i += 5) {
            const bool erased = dict.erase(ids[i]);
            sink += erased ? 1u : 0u;
        }
    }

    if (cfg.release_inputs_before_compact) {
        release_vector(keys);
        release_vector(read_keys);
        release_vector(order);
    }

    double compact_ms = 0.0;
    if (has_phase(cfg, "compact")) {
        compact_ms = run_ms([&] {
            dict.compact();
        });
    }
    print_metric("compact", compact_ms, dict.live_size());
    const auto rss_after_compact = current_rss_bytes();
    const auto usage_after_compact = dict.memory_usage();

    std::size_t compact_prefix_hits = 0;
    double compact_prefix_ms = 0.0;
    if (has_phase(cfg, "prefix_compact")) {
        compact_prefix_ms = run_ms([&] {
            dict.for_each_with_prefix(prefix, [&](StringId id, std::string_view value) {
                sink += id + value.size();
                ++compact_prefix_hits;
            });
        });
    }
    print_metric("prefix_scan_compact", compact_prefix_ms, compact_prefix_hits);

    std::size_t compact_prefix_unordered_hits = 0;
    double compact_prefix_unordered_ms = 0.0;
    if (has_phase(cfg, "prefix_compact")) {
        compact_prefix_unordered_ms = run_ms([&] {
            dict.for_each_with_prefix_unordered(prefix, [&](StringId id, std::string_view value) {
                sink += id + value.size();
                ++compact_prefix_unordered_hits;
            });
        });
    }
    print_metric("prefix_scan_compact_unordered", compact_prefix_unordered_ms, compact_prefix_unordered_hits);

    std::stringstream serialized(std::ios::in | std::ios::out | std::ios::binary);
    double save_ms = 0.0;
    if (has_phase(cfg, "save")) {
        if (cfg.serialized_file_path.empty()) {
            save_ms = run_ms([&] {
                serialized.str({});
                serialized.clear();
                dict.save(serialized);
            });
        } else {
            save_ms = run_ms([&] {
                if (cfg.save_compacted) {
                    dict.save_compacted(cfg.serialized_file_path);
                } else {
                    dict.save(cfg.serialized_file_path);
                }
            });
        }
    }
    const auto serialized_size = cfg.serialized_file_path.empty()
                                     ? serialized.str().size()
                                     : static_cast<std::size_t>(file_size_or_zero(cfg.serialized_file_path));
    print_metric("save", save_ms, has_phase(cfg, "save") ? dict.live_size() : 0);
    std::cout << "serialized_bytes=" << serialized_size << '\n';

    double load_ms = 0.0;
    StringBimap loaded(0, cfg.profile);
    if (cfg.release_serialized_before_load && has_phase(cfg, "load") && !has_phase(cfg, "save") &&
        cfg.serialized_file_path.empty()) {
        std::cerr << "warning: --release-serialized-before-load has no effect without serialized data\n";
    }
    if (has_phase(cfg, "load")) {
        if (cfg.serialized_file_path.empty()) {
            if (!has_phase(cfg, "save")) {
                throw std::invalid_argument("load phase without save requires --serialized-file");
            }
            load_ms = run_ms([&] {
                serialized.seekg(0);
                loaded = StringBimap::load(serialized);
            });
        } else {
            load_ms = run_ms([&] {
                loaded = StringBimap::load(cfg.serialized_file_path);
            });
        }
    }
    print_metric("load", load_ms, has_phase(cfg, "load") ? loaded.live_size() : 0);
    const auto rss_after_load = current_rss_bytes();
    const auto usage_after_load = loaded.memory_usage();

    std::vector<StringId> loaded_ids;
    if ((has_phase(cfg, "find_loaded") || has_phase(cfg, "get_loaded") || has_phase(cfg, "steady_loaded")) &&
        has_phase(cfg, "load")) {
        loaded_ids.reserve(loaded.live_size());
        loaded.for_each_live([&](StringId id, std::string_view) { loaded_ids.push_back(id); });
    }

    double loaded_find_ms = 0.0;
    if (has_phase(cfg, "find_loaded")) {
        loaded_find_ms = run_ms([&] {
            for (std::size_t repeat = 0; repeat < cfg.read_repeats; ++repeat) {
                for (const auto id : loaded_ids) {
                    sink += loaded.find_id(loaded.get_string(id)).value_or(0);
                }
            }
        });
    }
    print_metric("find_id_loaded", loaded_find_ms,
                 has_phase(cfg, "find_loaded") ? loaded_ids.size() * cfg.read_repeats : 0);

    double loaded_get_ms = 0.0;
    if (has_phase(cfg, "get_loaded")) {
        loaded_get_ms = run_ms([&] {
            for (std::size_t repeat = 0; repeat < cfg.read_repeats; ++repeat) {
                for (const auto id : loaded_ids) {
                    sink += loaded.get_string(id).size();
                }
            }
        });
    }
    print_metric("get_string_loaded", loaded_get_ms,
                 has_phase(cfg, "get_loaded") ? loaded_ids.size() * cfg.read_repeats : 0);

    double loaded_steady_ms = 0.0;
    std::size_t loaded_steady_ops = 0;
    if (has_phase(cfg, "steady_loaded")) {
        loaded_steady_ms = run_ms([&] {
            for (std::size_t repeat = 0; repeat < cfg.read_repeats; ++repeat) {
                for (const auto id : loaded_ids) {
                    const auto value = loaded.get_string(id);
                    for (std::size_t i = 0; i < cfg.loaded_find_ratio; ++i) {
                        sink += loaded.find_id(value).value_or(0);
                    }
                    for (std::size_t i = 0; i < cfg.loaded_get_ratio; ++i) {
                        sink += loaded.get_string(id).size();
                    }
                }
            }
        });
        loaded_steady_ops =
            loaded_ids.size() * cfg.read_repeats * (cfg.loaded_find_ratio + cfg.loaded_get_ratio);
    }
    print_metric("steady_loaded", loaded_steady_ms, loaded_steady_ops);

    std::cout << "phase=delta_heavy\n";
    print_memory("rss_before", rss_before);
    print_memory("rss_after_insert", rss_after_insert);
    print_memory_delta("rss_insert", rss_before, rss_after_insert);
    print_usage("internal_after_insert", usage_after_insert);

    std::cout << "phase=base_heavy\n";
    print_memory("rss_after_compact", rss_after_compact);
    print_memory_delta("rss_compact_vs_insert", rss_after_insert, rss_after_compact);
    print_memory_delta("rss_compact_vs_start", rss_before, rss_after_compact);
    print_usage("internal_after_compact", usage_after_compact);

    std::cout << "phase=loaded_copy\n";
    print_memory("rss_after_load", rss_after_load);
    print_memory_delta("rss_load_vs_compact", rss_after_compact, rss_after_load);
    print_memory_delta("rss_load_vs_start", rss_before, rss_after_load);
    print_memory("rss_peak", peak_rss_bytes());
    print_usage("internal_after_load", usage_after_load);

    std::cout << "sink=" << sink << '\n';
    return 0;
}
