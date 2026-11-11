#pragma once

#include "protocol.h"

#include <charconv>
#include <string_view>

namespace remote {

struct Ipv4Subnet {
    uint32_t network;
    uint32_t mask;

    bool Contains(uint32_t address) const { return (address & mask) == network; }
};

inline std::string_view TrimSubnetText(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

inline bool ParseSubnetNumber(std::string_view text, uint32_t max, uint32_t& value) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value <= max;
}

inline std::optional<Ipv4Subnet> ParseSubnet(std::string_view cidr) {
    const auto slash = cidr.find('/');
    uint32_t prefix = 0;
    if (slash == cidr.npos || !ParseSubnetNumber(cidr.substr(slash + 1), 32, prefix) || prefix == 0) {
        return std::nullopt;
    }

    auto ip = cidr.substr(0, slash);
    uint32_t address = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto dot = ip.find('.');
        const bool last = i == 3;
        uint32_t octet = 0;
        if ((dot == ip.npos) != last || !ParseSubnetNumber(ip.substr(0, dot), 255, octet)) {
            return std::nullopt;
        }
        address = (address << 8) | octet;
        if (!last) ip.remove_prefix(dot + 1);
    }

    const auto mask = uint32_t{0xffffffff} << (32 - prefix);
    return Ipv4Subnet{address & mask, mask};
}

inline std::optional<std::vector<Ipv4Subnet>> ParseSubnets(std::string_view text) {
    std::vector<Ipv4Subnet> subnets;
    text = TrimSubnetText(text);
    while (!text.empty()) {
        const auto comma = text.find(',');
        auto subnet = ParseSubnet(TrimSubnetText(text.substr(0, comma)));
        if (!subnet) return std::nullopt;
        subnets.push_back(*subnet);
        if (comma == text.npos) break;
        text.remove_prefix(comma + 1);
        if (TrimSubnetText(text).empty()) return std::nullopt;
    }
    return subnets;
}

} // namespace remote
