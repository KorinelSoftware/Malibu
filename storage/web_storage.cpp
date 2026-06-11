// storage/web_storage.cpp
// localStorage / sessionStorage backing (insertion-ordered key/value).

#include "malibu/storage/local_storage.h"

namespace malibu::storage {

std::optional<std::string> Storage::get_item(std::string_view key) const {
    for (const auto& [k, v] : items_) if (k == key) return v;
    return std::nullopt;
}

bool Storage::has(std::string_view key) const {
    for (const auto& [k, v] : items_) if (k == key) return true;
    return false;
}

void Storage::set_item(std::string_view key, std::string_view value) {
    for (auto& [k, v] : items_) if (k == key) { v = std::string(value); return; }
    items_.emplace_back(std::string(key), std::string(value));
}

void Storage::remove_item(std::string_view key) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->first == key) { items_.erase(it); return; }
    }
}

std::optional<std::string> Storage::key(size_t index) const {
    if (index >= items_.size()) return std::nullopt;
    return items_[index].first;
}

} // namespace malibu::storage
