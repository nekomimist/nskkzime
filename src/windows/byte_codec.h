#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nskkzime::windows {
// Broker message fields: little-endian u32 numbers and u32-length-prefixed bytes.
struct ByteWriter {
    std::string bytes;
    explicit ByteWriter(std::string_view magic) : bytes(magic) {}
    void number(std::uint32_t value) { for (int i = 0; i < 4; ++i) { bytes += static_cast<char>(value & 255); value >>= 8; } }
    void text(std::string_view value) {
        if (value.size() > UINT32_MAX) throw std::length_error("broker message field too large");
        number(static_cast<std::uint32_t>(value.size())); bytes += value;
    }
};
struct ByteReader {
    std::string_view bytes;
    ByteReader(std::string_view message, std::string_view magic) : bytes(message) {
        need(bytes.starts_with(magic)); bytes.remove_prefix(magic.size());
    }
    static void need(bool ok) { if (!ok) throw std::runtime_error("invalid broker message"); }
    std::uint32_t number() {
        need(bytes.size() >= 4); std::uint32_t value = 0;
        for (int i = 3; i >= 0; --i) value = (value << 8) | static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)]);
        bytes.remove_prefix(4); return value;
    }
    std::string_view text(std::size_t limit) {
        const auto size = number(); need(size <= limit && size <= bytes.size());
        const auto value = bytes.substr(0, size); bytes.remove_prefix(size); return value;
    }
    bool flag() { const auto value = number(); need(value <= 1); return value == 1; }
    void end() const { need(bytes.empty()); }
};
} // namespace nskkzime::windows
