#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "detail/string_map.hpp"
#include "packed_string_arena.hpp"
#include "serialization.hpp"
#include "third_party/cpp_fstlib/fstlib.h"
#include "types.hpp"

#if defined(STRING_BIMAP_HAS_XCDAT)
#include <xcdat.hpp>
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
#include <marisa.h>
#endif
#if defined(STRING_BIMAP_HAS_KEYVI)
#include <keyvi/dictionary/dictionary.h>
#include <keyvi/dictionary/dictionary_types.h>
#endif

namespace string_bimap {

class BaseSegment {
public:
    struct BuildItem {
        StringId id;
        std::string value;
    };

    explicit BaseSegment(BackendProfile profile = BackendProfile::FastLookup)
        : profile_(profile) {}

    BaseSegment(const BaseSegment&) = delete;
    BaseSegment& operator=(const BaseSegment&) = delete;
    BaseSegment(BaseSegment&&) noexcept = default;
    BaseSegment& operator=(BaseSegment&&) noexcept = default;

    ~BaseSegment() {
        release_keyvi_sidecar();
    }

    void set_backend_profile(BackendProfile profile) {
        profile_ = profile;
    }

    [[nodiscard]] std::optional<StringId> find_id(std::string_view value) const {
#if defined(STRING_BIMAP_HAS_XCDAT)
        if (use_xcdat_index()) {
            const auto local = trie_->lookup(value);
            if (!local.has_value()) {
                return std::nullopt;
            }
            return local_to_global_[static_cast<std::size_t>(*local)];
        }
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
        if (use_marisa_index()) {
            marisa::Agent agent;
            agent.set_query(value);
            if (!marisa_trie_.lookup(agent)) {
                return std::nullopt;
            }
            return local_to_global_[agent.key().id()];
        }
#endif
 #if defined(STRING_BIMAP_HAS_KEYVI)
        if (use_keyvi_index()) {
            const std::string key(value);
            if (!keyvi_dictionary_->Contains(key)) {
                return std::nullopt;
            }
            const auto match = (*keyvi_dictionary_)[key];
            if (!match) {
                return std::nullopt;
            }
            return parse_string_id(match->GetValueAsString());
        }
#endif
        if (use_fst_index()) {
            StringId id = kInvalidId;
            if (!fst_map_->exact_match_search(value, id)) {
                return std::nullopt;
            }
            return id;
        }
        const auto it = detail::map_find(fallback_index_, value);
        if (it == fallback_index_.end()) {
            return std::nullopt;
        }
        return detail::map_value(it);
    }

    [[nodiscard]] bool contains_id(StringId id) const noexcept {
        return id < entries_by_id_.size() && entries_by_id_[id].live();
    }

    [[nodiscard]] std::string_view get_string(StringId id) const noexcept {
        if (!contains_id(id)) {
            return {};
        }
        return arena_.view(entries_by_id_[id]);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return live_size_;
    }

    [[nodiscard]] SegmentMemoryUsage memory_usage() const noexcept {
        SegmentMemoryUsage usage;
        usage.arena_bytes = arena_.bytes_reserved();
        usage.entry_table_bytes = entries_by_id_.capacity() * sizeof(EntryLocation);
        usage.fallback_index_bytes = detail::estimate_map_memory_bytes(fallback_index_);
#if defined(STRING_BIMAP_HAS_XCDAT)
        if (use_xcdat_index()) {
            usage.compact_index_bytes += local_to_global_.capacity() * sizeof(StringId);
            usage.compact_index_bytes += static_cast<std::size_t>(xcdat::memory_in_bytes(*trie_));
        }
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
        if (use_marisa_index()) {
            usage.compact_index_bytes += local_to_global_.capacity() * sizeof(StringId);
            usage.compact_index_bytes += marisa_trie_.total_size();
        }
#endif
 #if defined(STRING_BIMAP_HAS_KEYVI)
        if (use_keyvi_index()) {
            usage.compact_index_bytes += keyvi_sidecar_size_bytes_;
        }
#endif
        if (use_fst_index()) {
            usage.compact_index_bytes += fst_bytecode_.capacity();
        }
        return usage;
    }

