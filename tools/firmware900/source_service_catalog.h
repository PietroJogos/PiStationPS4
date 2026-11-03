// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace PiStation {

struct SourceServiceRecord {
    std::string_view nid;
    std::string_view library;
    unsigned version;
    std::string_view module;
    std::string_view function;
    unsigned type;
    std::string_view source;
    std::string_view evidence;
};

std::span<const SourceServiceRecord> SourceServiceCatalog();
std::string_view SourceServiceCatalogDigest();
std::size_t SourceServiceCatalogSkipped();
bool ValidNidSyntax(std::string_view nid);

} // namespace PiStation
