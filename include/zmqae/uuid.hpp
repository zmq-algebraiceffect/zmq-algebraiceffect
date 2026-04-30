#pragma once

#include <cstdio>
#include <random>
#include <string>

namespace zmqae {

// UUID v4 (RFC 4122) — lowercase hex, thread_local RNG.
// Format: xxxxxxxx-xxxx-4xxx-[89ab]xx-xxxxxxxxxxxx
inline std::string generate_uuid() {
    static thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist32{0, 0xFFFFFFFF};
    std::uniform_int_distribution<uint16_t> dist16{0, 0xFFFF};

    uint32_t time_low = dist32(gen);
    uint16_t time_mid = dist16(gen);
    uint16_t time_hi_and_ver = static_cast<uint16_t>((dist16(gen) & 0x0FFF) | 0x4000); // version 4
    uint16_t clock_seq = static_cast<uint16_t>((dist16(gen) & 0x3FFF) | 0x8000);       // variant 10
    uint32_t node_hi = dist32(gen);
    uint16_t node_lo = dist16(gen);

    char buf[37];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x",
                  time_low, time_mid, time_hi_and_ver, clock_seq, node_lo, node_hi);
    return std::string{buf};
}

} // namespace zmqae