    [[nodiscard]] bool has_native_compact_index() const noexcept {
#if defined(STRING_BIMAP_HAS_XCDAT)
        if (profile_ == BackendProfile::CompactMemory && trie_.has_value()) {
            return true;
        }
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
        if (profile_ == BackendProfile::CompactMemoryMarisa && marisa_ready_) {
            return true;
        }
#endif
 #if defined(STRING_BIMAP_HAS_KEYVI)
        if (profile_ == BackendProfile::CompactMemoryKeyvi && keyvi_dictionary_) {
            return true;
        }
#endif
        if (profile_ == BackendProfile::CompactMemoryFst && fst_map_) {
            return true;
        }
        return false;
    }

    void save_native_compact_index(const std::string& path) const {
#if defined(STRING_BIMAP_HAS_XCDAT)
        if (use_xcdat_index()) {
            xcdat::save(*trie_, detail::compact_trie_sidecar_path(path));
            detail::write_vector_file(detail::compact_ids_sidecar_path(path), local_to_global_);
            return;
        }
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
        if (use_marisa_index()) {
            marisa_trie_.save(detail::compact_marisa_sidecar_path(path).c_str());
            detail::write_vector_file(detail::compact_ids_sidecar_path(path), local_to_global_);
            return;
        }
#endif
 #if defined(STRING_BIMAP_HAS_KEYVI)
        if (use_keyvi_index()) {
            std::filesystem::copy_file(
                keyvi_sidecar_path_,
                detail::compact_keyvi_sidecar_path(path),
                std::filesystem::copy_options::overwrite_existing);
            return;
        }
#endif
        if (use_fst_index()) {
            std::ofstream out(detail::compact_fst_sidecar_path(path), std::ios::binary);
            if (!out) {
                throw std::runtime_error("failed to open fst sidecar for writing");
            }
            detail::write_bytes(out, fst_bytecode_.data(), fst_bytecode_.size());
            return;
        }
#if defined(STRING_BIMAP_HAS_XCDAT) || defined(STRING_BIMAP_HAS_MARISA)
        throw std::runtime_error("compact trie index is not available");
#else
        throw std::runtime_error("compact trie index serialization requires an optional compact backend");
#endif
    }

    [[nodiscard]] bool load_native_compact_index(std::vector<BuildItem> items, const std::string& path) {
#if defined(STRING_BIMAP_HAS_XCDAT)
        if (profile_ == BackendProfile::CompactMemory) {
            try {
                rebuild_storage(items);
                fallback_index_.clear();
                trie_ = xcdat::load<TrieType>(detail::compact_trie_sidecar_path(path));
                local_to_global_ = detail::read_vector_file<StringId>(detail::compact_ids_sidecar_path(path));
                if (!trie_.has_value() || local_to_global_.size() != trie_->num_keys()) {
                    throw std::runtime_error("compact trie sidecar size mismatch");
                }
                return true;
            } catch (...) {
                trie_.reset();
                local_to_global_.clear();
                return false;
            }
        }
#else
        (void)items;
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
        if (profile_ == BackendProfile::CompactMemoryMarisa) {
            try {
                rebuild_storage(items);
                fallback_index_.clear();
                marisa_trie_.load(detail::compact_marisa_sidecar_path(path).c_str());
                local_to_global_ = detail::read_vector_file<StringId>(detail::compact_ids_sidecar_path(path));
                if (local_to_global_.size() != marisa_trie_.num_keys()) {
                    throw std::runtime_error("compact marisa sidecar size mismatch");
                }
                marisa_ready_ = true;
                return true;
            } catch (...) {
                marisa_trie_.clear();
                local_to_global_.clear();
                marisa_ready_ = false;
                return false;
            }
        }
#endif
 #if defined(STRING_BIMAP_HAS_KEYVI)
        if (profile_ == BackendProfile::CompactMemoryKeyvi) {
            try {
                rebuild_storage(items);
                fallback_index_.clear();
                release_keyvi_sidecar();
                keyvi_sidecar_path_ = detail::compact_keyvi_sidecar_path(path);
                keyvi_dictionary_ = std::make_unique<keyvi::dictionary::Dictionary>(keyvi_sidecar_path_);
                keyvi_sidecar_size_bytes_ = file_size_or_zero(keyvi_sidecar_path_);
                keyvi_owns_sidecar_ = false;
                return true;
            } catch (...) {
                keyvi_dictionary_.reset();
                keyvi_sidecar_path_.clear();
                keyvi_sidecar_size_bytes_ = 0;
                keyvi_owns_sidecar_ = false;
                return false;
            }
        }
#endif
        if (profile_ == BackendProfile::CompactMemoryFst) {
            try {
                rebuild_storage(items);
                fallback_index_.clear();
                std::ifstream in(detail::compact_fst_sidecar_path(path), std::ios::binary);
                if (!in) {
                    throw std::runtime_error("failed to open fst sidecar");
                }
                fst_bytecode_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                fst_map_ = std::make_unique<FstMapType>(fst_bytecode_);
                if (!*fst_map_) {
                    throw std::runtime_error("failed to load fst sidecar");
                }
                return true;
            } catch (...) {
                fst_map_.reset();
                fst_bytecode_.clear();
                return false;
            }
        }
        (void)path;
        return false;
    }

