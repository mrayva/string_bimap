#include <string_view>

#include <string_bimap/string_bimap.hpp>

#if defined(STRING_BIMAP_EXPECT_OPTIONAL_BACKENDS)
#if !defined(STRING_BIMAP_HAS_XCDAT)
#error "installed package did not rediscover xcdat"
#endif
#if !defined(STRING_BIMAP_HAS_MARISA)
#error "installed package did not rediscover marisa"
#endif
#if !defined(STRING_BIMAP_HAS_HAT_TRIE)
#error "installed package did not rediscover hat-trie"
#endif
#if !defined(STRING_BIMAP_HAS_ARRAY_HASH)
#error "installed package did not rediscover array-hash"
#endif
#endif

int main() {
    string_bimap::StringBimap bimap;
    const auto alpha = bimap.insert("alpha");
    const auto beta = bimap.insert("beta");
    if (bimap.find_id("alpha") != alpha || bimap.get_string(beta) != std::string_view("beta")) {
        return 1;
    }

    bimap.compact();
    return bimap.find_id("beta") == beta ? 0 : 2;
}
