#include <iostream>

#include <string_bimap/string_bimap.hpp>

int main() {
    string_bimap::StringBimap dict;

    const auto alpha = dict.insert("alpha");
    const auto beta = dict.insert("beta");
    const auto alpha_again = dict.insert("alpha");

    std::cout << "alpha=" << alpha << "\n";
    std::cout << "beta=" << beta << "\n";
    std::cout << "alpha_again=" << alpha_again << "\n";
    std::cout << "beta_value=" << dict.get_string(beta) << "\n";

    dict.erase(beta);
    dict.compact();

    std::cout << "contains(alpha)=" << dict.contains("alpha") << "\n";
    std::cout << "contains(beta)=" << dict.contains("beta") << "\n";
}