    template <class Func>
    void for_each_with_prefix(std::string_view prefix, Func&& func) const {
#if defined(STRING_BIMAP_HAS_XCDAT)
        if (use_xcdat_index()) {
            std::vector<StringId> ids;
            trie_->predictive_search(prefix, [&](std::uint64_t local_id, std::string_view) {
                const auto id = local_to_global_[static_cast<std::size_t>(local_id)];
                if (id != kInvalidId) {
                    ids.push_back(id);
                }
            });
            std::sort(ids.begin(), ids.end());
            for (const auto id : ids) {
                func(id, get_string(id));
            }
            return;
        }
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
        if (use_marisa_index()) {
            std::vector<StringId> ids;
            marisa::Agent agent;
            agent.set_query(prefix);
            while (marisa_trie_.predictive_search(agent)) {
                const auto id = local_to_global_[agent.key().id()];
                if (id != kInvalidId) {
                    ids.push_back(id);
                }
            }
            std::sort(ids.begin(), ids.end());
            for (const auto id : ids) {
                func(id, get_string(id));
            }
            return;
        }
#endif
 #if defined(STRING_BIMAP_HAS_KEYVI)
        if (use_keyvi_index()) {
            std::vector<StringId> ids;
            const auto matches = keyvi_dictionary_->GetPrefixCompletion(std::string(prefix));
            for (auto it = matches.begin(); it != matches.end(); ++it) {
                if (!*it) {
                    continue;
                }
                ids.push_back(parse_string_id((*it)->GetValueAsString()));
            }
            std::sort(ids.begin(), ids.end());
            for (const auto id : ids) {
                func(id, get_string(id));
            }
            return;
        }
#endif
        if (use_fst_index()) {
            std::vector<StringId> ids;
            fst_map_->predictive_search(prefix, [&](const std::string&, const StringId& id) {
                ids.push_back(id);
            });
            std::sort(ids.begin(), ids.end());
            for (const auto id : ids) {
                func(id, get_string(id));
            }
            return;
        }
        for (StringId id = 0; id < entries_by_id_.size(); ++id) {
            if (!entries_by_id_[id].live()) {
                continue;
            }
            const auto value = arena_.view(entries_by_id_[id]);
            if (value.substr(0, prefix.size()) == prefix) {
                func(id, value);
            }
        }
    }

    template <class Func>
    void for_each_with_prefix_unordered(std::string_view prefix, Func&& func) const {
#if defined(STRING_BIMAP_HAS_XCDAT)
        if (use_xcdat_index()) {
            trie_->predictive_search(prefix, [&](std::uint64_t local_id, std::string_view) {
                const auto id = local_to_global_[static_cast<std::size_t>(local_id)];
                if (id != kInvalidId) {
                    func(id, get_string(id));
                }
            });
            return;
        }
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
        if (use_marisa_index()) {
            marisa::Agent agent;
            agent.set_query(prefix);
            while (marisa_trie_.predictive_search(agent)) {
                const auto id = local_to_global_[agent.key().id()];
                if (id != kInvalidId) {
                    func(id, get_string(id));
                }
            }
            return;
        }
#endif
 #if defined(STRING_BIMAP_HAS_KEYVI)
        if (use_keyvi_index()) {
            const auto matches = keyvi_dictionary_->GetPrefixCompletion(std::string(prefix));
            for (auto it = matches.begin(); it != matches.end(); ++it) {
                if (!*it) {
                    continue;
                }
                const auto id = parse_string_id((*it)->GetValueAsString());
                func(id, get_string(id));
            }
            return;
        }
#endif
        if (use_fst_index()) {
            fst_map_->predictive_search(prefix, [&](const std::string&, const StringId& id) {
                func(id, get_string(id));
            });
            return;
        }
        for_each_with_prefix(prefix, std::forward<Func>(func));
    }

