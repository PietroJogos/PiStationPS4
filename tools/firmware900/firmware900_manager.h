// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace PiStation {

struct Dependency {
    std::string kind;
    std::string name;
    std::vector<std::string> filename_candidates;
    bool candidates_truncated{};
    std::uint16_t id{};
    std::uint16_t version{};
    std::uint16_t minor_version{};
};

struct ModuleIdentity {
    std::string name;
    std::uint16_t id{};
    std::uint16_t version{};
    std::uint16_t minor_version{};
};

struct SymbolMetadata {
    std::string name;
    std::string library;
    std::string module;
    std::uint16_t library_version{};
    std::uint8_t type{};
    bool weak{};
    bool qualified{};
    bool imported{};
};

struct SegmentMetadata {
    std::uint64_t type{};
    std::uint64_t flags{};
    std::uint64_t offset{};
    std::uint64_t address{};
    std::uint64_t file_size{};
    std::uint64_t memory_size{};
    std::uint64_t alignment{};
};

struct ElfMetadata {
    std::uint16_t type{};
    std::uint16_t machine{};
    std::uint64_t entry{};
    std::uint16_t program_headers{};
    bool entry_in_executable_segment{};
    std::vector<SegmentMetadata> segments;
    std::map<std::uint32_t, std::size_t> relocation_types;
    std::string relocation_status = "not_present";
    bool tls_present{};
    std::uint64_t symbol_table_entries{};
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
    std::optional<ModuleIdentity> module_identity;
    std::vector<ModuleIdentity> exported_libraries;
    std::vector<ModuleIdentity> exported_modules;
    std::vector<SymbolMetadata> symbols;
    std::string symbol_status = "not_examined";
    std::string module_role = "unclassified";
    std::vector<std::string> notes;
};

struct Diagnostic {
    std::string category;
    std::string message;
    std::string operation;
    std::string related;
    std::string result;
};

using DiagnosticSink = std::function<void(const Diagnostic&)>;

struct DependencyEdge {
    std::string from;
    std::string name;
    std::string kind;
    std::string status;
    std::vector<std::string> targets;
};

struct SymbolResolution {
    std::string from;
    std::string name;
    std::string library;
    std::string module;
    std::string status;
    std::string provider;
};

struct DependencyAnalysis {
    bool complete = true;
    bool hle_registry_checked{};
    std::vector<DependencyEdge> edges;
    std::vector<SymbolResolution> imports;
    std::vector<std::vector<std::string>> cycles;
};

struct ScanReport {
    std::filesystem::path root;
    bool input_valid{};
    bool complete = true;
    std::vector<FileRecord> files;
    std::vector<Diagnostic> diagnostics;
    std::size_t error_count{};
    std::vector<std::string> vfs_accesses;
    DependencyAnalysis analysis;
};

class Firmware900Manager {
public:
    ScanReport Scan(const std::filesystem::path& directory, std::size_t max_entries = 10000) const;
    static void WriteReport(const ScanReport& report, const std::filesystem::path& output);
    static void WriteTextReport(const ScanReport& report, const std::filesystem::path& output);
    static void Emit(const ScanReport& report, const DiagnosticSink& sink);
    static std::string JsonString(const std::string& value);
    static std::string PathText(const std::filesystem::path& path);
};

} // namespace PiStation
