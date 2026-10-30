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
#include "firmware900_parser_budget.h"

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
    std::uint64_t symbol_entries_examined{};
    std::uint64_t symbol_records_omitted{};
    std::uint64_t relocation_table_entries{};
    std::uint64_t relocation_entries_examined{};
    bool segments_validated{};
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

struct ServiceEvent {
    std::string requester;
    std::string library;
    std::string module;
    std::string function;
    std::string nid;
    std::string resolution;
    std::optional<std::int64_t> return_code;
    std::string reason;
};

struct Diagnostic {
    std::string category;
    std::string message;
    std::string operation;
    std::string related;
    std::string result;
    std::optional<ServiceEvent> service{};
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
    bool performed{};
    bool complete = true;
    bool hle_registry_connected{};
    bool hle_registry_checked{};
    std::size_t hle_query_count{};
    std::size_t hle_query_failures{};
    std::vector<DependencyEdge> edges;
    std::vector<SymbolResolution> imports;
    std::vector<std::vector<std::string>> cycles;
};

struct ServiceRequirement {
    std::string requester;
    std::string kind;
    std::string name;
    std::string library;
    std::string module;
    std::string nid_validation;
    std::string resolution;
    std::string classification = "unknown_or_unverified";
    std::string reason;
    std::string provider;
    std::vector<std::string> source_evidence;
    std::vector<std::string> function_names;
};

struct ServiceAnalysis {
    bool performed{};
    bool complete{};
    bool live_hle_registry_checked{};
    std::string source_catalog_digest;
    std::size_t source_catalog_entries{};
    std::size_t source_catalog_skipped{};
    std::vector<ServiceRequirement> requirements;
    std::map<std::string, std::size_t> classifications;
};

struct BootImagePlan {
    std::string path;
    std::string role;
    bool process_candidate{};
    std::uint64_t entry{};
    std::size_t load_segments{};
    std::uint64_t segment_bytes{};
    std::uint64_t minimum_page_bytes{};
    std::string memory_estimate_status = "not_inspected";
    std::uint64_t tls_bytes{};
    std::string process_parameters = "not_inspected";
    std::vector<std::string> blockers;
};

struct BootDiagnostic {
    bool performed{};
    std::string state = "not_requested";
    std::vector<BootImagePlan> images;
    std::vector<std::string> blockers;
    std::vector<std::string> stages;
};

struct ScanReport {
    std::filesystem::path root;
    bool input_valid{};
    bool complete = true;
    std::vector<FileRecord> files;
    std::vector<Diagnostic> diagnostics;
    std::size_t error_count{};
    std::vector<std::string> vfs_accesses;
    ParserBudget parser_budget_initial;
    ParserBudget parser_budget_remaining;
    DependencyAnalysis analysis;
    ServiceAnalysis services;
    BootDiagnostic boot;
};

using HleLookup = std::function<bool(const SymbolMetadata&)>;

class Firmware900Manager {
public:
    ScanReport Scan(const std::filesystem::path& directory, std::size_t max_entries = 10000,
                    ParserBudget parser_budget = {}) const;
    static void DiagnoseServices(ScanReport& report, const HleLookup& registered_hle = {});
    static void WriteReport(const ScanReport& report, const std::filesystem::path& output);
    static void WriteTextReport(const ScanReport& report, const std::filesystem::path& output);
    static void Emit(const ScanReport& report, const DiagnosticSink& sink);
    static std::string JsonString(const std::string& value);
    static std::string PathText(const std::filesystem::path& path);
};

} // namespace PiStation
