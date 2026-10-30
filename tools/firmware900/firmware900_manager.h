// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace PiStation {

struct Dependency {
    std::string kind;
    std::string name;
    std::vector<std::string> filename_candidates;
    bool candidates_truncated{};
};

struct ElfMetadata {
    std::uint16_t type{};
    std::uint16_t machine{};
    std::uint64_t entry{};
    std::uint16_t program_headers{};
};

struct SelfMetadata {
    std::uint8_t version{};
    std::uint16_t segments{};
    bool encrypted_segments{};
    bool compressed_segments{};
};

struct FileRecord {
    std::filesystem::path path;
    std::uint64_t size{};
    std::string kind = "unknown";
    std::string status = "unrecognized";
    std::string shell_role;
    std::string dependency_status = "not_examined";
    std::optional<ElfMetadata> elf;
    std::optional<SelfMetadata> self;
    std::vector<Dependency> dependencies;
    std::vector<std::string> notes;
};

struct Diagnostic {
    std::string category;
    std::string message;
};

struct ScanReport {
    std::filesystem::path root;
    bool input_valid{};
    bool complete = true;
    std::vector<FileRecord> files;
    std::vector<Diagnostic> diagnostics;
    std::size_t error_count{};
};

class Firmware900Manager {
public:
    ScanReport Scan(const std::filesystem::path& directory, std::size_t max_entries = 10000) const;
    static void WriteReport(const ScanReport& report, const std::filesystem::path& output);
    static std::string JsonString(const std::string& value);
    static std::string PathText(const std::filesystem::path& path);
};

} // namespace PiStation
