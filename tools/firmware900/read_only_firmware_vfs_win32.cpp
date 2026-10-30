// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmware_vfs_paths.h"
#include "read_only_firmware_vfs.h"

#define NOMINMAX
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <windows.h>
#include <winternl.h>

namespace PiStation {
using namespace VfsPaths;
namespace {
HANDLE Handle(ReadOnlyFirmwareVfs::NativeHandle value) {
    return reinterpret_cast<HANDLE>(value);
}
void Close(ReadOnlyFirmwareVfs::NativeHandle value) {
    if (value != -1) {
        CloseHandle(Handle(value));
    }
}
[[noreturn]] void Fail(const char* operation) {
    throw std::runtime_error(std::string("VFS Win32 ") + operation + " failed (error " +
                             std::to_string(GetLastError()) + ")");
}
FILE_STANDARD_INFO Inspect(HANDLE handle, bool directory) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_STANDARD_INFO info{};
    if (GetFileType(handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                      sizeof(attributes)) ||
        !GetFileInformationByHandleEx(handle, FileStandardInfo, &info, sizeof(info))) {
        Fail("handle inspection");
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        static_cast<bool>(info.Directory) != directory || info.DeletePending ||
        info.EndOfFile.QuadPart < 0) {
        throw std::runtime_error("VFS rejects reparse points, wrong entry types or deleting files");
    }
    return info;
}
HANDLE OpenChild(HANDLE parent, const std::wstring& component, bool directory) {
    using NtOpen = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    static const auto open =
        reinterpret_cast<NtOpen>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
    if (!open || component.size() > 32766) {
        throw std::runtime_error("VFS handle-relative NT file opening unavailable");
    }
    UNICODE_STRING name{};
    name.Buffer = const_cast<wchar_t*>(component.data());
    name.Length = static_cast<USHORT>(component.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &name;
    attributes.Attributes = 0x40 | 0x1000; // OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE
    IO_STATUS_BLOCK status{};
    HANDLE child = INVALID_HANDLE_VALUE;
    const auto result =
        open(&child,
             FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE | (directory ? FILE_TRAVERSE : 0),
             &attributes, &status, nullptr, 0, FILE_SHARE_READ,
             1, // FILE_OPEN: never create
             0x00200000 | 0x20 | (directory ? 1 : 0x40), nullptr,
             0); // OPEN_REPARSE_POINT | SYNCHRONOUS_IO_NONALERT | type
    if (result < 0) {
        throw std::runtime_error("VFS handle-relative open denied (NTSTATUS " +
                                 std::to_string(static_cast<ULONG>(result)) + ")");
    }
    return child;
}
} // namespace

ReadOnlyFirmwareVfs::File::File(NativeHandle descriptor, std::uint64_t size)
    : descriptor{descriptor}, size{size} {}
ReadOnlyFirmwareVfs::File::~File() {
    Close(descriptor);
}
ReadOnlyFirmwareVfs::File::File(File&& other) noexcept
    : descriptor{std::exchange(other.descriptor, -1)}, parent{std::move(other.parent)},
      size{other.size} {}
ReadOnlyFirmwareVfs::File& ReadOnlyFirmwareVfs::File::operator=(File&& other) noexcept {
    if (this != &other) {
        Close(descriptor);
        descriptor = std::exchange(other.descriptor, -1);
        parent = std::move(other.parent);
        size = other.size;
    }
    return *this;
}
std::uint64_t ReadOnlyFirmwareVfs::File::Size() const {
    return size;
}
std::vector<unsigned char> ReadOnlyFirmwareVfs::File::Read(std::uint64_t offset,
                                                           std::uint64_t count) const {
    if (count > 4 * 1024 * 1024 || offset > size || count > size - offset ||
        offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - count) {
        throw std::runtime_error("VFS metadata range invalid or diagnostic limit exceeded");
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(count));
    std::size_t total{};
    while (total < bytes.size()) {
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset + total);
        position.OffsetHigh = static_cast<DWORD>((offset + total) >> 32);
        DWORD read{};
        if (!ReadFile(Handle(descriptor), bytes.data() + total,
                      static_cast<DWORD>(bytes.size() - total), &read, &position) ||
            !read) {
            Fail("metadata read");
        }
        total += read;
    }
    return bytes;
}
ReadOnlyFirmwareVfs::ReadOnlyFirmwareVfs(const std::filesystem::path& allowed_root) : root{-1, 0} {
    const auto absolute = std::filesystem::absolute(allowed_root).lexically_normal();
    const auto drive = absolute.root_name().native();
    if (drive.size() != 2 || drive[1] != L':' ||
        GetDriveTypeW((drive + L"\\").c_str()) != DRIVE_FIXED) {
        throw std::runtime_error(
            "VFS requires a local fixed-drive root; network/device roots rejected");
    }
    const auto path = L"\\\\?\\" + absolute.native();
    root = File{
        reinterpret_cast<NativeHandle>(CreateFileW(
            path.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr)),
        0};
    if (root.descriptor == -1) {
        Fail("root open");
    }
    Inspect(Handle(root.descriptor), true);
    mounts.push_back({"/", {}});
}
ReadOnlyFirmwareVfs::File ReadOnlyFirmwareVfs::OpenWindowsPath(const std::filesystem::path& path,
                                                               bool directory) const {
    ValidateRelative(path);
    HANDLE duplicate{};
    if (!DuplicateHandle(GetCurrentProcess(), Handle(root.descriptor), GetCurrentProcess(),
                         &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        Fail("root duplication");
    }
    File current{reinterpret_cast<NativeHandle>(duplicate), 0};
    std::vector<std::wstring> components;
    for (const auto& part : path) {
        if (part != L"." && !part.empty()) {
            components.push_back(part.native());
        }
    }
    if (components.empty() && !directory) {
        throw std::runtime_error("VFS file open requires a regular file");
    }
    for (std::size_t i = 0; i < components.size(); ++i) {
        const bool is_directory = i + 1 < components.size() || directory;
        File next{reinterpret_cast<NativeHandle>(
                      OpenChild(Handle(current.descriptor), components[i], is_directory)),
                  0};
        const auto info = Inspect(Handle(next.descriptor), is_directory);
        next.size = static_cast<std::uint64_t>(info.EndOfFile.QuadPart);
        next.parent = std::make_shared<File>(std::move(current));
        current = std::move(next);
    }
    return current;
}
ReadOnlyFirmwareVfs::File ReadOnlyFirmwareVfs::OpenRelative(
    const std::filesystem::path& path) const {
    return OpenWindowsPath(path, false);
}
ReadOnlyFirmwareVfs::Listing ReadOnlyFirmwareVfs::Enumerate(std::size_t max_entries) const {
    Listing listing;
    std::size_t visited{};
    bool limit{};
    std::function<void(const std::filesystem::path&, std::size_t)> visit;
    visit = [&](const auto& relative, std::size_t depth) {
        auto directory = OpenWindowsPath(relative, true);
        struct DirectoryBuffer {
            alignas(FILE_ID_BOTH_DIR_INFO) std::array<unsigned char, 65536> bytes{};
        };
        auto storage = std::make_unique<DirectoryBuffer>();
        auto& buffer = storage->bytes;
        bool first = true;
        while (!limit) {
            if (!GetFileInformationByHandleEx(Handle(directory.descriptor),
                                              first ? FileIdBothDirectoryRestartInfo
                                                    : FileIdBothDirectoryInfo,
                                              buffer.data(), static_cast<DWORD>(buffer.size()))) {
                if (GetLastError() == ERROR_NO_MORE_FILES) {
                    return;
                }
                Fail("directory enumeration");
            }
            first = false;
            std::size_t offset{};
            while (!limit) {
                constexpr auto header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
                if (offset > buffer.size() - header) {
                    throw std::runtime_error("VFS malformed directory entry");
                }
                const auto* entry =
                    reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
                if (entry->FileNameLength % sizeof(wchar_t) ||
                    entry->FileNameLength > buffer.size() - offset - header) {
                    throw std::runtime_error("VFS malformed directory name");
                }
                const std::wstring name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
                if (name != L"." && name != L"..") {
                    if (visited++ >= max_entries) {
                        limit = true;
                        listing.complete = false;
                        listing.errors.emplace_back("entry limit reached; scan is incomplete");
                        return;
                    }
                    const auto path = relative / name;
                    try {
                        ValidateRelative(path);
                        if (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                            listing.skipped.push_back(path);
                        } else if (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                            if (depth >= 64) {
                                throw std::runtime_error("VFS path depth limit reached");
                            }
                            visit(path, depth + 1);
                        } else {
                            auto file = OpenRelative(path);
                            listing.files.push_back({path, file.Size()});
                        }
                    } catch (const std::exception& error) {
                        listing.complete = false;
                        listing.errors.push_back(error.what());
                    }
                }
                if (!entry->NextEntryOffset) {
                    break;
                }
                if (entry->NextEntryOffset < header ||
                    entry->NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO) ||
                    entry->NextEntryOffset > buffer.size() - offset) {
                    throw std::runtime_error("VFS malformed directory offset");
                }
                offset += entry->NextEntryOffset;
            }
        }
    };
    visit({}, 0);
    return listing;
}
void ReadOnlyFirmwareVfs::Mount(std::string virtual_root,
                                const std::filesystem::path& relative_host) {
    virtual_root = ValidateVirtual(virtual_root);
    auto directory = OpenWindowsPath(relative_host, true);
    if (std::ranges::any_of(
            mounts, [&](const auto& mount) { return mount.virtual_root == virtual_root; })) {
        throw std::runtime_error("VFS duplicate virtual mount rejected");
    }
    mounts.push_back({std::move(virtual_root), relative_host});
}
ReadOnlyFirmwareVfs::File ReadOnlyFirmwareVfs::OpenVirtual(std::string_view path) const {
    const auto normalized = ValidateVirtual(path);
    const MountPoint* selected = nullptr;
    for (const auto& mount : mounts) {
        if ((mount.virtual_root == "/" || normalized == mount.virtual_root ||
             normalized.starts_with(mount.virtual_root + "/")) &&
            (!selected || mount.virtual_root.size() > selected->virtual_root.size())) {
            selected = &mount;
        }
    }
    if (!selected) {
        throw std::runtime_error("VFS guest path has no permitted mount");
    }
    const auto skip = selected->virtual_root == "/" ? 1 : selected->virtual_root.size() + 1;
    const auto suffix = normalized.size() > skip ? normalized.substr(skip) : std::string{};
    const auto utf8 = std::u8string(suffix.begin(), suffix.end());
    return OpenRelative(selected->relative_host / std::filesystem::path{utf8});
}
} // namespace PiStation
