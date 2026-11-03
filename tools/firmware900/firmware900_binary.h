// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace PiStation::Binary {

inline bool InRange(std::uint64_t offset, std::uint64_t count, std::uint64_t size) {
    return offset <= size && count <= size - offset;
}

inline std::uint64_t ReadLe(std::span<const unsigned char> data, std::size_t offset,
                            std::size_t width) {
    if (width > 8 || !InRange(offset, width, data.size())) {
        throw std::runtime_error("truncated metadata field");
    }
    std::uint64_t value{};
    for (std::size_t i = 0; i < width; ++i) {
        value |= std::uint64_t{data[offset + i]} << (8 * i);
    }
    return value;
}

inline std::string ReadString(std::span<const unsigned char> data, std::uint64_t offset) {
    if (offset >= data.size()) {
        throw std::runtime_error("string offset outside metadata table");
    }
    const auto begin = data.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto limit =
        begin + static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(1025, data.size() - offset));
    const auto end = std::find(begin, limit, 0);
    if (end == limit) {
        throw std::runtime_error("metadata string is unterminated or exceeds 1024 bytes");
    }
    return {begin, end};
}

} // namespace PiStation::Binary
