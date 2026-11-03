// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <windows.h>
#include "read_only_firmware_vfs.h"

namespace {
void Check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
} // namespace
int main() {
    wchar_t temporary[MAX_PATH + 1]{};
    wchar_t unique[MAX_PATH + 1]{};
    Check(GetTempPathW(MAX_PATH, temporary) != 0, "temporary path unavailable");
    Check(GetTempFileNameW(temporary, L"psv", 0, unique) != 0, "temporary name unavailable");
    Check(DeleteFileW(unique) != 0 && CreateDirectoryW(unique, nullptr) != 0,
          "temporary directory failed");
    Cleanup cleanup{unique};
    const auto folder = cleanup.path / L"folder";
    std::filesystem::create_directory(folder);
    const auto file = folder / L"sample";
    {
        std::ofstream out(file, std::ios::binary);
        out << "0123456789";
    }
    DWORD baseline{};
    Check(GetProcessHandleCount(GetCurrentProcess(), &baseline) != 0, "handle count failed");
    for (unsigned i = 0; i < 32; ++i) {
        PiStation::ReadOnlyFirmwareVfs vfs(cleanup.path);
        auto opened = vfs.OpenRelative(L"folder/sample");
        auto moved = std::move(opened);
        Check(moved.Read(3, 4) == std::vector<unsigned char>({'3', '4', '5', '6'}),
              "offset read failed");
        Check(!MoveFileW(folder.c_str(), (cleanup.path / L"renamed").c_str()),
              "pinned ancestor was renamed");
        const auto writer = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, 0, nullptr);
        if (writer != INVALID_HANDLE_VALUE) {
            CloseHandle(writer);
            throw std::runtime_error("write sharing allowed");
        }
        bool denied{};
        try {
            moved.Read(9, 2);
        } catch (const std::exception&) {
            denied = true;
        }
        Check(denied, "out-of-bounds read accepted");
        const auto listing = vfs.Enumerate(1);
        Check(!listing.complete, "enumeration limit ignored");
    }
    DWORD final_count{};
    Check(GetProcessHandleCount(GetCurrentProcess(), &final_count) != 0, "handle count failed");
    Check(final_count <= baseline + 2, "handle count grew after repeated destruction");
    Check(MoveFileW(folder.c_str(), (cleanup.path / L"renamed").c_str()) != 0,
          "directory remained pinned after VFS destruction");
}
