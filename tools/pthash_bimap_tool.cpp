#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <string_bimap/pthash_bimap.hpp>

namespace {

using string_bimap::PthashBimap;
using string_bimap::PthashBuildOptions;

struct InputSpec {
    std::string json_array_file;
    std::string csv_file;
    std::string csv_column;
    std::size_t csv_index = 0;
    bool csv_has_header = true;
};

[[noreturn]] void usage() {
    std::cerr
        << "usage:\n"
        << "  string_bimap_pthash_tool build --output <snapshot.bin> "
           "(--json-array-file <path> | --csv-file <path> [--csv-column <name> | --csv-index <n>] "
           "[--csv-no-header])\n"
        << "  string_bimap_pthash_tool dump --input <snapshot.bin> [--values-only]\n"
        << "  string_bimap_pthash_tool stats --input <snapshot.bin>\n"
        << "  string_bimap_pthash_tool lookup --input <snapshot.bin> --value <string>\n";
    std::exit(1);
}

std::string require_arg(int& i, int argc, char** argv, const char* flag) {
    if (i + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + flag);
    }
    return argv[++i];
}

InputSpec parse_input_spec(int& i, int argc, char** argv) {
    InputSpec spec;
    for (; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--json-array-file") {
            spec.json_array_file = require_arg(i, argc, argv, "--json-array-file");
        } else if (arg == "--csv-file") {
            spec.csv_file = require_arg(i, argc, argv, "--csv-file");
        } else if (arg == "--csv-column") {
            spec.csv_column = require_arg(i, argc, argv, "--csv-column");
        } else if (arg == "--csv-index") {
            spec.csv_index = static_cast<std::size_t>(
                std::strtoull(require_arg(i, argc, argv, "--csv-index").c_str(), nullptr, 10));
        } else if (arg == "--csv-no-header") {
            spec.csv_has_header = false;
        } else {
            --i;
            break;
        }
    }
    return spec;
}

PthashBimap build_from_spec(const InputSpec& spec) {
    PthashBuildOptions options;
    if (!spec.json_array_file.empty()) {
        return PthashBimap::from_json_array_file(spec.json_array_file, options);
    }
    if (!spec.csv_file.empty()) {
        if (!spec.csv_column.empty()) {
            return PthashBimap::from_csv_file(spec.csv_file, spec.csv_column, spec.csv_index,
                                              spec.csv_has_header, options);
        }
        return PthashBimap::from_csv_file(spec.csv_file, std::nullopt, spec.csv_index,
                                          spec.csv_has_header, options);
    }
    throw std::runtime_error("build requires either --json-array-file or --csv-file");
}

void command_build(int argc, char** argv) {
    std::string output_path;
    InputSpec spec;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--output") {
            output_path = require_arg(i, argc, argv, "--output");
        } else {
            spec = parse_input_spec(i, argc, argv);
        }
    }

    if (output_path.empty()) {
        throw std::runtime_error("build requires --output");
    }

    const auto bimap = build_from_spec(spec);
    bimap.save(output_path);
    std::cout << "output=" << output_path << '\n';
    std::cout << "native_sidecar=" << output_path << ".native.pthash\n";
    std::cout << "size=" << bimap.size() << '\n';
    std::cout << "id_width_bytes=" << bimap.id_width_bytes() << '\n';
    std::cout << "seed=" << bimap.seed() << '\n';
}

void command_dump(int argc, char** argv) {
    std::string input_path;
    bool values_only = false;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--input") {
            input_path = require_arg(i, argc, argv, "--input");
        } else if (arg == "--values-only") {
            values_only = true;
        } else {
            usage();
        }
    }

    if (input_path.empty()) {
        throw std::runtime_error("dump requires --input");
    }

    const auto bimap = PthashBimap::load(input_path);
    for (std::uint32_t id = 0; id < bimap.size(); ++id) {
        if (values_only) {
            std::cout << bimap.by_id(id) << '\n';
        } else {
            std::cout << id << '\t' << bimap.by_id(id) << '\n';
        }
    }
}

void command_stats(int argc, char** argv) {
    std::string input_path;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--input") {
            input_path = require_arg(i, argc, argv, "--input");
        } else {
            usage();
        }
    }

    if (input_path.empty()) {
        throw std::runtime_error("stats requires --input");
    }

    const auto bimap = PthashBimap::load(input_path);
    const auto usage = bimap.memory_usage();
    const auto info = bimap.id_info();

    std::cout << "size=" << bimap.size() << '\n';
    std::cout << "seed=" << bimap.seed() << '\n';
    std::cout << "id_width_bytes=" << info.width_bytes << '\n';
    std::cout << "max_id=" << info.max_dense_id << '\n';
    std::cout << "memory_total_bytes=" << usage.total_bytes() << '\n';
    std::cout << "memory_string_bytes=" << usage.string_bytes << '\n';
    std::cout << "memory_string_vector_bytes=" << usage.string_vector_bytes << '\n';
    std::cout << "memory_mph_bytes=" << usage.mph_bytes << '\n';
}

void command_lookup(int argc, char** argv) {
    std::string input_path;
    std::string value;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--input") {
            input_path = require_arg(i, argc, argv, "--input");
        } else if (arg == "--value") {
            value = require_arg(i, argc, argv, "--value");
        } else {
            usage();
        }
    }

    if (input_path.empty() || value.empty()) {
        throw std::runtime_error("lookup requires --input and --value");
    }

    const auto bimap = PthashBimap::load(input_path);
    const auto id = bimap.find(value);
    if (!id.has_value()) {
        std::cout << "found=false\n";
        return;
    }

    std::cout << "found=true\n";
    std::cout << "id=" << *id << '\n';
    std::cout << "value=" << bimap.by_id(*id) << '\n';
    if (const auto compact = bimap.compact_id_for(*id)) {
        std::cout << "compact_id=" << PthashBimap::widen_compact_id(*compact) << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage();
        }
        const std::string_view command(argv[1]);
        if (command == "build") {
            command_build(argc, argv);
        } else if (command == "dump") {
            command_dump(argc, argv);
        } else if (command == "stats") {
            command_stats(argc, argv);
        } else if (command == "lookup") {
            command_lookup(argc, argv);
        } else {
            usage();
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
