// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace PiStation {

class ReadOnlyFirmwareVfs {
public:
#ifdef _WIN32
    using NativeHandle = std::intptr_t;
#else
    using NativeHandle = int;
#endif
    class File {
    public:
        File(NativeHandle descriptor, std::uint64_t size);
        ~File();
        File(const File&) = delete;
        File& operator=(const File&) = delete;
        File(File&& other) noexcept;
        File& operator=(File&& other) noexcept;
        std::uint64_t Size() const;
        std::vector<unsigned char> Read(std::uint64_t offset, std::uint64_t count) const;

    private:
        friend class ReadOnlyFirmwareVfs;
        NativeHandle descriptor = -1;
#ifdef _WIN32
        std::shared_ptr<File> parent;
#endif
        std::uint64_t size{};
    };

    explicit ReadOnlyFirmwareVfs(const std::filesystem::path& allowed_root);
    struct Entry {
        std::filesystem::path path;
        std::uint64_t size{};
    };
    struct Listing {
        std::vector<Entry> files;
        std::vector<std::filesystem::path> skipped;
        std::vector<std::string> errors;
        bool complete = true;
    };
    Listing Enumerate(std::size_t max_entries) const;
    void Mount(std::string virtual_root, const std::filesystem::path& relative_host);
    File OpenRelative(const std::filesystem::path& relative_path) const;
    File OpenVirtual(std::string_view path) const;

private:
#ifdef _WIN32
    File OpenWindowsPath(const std::filesystem::path& path, bool directory) const;
#endif
    File root;
    struct MountPoint {
        std::string virtual_root;
        std::filesystem::path relative_host;
    };
    std::vector<MountPoint> mounts;
};

} // namespace PiStation