    void rebuild(std::vector<BuildItem> items) {
        rebuild_storage(items);
        build_lookup(items);
    }

private:
    void release_fallback_index() {
        detail::StringIdMap empty;
        fallback_index_.swap(empty);
    }

    void rebuild_storage(std::vector<BuildItem>& items) {
        std::sort(items.begin(), items.end(), [](const BuildItem& lhs, const BuildItem& rhs) {
            if (lhs.id != rhs.id) {
                return lhs.id < rhs.id;
            }
            return lhs.value < rhs.value;
        });

        std::size_t reserve_bytes = 0;
        StringId max_id = 0;
        for (const auto& item : items) {
            reserve_bytes += item.value.size() + 1;
            max_id = std::max(max_id, item.id);
        }

        arena_.clear(reserve_bytes);
        entries_by_id_.assign(items.empty() ? 0 : static_cast<std::size_t>(max_id) + 1, EntryLocation{});
        live_size_ = 0;

        for (const auto& item : items) {
            entries_by_id_[item.id] = arena_.append(item.value);
            ++live_size_;
        }
    }
    [[nodiscard]] bool use_xcdat_index() const noexcept {
#if defined(STRING_BIMAP_HAS_XCDAT)
        return profile_ == BackendProfile::CompactMemory && trie_.has_value();
#else
        return false;
#endif
    }

    [[nodiscard]] bool use_marisa_index() const noexcept {
#if defined(STRING_BIMAP_HAS_MARISA)
        return profile_ == BackendProfile::CompactMemoryMarisa && marisa_ready_;
#else
        return false;
#endif
    }

    [[nodiscard]] bool use_keyvi_index() const noexcept {
#if defined(STRING_BIMAP_HAS_KEYVI)
        return profile_ == BackendProfile::CompactMemoryKeyvi && static_cast<bool>(keyvi_dictionary_);
#else
        return false;
#endif
    }

    [[nodiscard]] bool use_fst_index() const noexcept {
        return profile_ == BackendProfile::CompactMemoryFst && static_cast<bool>(fst_map_);
    }

    static StringId parse_string_id(const std::string& value) {
        return static_cast<StringId>(std::stoull(value));
    }

    static std::size_t file_size_or_zero(const std::string& path) noexcept {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        return ec ? 0 : static_cast<std::size_t>(size);
    }

    static std::string make_temp_keyvi_sidecar_path() {
        const auto stamp = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return (std::filesystem::temp_directory_path() /
                ("string_bimap-keyvi-" + std::to_string(stamp) + ".kv")).string();
    }

    void release_keyvi_sidecar() noexcept {
#if defined(STRING_BIMAP_HAS_KEYVI)
        keyvi_dictionary_.reset();
        if (keyvi_owns_sidecar_ && !keyvi_sidecar_path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(keyvi_sidecar_path_, ec);
        }
        keyvi_sidecar_path_.clear();
        keyvi_sidecar_size_bytes_ = 0;
        keyvi_owns_sidecar_ = false;
#endif
    }

    void build_lookup(const std::vector<BuildItem>& items) {
        release_fallback_index();
        detail::map_reserve(fallback_index_, items.size());
        for (const auto& item : items) {
            detail::map_insert_or_assign(fallback_index_, item.value, item.id);
        }

        std::vector<std::pair<std::string, StringId>> sorted_keys;
        sorted_keys.reserve(items.size());
        for (const auto& item : items) {
            sorted_keys.emplace_back(item.value, item.id);
        }
        std::sort(sorted_keys.begin(), sorted_keys.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });
        sorted_keys.erase(std::unique(sorted_keys.begin(), sorted_keys.end(), [](const auto& lhs, const auto& rhs) {
                             return lhs.first == rhs.first;
                         }),
                         sorted_keys.end());

        std::vector<std::string> keys;
        keys.reserve(sorted_keys.size());
        for (const auto& item : sorted_keys) {
            keys.push_back(item.first);
        }

#if defined(STRING_BIMAP_HAS_XCDAT)
        trie_.reset();
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
        marisa_trie_.clear();
        marisa_ready_ = false;
#endif
#if defined(STRING_BIMAP_HAS_KEYVI)
        release_keyvi_sidecar();
#endif
        fst_map_.reset();
        fst_bytecode_.clear();

