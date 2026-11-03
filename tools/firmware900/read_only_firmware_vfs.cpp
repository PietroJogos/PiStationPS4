// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmware_vfs_paths.h"
#include "read_only_firmware_vfs.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace PiStation {
using namespace VfsPaths;
namespace {

void Close(int descriptor) {
#if defined(__unix__) || defined(__APPLE__)
    if (descriptor >= 0) {
        ::close(descriptor);
    }
#else
    (void)descriptor;
#endif
}

} // namespace

ReadOnlyFirmwareVfs::File::File(int descriptor, std::uint64_t size)
    : descriptor{descriptor}, size{size} {}

ReadOnlyFirmwareVfs::File::~File() {
    Close(descriptor);
}

ReadOnlyFirmwareVfs::File::File(File&& other) noexcept
    : descriptor{std::exchange(other.descriptor, -1)}, size{other.size} {}

ReadOnlyFirmwareVfs::File& ReadOnlyFirmwareVfs::File::operator=(File&& other) noexcept {
    if (this != &other) {
        Close(descriptor);
        descriptor = std::exchange(other.descriptor, -1);
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
#if defined(__unix__) || defined(__APPLE__)
    std::size_t total{};
    while (total < bytes.size()) {
        const auto read = ::pread(descriptor, bytes.data() + total, bytes.size() - total,
                                  static_cast<off_t>(offset + total));
        if (read < 0 && errno == EINTR) {
            continue;
        }
        if (read <= 0) {
            throw std::runtime_error("VFS metadata read failed; file may have changed");
        }
        total += static_cast<std::size_t>(read);
    }
#else
    throw std::runtime_error("secure VFS reads require POSIX descriptor support");
#endif
    return bytes;
}

ReadOnlyFirmwareVfs::ReadOnlyFirmwareVfs(const std::filesystem::path& allowed_root) : root{-1, 0} {
#if defined(__unix__) || defined(__APPLE__)
    const int fd = ::open(allowed_root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error("VFS cannot open permitted root directory");
    }
    root = File{fd, 0};
#else
    (void)allowed_root;
    throw std::runtime_error("secure VFS mounts require POSIX descriptor support");
#endif
    mounts.push_back({"/", {}});
}

ReadOnlyFirmwareVfs::Listing ReadOnlyFirmwareVfs::Enumerate(std::size_t max_entries) const {
    Listing listing;
#if defined(__unix__) || defined(__APPLE__)
    struct DirectoryCloser {
        void operator()(DIR* directory) const {
            ::closedir(directory);
        }
    };
    using Directory = std::unique_ptr<DIR, DirectoryCloser>;
    struct Frame {
        Directory directory;
        std::filesystem::path relative;
    };
    const auto open_directory = [](int descriptor) {
        if (descriptor < 0) {
            throw std::runtime_error("VFS enumeration directory open failed");
        }
        DIR* directory = ::fdopendir(descriptor);
        if (!directory) {
            Close(descriptor);
            throw std::runtime_error("VFS directory enumeration failed");
        }
        return Directory{directory};
    };
    std::vector<Frame> stack;
    stack.push_back(
        {open_directory(::openat(root.descriptor, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)), {}});
    std::size_t visited{};
    while (!stack.empty()) {
        auto& frame = stack.back();
        errno = 0;
        const auto* entry = ::readdir(frame.directory.get());
        if (!entry) {
            if (errno) {
                listing.complete = false;
                listing.errors.emplace_back("VFS directory read failed");
            }
            stack.pop_back();
            continue;
        }
        const std::string name{entry->d_name};
        if (name == "." || name == "..") {
            continue;
        }
        if (visited++ >= max_entries) {
            listing.complete = false;
            listing.errors.emplace_back("entry limit reached; scan is incomplete");
            break;
        }
        const auto path = frame.relative / name;
        if (path.native().size() > 4096 || stack.size() > 64) {
            listing.complete = false;
            listing.errors.emplace_back("VFS path length/depth limit reached");
            continue;
        }
        struct stat info{};
        const int parent = ::dirfd(frame.directory.get());
        if (::fstatat(parent, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
            listing.complete = false;
            listing.errors.emplace_back("VFS entry metadata unavailable");
            continue;
        }
        if (S_ISDIR(info.st_mode)) {
            try {
                auto child = open_directory(
                    ::openat(parent, name.c_str(),
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
                stack.push_back({std::move(child), path});
            } catch (const std::exception&) {
                listing.complete = false;
                listing.errors.emplace_back("VFS child directory access denied");
            }
        } else if (S_ISREG(info.st_mode) && info.st_size >= 0) {
            listing.files.push_back({path, static_cast<std::uint64_t>(info.st_size)});
        } else {
            listing.skipped.push_back(path);
        }
    }
#else
    (void)max_entries;
    throw std::runtime_error("secure VFS enumeration requires POSIX descriptor support");
#endif
    return listing;
}

ReadOnlyFirmwareVfs::File ReadOnlyFirmwareVfs::OpenRelative(
    const std::filesystem::path& relative_path) const {
    ValidateRelative(relative_path);
#if defined(__unix__) || defined(__APPLE__)
    const int copy = ::fcntl(root.descriptor, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) {
        throw std::runtime_error("VFS root descriptor duplication failed");
    }
    File current{copy, 0};
    std::vector<std::filesystem::path> components;
    for (const auto& part : relative_path) {
        if (part != "." && !part.empty()) {
            components.push_back(part);
        }
    }
    for (std::size_t i = 0; i < components.size(); ++i) {
        const bool last = i + 1 == components.size();
        const auto flags =
            O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (last ? 0 : O_DIRECTORY);
        const int fd = ::openat(current.descriptor, components[i].c_str(), flags);
        if (fd < 0) {
            throw std::runtime_error("VFS open denied: missing, inaccessible or symlink component");
        }
        current = File{fd, 0};
    }
    struct stat info{};
    if (::fstat(current.descriptor, &info) != 0 || info.st_size < 0 ||
        (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))) {
        throw std::runtime_error("VFS rejects special or inaccessible files");
    }
    if (S_ISDIR(info.st_mode)) {
        throw std::runtime_error("VFS file open requires a regular file");
    }
    current.size = static_cast<std::uint64_t>(info.st_size);
    return current;
#else
    throw std::runtime_error("secure VFS reads require POSIX descriptor support");
#endif
}

void ReadOnlyFirmwareVfs::Mount(std::string virtual_root,
                                const std::filesystem::path& relative_host) {
    virtual_root = ValidateVirtual(virtual_root);
    ValidateRelative(relative_host);
#if defined(__unix__) || defined(__APPLE__)
    File current{::fcntl(root.descriptor, F_DUPFD_CLOEXEC, 0), 0};
    if (current.descriptor < 0) {
        throw std::runtime_error("VFS root descriptor duplication failed");
    }
    for (const auto& part : relative_host) {
        if (part == "." || part.empty()) {
            continue;
        }
        const int fd = ::openat(current.descriptor, part.c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            throw std::runtime_error("VFS mount requires an existing non-symlink directory");
        }
        current = File{fd, 0};
    }
#endif
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
    return OpenRelative(selected->relative_host / std::filesystem::path{suffix});
}

} // namespace PiStation
