#include "common.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using string_bimap_test::expect;

std::string run_command(const std::string& command) {
    std::array<char, 256> buffer{};
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("failed to open pipe");
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    const int status = pclose(pipe);
    if (status != 0) {
        throw std::runtime_error("command failed: " + command);
    }
    return output;
}

void test_tool_build_dump_lookup() {
    const std::string json_path = "/tmp/pthash_tool_values.json";
    const std::string snapshot_path = "/tmp/pthash_tool_values.bin";
    const std::string sidecar_path = snapshot_path + ".native.pthash";

    {
        std::ofstream out(json_path);
        out << "{\"values\":[\"Bond\",\"Common Stock\",\"ETF\"]}\n";
    }

    std::filesystem::remove(snapshot_path);
    std::filesystem::remove(sidecar_path);

    const auto build_out =
        run_command("./string_bimap_pthash_tool build --json-array-file " + json_path +
                    " --output " + snapshot_path);
    expect(build_out.find("size=3") != std::string::npos, "tool build should report cardinality");
    expect(std::filesystem::exists(snapshot_path), "tool build should write snapshot");
    expect(std::filesystem::exists(sidecar_path), "tool build should write native sidecar");

    const auto dump_out =
        run_command("./string_bimap_pthash_tool dump --input " + snapshot_path);
    expect(dump_out.find("Bond") != std::string::npos, "tool dump should print values");
    expect(dump_out.find("Common Stock") != std::string::npos, "tool dump should print all rows");

    const auto lookup_out = run_command("./string_bimap_pthash_tool lookup --input " +
                                        snapshot_path + " --value \"Common Stock\"");
    expect(lookup_out.find("found=true") != std::string::npos, "tool lookup should resolve members");
    expect(lookup_out.find("value=Common Stock") != std::string::npos,
           "tool lookup should print the matched value");

    std::filesystem::remove(json_path);
    std::filesystem::remove(snapshot_path);
    std::filesystem::remove(sidecar_path);
}

void test_tool_build_from_stdin() {
    const std::string snapshot_path = "/tmp/pthash_tool_stdin.bin";
    const std::string sidecar_path = snapshot_path + ".native.pthash";
    std::filesystem::remove(snapshot_path);
    std::filesystem::remove(sidecar_path);

    const auto build_out = run_command(
        "printf '%s' '[\"Bond\",\"Common Stock\",\"ETF\"]' | "
        "./string_bimap_pthash_tool build --json-array-file - --output " + snapshot_path);
    expect(build_out.find("size=3") != std::string::npos, "stdin build should report cardinality");
    expect(std::filesystem::exists(snapshot_path), "stdin build should write snapshot");
    expect(std::filesystem::exists(sidecar_path), "stdin build should write native sidecar");

    std::filesystem::remove(snapshot_path);
    std::filesystem::remove(sidecar_path);
}

}  // namespace

int main() {
    test_tool_build_dump_lookup();
    test_tool_build_from_stdin();
    return 0;
}
