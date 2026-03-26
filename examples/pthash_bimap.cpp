#include <iostream>
#include <string>
#include <vector>

#include <string_bimap/pthash_bimap.hpp>

int main() {
    std::vector<std::string> security_types = {
        "Bond",
        "Common Stock",
        "Depositary Receipt",
        "ETF",
        "Mutual Fund",
        "RIGHT",
    };

    string_bimap::PthashBimap bimap(security_types);

    std::cout << "size=" << bimap.size() << " id_width_bytes=" << bimap.id_width_bytes()
              << " seed=" << bimap.seed() << '\n';

    if (const auto id = bimap.find("ETF")) {
        std::cout << "ETF -> " << *id << '\n';
        std::cout << *id << " -> " << bimap.by_id(*id) << '\n';
    }

    std::cout << "csv/json loaders available via "
              << "PthashBimap::from_csv_file(...) and "
              << "PthashBimap::from_json_array_file(...)\n";

    return 0;
}