        if (sorted_keys.empty()) {
            return;
        }

#if defined(STRING_BIMAP_HAS_XCDAT)
        if (profile_ == BackendProfile::CompactMemory) {
            trie_ = TrieType(keys);
            local_to_global_.assign(keys.size(), kInvalidId);
            for (std::size_t local_id = 0; local_id < keys.size(); ++local_id) {
                const auto key = trie_->decode(local_id);
                const auto global = detail::map_find(fallback_index_, key);
                if (global != fallback_index_.end()) {
                    local_to_global_[local_id] = detail::map_value(global);
                }
            }
            release_fallback_index();
        } else {
            local_to_global_.clear();
        }
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
        if (profile_ == BackendProfile::CompactMemoryMarisa) {
            marisa::Keyset keyset;
            for (const auto& key : keys) {
                keyset.push_back(key);
            }
            marisa_trie_.build(keyset);
            marisa_ready_ = true;
            local_to_global_.assign(marisa_trie_.num_keys(), kInvalidId);
            marisa::Agent agent;
            for (std::size_t local_id = 0; local_id < marisa_trie_.num_keys(); ++local_id) {
                agent.set_query(local_id);
                marisa_trie_.reverse_lookup(agent);
                const auto global = detail::map_find(fallback_index_, agent.key().str());
                if (global != fallback_index_.end()) {
                    local_to_global_[local_id] = detail::map_value(global);
                }
            }
            release_fallback_index();
        }
#endif
#if defined(STRING_BIMAP_HAS_KEYVI)
        if (profile_ == BackendProfile::CompactMemoryKeyvi) {
            keyvi::dictionary::IntDictionaryCompiler compiler;
            for (const auto& item : sorted_keys) {
                compiler.Add(item.first, item.second);
            }
            compiler.Compile();
            keyvi_sidecar_path_ = make_temp_keyvi_sidecar_path();
            compiler.WriteToFile(keyvi_sidecar_path_);
            keyvi_dictionary_ = std::make_unique<keyvi::dictionary::Dictionary>(keyvi_sidecar_path_);
            keyvi_sidecar_size_bytes_ = file_size_or_zero(keyvi_sidecar_path_);
            keyvi_owns_sidecar_ = true;
            release_fallback_index();
        }
#endif
        if (profile_ == BackendProfile::CompactMemoryFst) {
            std::vector<std::pair<std::string, StringId>> fst_items;
            fst_items.reserve(sorted_keys.size());
            for (const auto& item : sorted_keys) {
                fst_items.emplace_back(item.first, item.second);
            }
            std::ostringstream out(std::ios::binary);
            const auto [result, error_index] = fst::compile<StringId>(fst_items, out, true);
            if (result != fst::Result::Success) {
                throw std::runtime_error("fst compile failed at input index " + std::to_string(error_index));
            }
            fst_bytecode_ = out.str();
            fst_map_ = std::make_unique<FstMapType>(fst_bytecode_);
            if (!*fst_map_) {
                throw std::runtime_error("fst map construction failed");
            }
            release_fallback_index();
        }
    }

    PackedStringArena arena_;
    std::vector<EntryLocation> entries_by_id_;
    detail::StringIdMap fallback_index_;
    BackendProfile profile_;
    std::size_t live_size_ = 0;

#if defined(STRING_BIMAP_HAS_XCDAT)
    using TrieType = xcdat::trie_8_type;
    std::optional<TrieType> trie_;
#endif
#if defined(STRING_BIMAP_HAS_MARISA)
    marisa::Trie marisa_trie_;
    bool marisa_ready_ = false;
#endif
#if defined(STRING_BIMAP_HAS_KEYVI)
    std::unique_ptr<keyvi::dictionary::Dictionary> keyvi_dictionary_;
    std::string keyvi_sidecar_path_;
    std::size_t keyvi_sidecar_size_bytes_ = 0;
    bool keyvi_owns_sidecar_ = false;
#endif
    using FstMapType = fst::map<StringId>;
    std::string fst_bytecode_;
    std::unique_ptr<FstMapType> fst_map_;
    std::vector<StringId> local_to_global_;
};

}  // namespace string_bimap
