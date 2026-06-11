#pragma once
// core/include/malibu/storage/local_storage.h
// Web Storage (localStorage / sessionStorage) per the HTML Living Standard.
// Insertion order is preserved so key(index) is stable.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace malibu::storage {

class Storage {
public:
    [[nodiscard]] std::optional<std::string> get_item(std::string_view key) const;
    void set_item(std::string_view key, std::string_view value);
    void remove_item(std::string_view key);
    void clear() { items_.clear(); }
    [[nodiscard]] size_t length() const noexcept { return items_.size(); }
    [[nodiscard]] std::optional<std::string> key(size_t index) const;
    [[nodiscard]] bool has(std::string_view key) const;

private:
    std::vector<std::pair<std::string, std::string>> items_;  // insertion order
};

using LocalStorage   = Storage;
using SessionStorage = Storage;

} // namespace malibu::storage
