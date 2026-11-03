// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string_view>
namespace PiStation::VfsPaths {
inline void ValidateRelative(const std::filesystem::path& path) {
    if (path.is_absolute() || path.has_root_name() || path.native().size() > 4096) {
        throw std::runtime_error("VFS requires a relative host path");
    }
    std::size_t depth{};
    for (const auto& component : path) {
        if (++depth > 64) {
            throw std::runtime_error("VFS path depth exceeds diagnostic limit");
        }
#ifdef _WIN32
        const auto name = component.native();
        if (name != L"." && !name.empty() &&
            (name.find_first_of(L":<>\"|?*") != std::wstring::npos || name.back() == L'.' ||
             name.back() == L' ' || std::ranges::any_of(name, [](wchar_t c) { return c < 32; }))) {
            throw std::runtime_error("VFS rejects Windows namespace or malformed components");
        }
#endif
        if (component == ".." || component.native().find(std::filesystem::path::value_type{}) !=
                                     std::filesystem::path::string_type::npos) {
            throw std::runtime_error("VFS path traversal or NUL rejected");
        }
    }
}

inline std::string ValidateVirtual(std::string_view path) {
    if (path.empty() || path.size() > 4096 || path.front() != '/' || path.starts_with("//") ||
        path.find('\\') != std::string_view::npos || path.find('\0') != std::string_view::npos) {
        throw std::runtime_error("VFS requires an absolute guest path without backslashes or NUL");
    }
    const std::filesystem::path value{std::u8string(path.begin(), path.end())};
    ValidateRelative(value.relative_path());
    const auto utf8 = value.lexically_normal().generic_u8string();
    std::string normalized(utf8.begin(), utf8.end());
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

} // namespace PiStation::VfsPaths
