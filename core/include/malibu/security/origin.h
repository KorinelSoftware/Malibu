#pragma once
// core/include/malibu/security/origin.h
// Origin model - scheme, host, port.

#include <string>
#include <string_view>
#include <cstdint>

namespace malibu::security {

struct Origin {
    std::string scheme;
    std::string host;
    uint16_t port = 0;
    
    bool operator==(const Origin&) const noexcept = default;
    bool operator!=(const Origin&) const noexcept = default;
    
    bool is_opaque() const noexcept;
    bool same_origin(const Origin& other) const noexcept;
    std::string serialize() const noexcept;
    static Origin parse(std::string_view str);
    static Origin opaque();
};

} // namespace malibu::security