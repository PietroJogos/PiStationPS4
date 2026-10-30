// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/loader/elf_types.h"
#include "elf_metadata_parser.h"
#include "firmware900_binary.h"
#include "firmware900_dependency_analyzer.h"
#include "firmware900_manager.h"
#include "firmware900_service_manager.h"
#include "firmware900_service_report.h"
#include "read_only_firmware_vfs.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace PiStation {
namespace {

constexpr std::uint64_t MaxHeaders = 4096;
constexpr std::size_t MaxTotalDependencies = 8192;
constexpr std::size_t MaxTotalCandidates = 8192;

std::string Lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return value;
}

using Binary::InRange;
using Binary::ReadLe;
using Reader = ReadOnlyFirmwareVfs::File;
std::string PublicError(const std::exception& error);

void ParseSelf(Reader& reader, FileRecord& file) {
    const auto header = reader.Read(0, 32);
    if (header[6] != 1) {
        file.status = "unsupported";
        file.notes.emplace_back("SELF container endianness is unsupported.");
        return;
    }
    const auto count = ReadLe(header, 24, 2);
    const auto header_size = ReadLe(header, 12, 2);
    const auto declared_size = ReadLe(header, 16, 8);
    if (count > MaxHeaders || header_size < 32 + count * 32 || header_size > file.size ||
        declared_size != file.size || !InRange(header_size, ReadLe(header, 14, 2), file.size)) {
        throw std::runtime_error("SELF header, segment count or declared file size is invalid");
    }
    file.self = SelfMetadata{header[4], static_cast<std::uint16_t>(count), false, false};
    const auto segments = reader.Read(32, count * 32);
    for (std::size_t i = 0; i < count; ++i) {
        const auto base = i * 32;
        const auto flags = ReadLe(segments, base, 8);
        if (!InRange(ReadLe(segments, base + 8, 8), ReadLe(segments, base + 16, 8), file.size)) {
            throw std::runtime_error("SELF segment extends beyond the file");
        }
        file.self->encrypted_segments |= (flags & 2) != 0;
        file.self->compressed_segments |= (flags & 8) != 0;
    }
    file.status = "container_header_only";
    file.dependency_status = "self_payload_not_examined";
    file.notes.emplace_back("Only the public SELF container header was inspected; payload was not "
                            "decrypted, decompressed, loaded or executed.");
}

void Log(ScanReport& report, std::string category, std::string message) {
    if (category == "ERROR") {
        ++report.error_count;
    }
    const auto result = category == "ERROR" ? "failed" : "observed";
    report.diagnostics.push_back(
        {std::move(category), std::move(message), "scan_metadata", {}, result});
}

void Inspect(ScanReport& report, const ReadOnlyFirmwareVfs& vfs,
             const std::filesystem::path& relative, std::uint64_t size,
             std::size_t& total_dependencies, std::size_t& total_metadata) {
    FileRecord file;
    file.path = relative;
    const auto quoted = Firmware900Manager::JsonString(Firmware900Manager::PathText(file.path));
    const auto filename = Lower(Firmware900Manager::PathText(file.path.filename()));
    if (filename.find("sceshellui") != std::string::npos) {
        file.shell_role = "SceShellUI_name_candidate";
    } else if (filename.find("sceshellcore") != std::string::npos) {
        file.shell_role = "SceShellCore_name_candidate";
    }
    try {
        file.size = size;
        auto reader = vfs.OpenRelative(file.path);
        if (reader.Size() != file.size) {
            throw std::runtime_error("file changed before metadata scan");
        }
        const auto prefix = reader.Read(0, std::min<std::uint64_t>(file.size, 4));
        if (prefix == std::vector<unsigned char>{0x7f, 'E', 'L', 'F'}) {
            file.kind = "ELF";
            AnalyzeElfMetadata(reader, file);
        } else if (prefix == std::vector<unsigned char>{0x4f, 0x15, 0x3d, 0x1d}) {
            file.kind = "SELF";
            ParseSelf(reader, file);
        } else if (prefix == std::vector<unsigned char>{'P', 'K', 3, 4} ||
                   prefix == std::vector<unsigned char>{'P', 'K', 5, 6}) {
            file.kind = "ZIP";
            file.status = "archive_not_extracted";
        } else if (prefix == std::vector<unsigned char>{0, 'P', 'S', 'F'}) {
            file.kind = "PSF";
            file.status = "signature_only";
        } else {
            const auto extension = Lower(Firmware900Manager::PathText(file.path.extension()));
            if (extension == ".elf" || extension == ".self" || extension == ".sprx" ||
                extension == ".prx" || extension == ".bin") {
                file.status = "unrecognized_executable_extension";
                file.notes.emplace_back("The filename extension does not match ELF or SELF magic.");
            }
        }
    } catch (const std::exception& error) {
        file.status = "invalid_or_unreadable";
        file.dependency_status = "incomplete";
        file.dependencies.clear();
        file.symbols.clear();
        file.exported_libraries.clear();
        file.exported_modules.clear();
        file.notes.emplace_back(PublicError(error));
        Log(report, "ERROR", quoted + ": " + PublicError(error));
        report.diagnostics.back().related = Firmware900Manager::PathText(file.path);
        report.diagnostics.back().operation =
            file.kind == "SELF" ? "SELF_validate" : "ELF_validate";
    }
    if (file.dependencies.size() > MaxTotalDependencies - total_dependencies) {
        file.dependencies.resize(MaxTotalDependencies - total_dependencies);
        file.dependency_status = "incomplete";
        report.complete = false;
        Log(report, "ERROR", quoted + ": total dependency limit reached");
    }
    total_dependencies += file.dependencies.size();
    bool clipped{};
    const auto budget = [&](auto& values) {
        const auto count = std::min(values.size(), std::size_t{16384} - total_metadata);
        clipped |= count != values.size();
        values.resize(count);
        total_metadata += count;
    };
    budget(file.symbols);
    budget(file.exported_libraries);
    budget(file.exported_modules);
    if (file.elf) {
        budget(file.elf->segments);
        if (file.elf->relocation_types.size() > 16384 - total_metadata) {
            file.elf->relocation_types.clear();
            clipped = true;
        } else {
            total_metadata += file.elf->relocation_types.size();
        }
    }
    if (clipped) {
        report.complete = false;
        file.symbol_status = "incomplete_metadata_limit";
        Log(report, "ERROR", quoted + ": global metadata limit reached");
    }
    Log(report, "LOADER", quoted + " kind=" + file.kind + " status=" + file.status);
    if (file.kind == "ELF" || file.kind == "SELF") {
        Log(report, file.kind == "ELF" ? "ELF_LOADER" : "SELF_LOADER", quoted + " " + file.status);
    }
    Log(report, "VFS", quoted + ": read-only descriptor access; status=" + file.status);
    report.diagnostics.back().operation = "read_metadata_readonly";
    report.diagnostics.back().related = Firmware900Manager::PathText(file.path);
    report.diagnostics.back().result = file.status;
    if (!file.shell_role.empty()) {
        Log(report, "MODULES", quoted + " " + file.shell_role + "; boot not evaluated");
        Log(report, file.shell_role.starts_with("SceShellUI") ? "SCESHELLUI" : "SCESHELLCORE",
            quoted + " format=" + file.kind + " status=" + file.status + "; loading disabled");
    }
    report.files.push_back(std::move(file));
}

bool IsWithin(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto p = path.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++p) {
        if (p == path.end() || *p != *r) {
            return false;
        }
    }
    return true;
}

std::string Hex(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

void StringArray(std::ostream& out, const std::vector<std::string>& values) {
    out << '[';
    bool first = true;
    for (const auto& value : values) {
        if (!std::exchange(first, false)) {
            out << ',';
        }
        out << Firmware900Manager::JsonString(value);
    }
    out << ']';
}

std::string PublicError(const std::exception& error) {
    if (const auto* fs_error = dynamic_cast<const std::filesystem::filesystem_error*>(&error)) {
        return fs_error->code().message();
    }
    return error.what();
}

void Identity(std::ostream& out, const ModuleIdentity& identity) {
    out << "{\"name\":" << Firmware900Manager::JsonString(identity.name)
        << ",\"id\":" << identity.id << ",\"version\":" << identity.version
        << ",\"minor_version\":" << identity.minor_version << '}';
}

void Identities(std::ostream& out, const std::vector<ModuleIdentity>& identities) {
    out << '[';
    bool first = true;
    for (const auto& identity : identities) {
        if (!std::exchange(first, false)) {
            out << ',';
        }
        Identity(out, identity);
    }
    out << ']';
}

} // namespace

std::string Firmware900Manager::PathText(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}

std::string Firmware900Manager::JsonString(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c == '"' || c == '\\') {
            out << '\\' << static_cast<char>(c);
        } else if (c < 0x20 || c == 0x7f) {
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned{c};
        } else if (c < 0x80) {
            out << static_cast<char>(c);
        } else {
            const std::size_t count = c >= 0xc2 && c <= 0xdf   ? 2
                                      : c >= 0xe0 && c <= 0xef ? 3
                                      : c >= 0xf0 && c <= 0xf4 ? 4
                                                               : 0;
            bool valid = count != 0 && count <= value.size() - i;
            for (std::size_t j = 1; valid && j < count; ++j) {
                const auto next = static_cast<unsigned char>(value[i + j]);
                valid = next >= 0x80 && next <= 0xbf;
            }
            if (valid) {
                const auto next = static_cast<unsigned char>(value[i + 1]);
                valid = !(c == 0xe0 && next < 0xa0) && !(c == 0xed && next >= 0xa0) &&
                        !(c == 0xf0 && next < 0x90) && !(c == 0xf4 && next >= 0x90);
            }
            if (valid) {
                out << value.substr(i, count);
                i += count - 1;
            } else {
                out << "\\ufffd";
            }
        }
    }
    out << '"';
    return out.str();
}

ScanReport Firmware900Manager::Scan(const std::filesystem::path& directory,
                                    std::size_t max_entries) const {
    ScanReport report;
    Log(report, "PISTATION", "Firmware 9.00 Dashboard — Experimental: offline diagnostic mode");
    Log(report, "FIRMWARE900", "Firmware version, loader compatibility and boot are not validated");
    try {
        if (directory.empty() || !std::filesystem::is_directory(directory) || max_entries == 0) {
            throw std::runtime_error(
                "input must be an existing directory; entry limit must be positive");
        }
        report.root = std::filesystem::canonical(directory);
        report.input_valid = true;
        ReadOnlyFirmwareVfs vfs(report.root);
        const auto listing = vfs.Enumerate(max_entries);
        report.complete = listing.complete;
        std::size_t total_dependencies{};
        std::size_t total_metadata{};
        for (const auto& entry : listing.files) {
            Inspect(report, vfs, entry.path, entry.size, total_dependencies, total_metadata);
        }
        for (const auto& skipped : listing.skipped) {
            Log(report, "VFS", "symlink or special file skipped: " + JsonString(PathText(skipped)));
        }
        for (const auto& error : listing.errors) {
            Log(report, "ERROR", error);
        }
        if (report.files.empty()) {
            Log(report, "ERROR", "input contains no scanned regular files");
        }
    } catch (const std::exception& error) {
        report.complete = false;
        Log(report, "ERROR", PublicError(error));
    }
    std::ranges::sort(report.files, {}, [](const FileRecord& file) { return file.path; });
    std::map<std::string, std::vector<std::string>> filenames;
    for (const auto& file : report.files) {
        const auto filename = Lower(PathText(file.path.filename()));
        const auto path = PathText(file.path);
        filenames[filename].push_back(path);
        const auto extension = Lower(PathText(file.path.extension()));
        if (extension == ".sprx" || extension == ".prx") {
            filenames[Lower(PathText(file.path.stem()))].push_back(path);
        }
    }
    std::size_t total_candidates{};
    for (auto& file : report.files) {
        for (auto& dependency : file.dependencies) {
            const auto name = Lower(dependency.name);
            const auto found = filenames.find(name);
            if (found != filenames.end()) {
                const auto& candidates = found->second;
                const auto count = std::min(
                    {candidates.size(), std::size_t{32}, MaxTotalCandidates - total_candidates});
                dependency.filename_candidates.assign(
                    candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(count));
                total_candidates += count;
                if (count < candidates.size()) {
                    dependency.candidates_truncated = true;
                    report.complete = false;
                    Log(report, "ERROR",
                        "filename candidate limit reached for " + JsonString(dependency.name));
                }
            }
            Log(report, "DEPENDENCIES",
                JsonString(PathText(file.path)) + " declares " + dependency.kind + " " +
                    JsonString(dependency.name) + "; filename_candidates=" +
                    std::to_string(dependency.filename_candidates.size()) +
                    "; resolution not evaluated");
        }
    }
    Log(report, "DEPENDENCIES",
        "Symbol/NID resolution, ABI, kernel services, IPC, mounts, users, "
        "graphics and multi-process lifecycle still require investigation; no modules were loaded");
    Log(report, "PISTATION",
        "scan finished: files=" + std::to_string(report.files.size()) +
            " errors=" + std::to_string(report.error_count) +
            " complete=" + (report.complete ? "true" : "false"));
    return report;
}

void Firmware900Manager::DiagnoseServices(ScanReport& report, const HleLookup& registered_hle) {
    report.boot = {};
    std::erase_if(report.diagnostics, [](const auto& event) {
        return event.operation == "plan_boot" || event.operation == "plan_process";
    });
    Firmware900DependencyAnalyzer{}.Analyze(report, registered_hle);
    Firmware900ServiceManager{}.Analyze(report);
}

void Firmware900Manager::WriteReport(const ScanReport& report,
                                     const std::filesystem::path& output) {
    const auto resolved = std::filesystem::weakly_canonical(output);
    if (report.input_valid && IsWithin(resolved, report.root)) {
        throw std::runtime_error("report must be outside the scanned directory");
    }
    std::ofstream out(output, std::ios::out | std::ios::noreplace);
    if (!out) {
        throw std::runtime_error("cannot create report; parent must exist and output must be new");
    }
    out << std::boolalpha
        << "{\n\"schema_version\":3,\"mode\":\"Firmware 9.00 Dashboard — Experimental\","
        << "\"firmware_version_verified\":false,\"boot_attempted\":false,"
        << "\"symbol_resolution\":\"not_evaluated\",\"root\":\"<user-selected-directory>\""
        << ",\"input_valid\":" << report.input_valid << ",\"scan_complete\":" << report.complete
        << ",\"error_count\":" << report.error_count << ",\"files\":[\n";
    bool first = true;
    for (const auto& file : report.files) {
        if (!std::exchange(first, false)) {
            out << ",\n";
        }
        out << "{\"path\":" << JsonString(PathText(file.path)) << ",\"size\":" << file.size
            << ",\"extension\":" << JsonString(PathText(file.path.extension()))
            << ",\"module_role\":" << JsonString(file.module_role)
            << ",\"kind\":" << JsonString(file.kind) << ",\"status\":" << JsonString(file.status)
            << ",\"shell_role\":" << JsonString(file.shell_role)
            << ",\"dependency_status\":" << JsonString(file.dependency_status) << ",\"elf\":";
        if (file.elf) {
            out << "{\"type\":" << file.elf->type << ",\"machine\":" << file.elf->machine
                << ",\"entry\":" << JsonString(Hex(file.elf->entry))
                << ",\"program_headers\":" << file.elf->program_headers
                << ",\"entry_in_executable_segment\":" << file.elf->entry_in_executable_segment
                << ",\"tls_present\":" << file.elf->tls_present
                << ",\"symbol_table_entries\":" << file.elf->symbol_table_entries
                << ",\"relocation_status\":" << JsonString(file.elf->relocation_status)
                << ",\"relocation_types\":{";
            bool first_type = true;
            for (const auto& [type, count] : file.elf->relocation_types) {
                if (!std::exchange(first_type, false)) {
                    out << ',';
                }
                out << JsonString(std::to_string(type)) << ':' << count;
            }
            out << "},\"segments\":[";
            bool first_segment = true;
            for (const auto& segment : file.elf->segments) {
                if (!std::exchange(first_segment, false)) {
                    out << ',';
                }
                out << "{\"type\":" << segment.type << ",\"flags\":" << segment.flags
                    << ",\"offset\":" << segment.offset
                    << ",\"address\":" << JsonString(Hex(segment.address))
                    << ",\"file_size\":" << segment.file_size
                    << ",\"memory_size\":" << segment.memory_size
                    << ",\"alignment\":" << segment.alignment << '}';
            }
            out << "]}";
        } else {
            out << "null";
        }
        out << ",\"self\":";
        if (file.self) {
            out << "{\"version\":" << unsigned{file.self->version}
                << ",\"segments\":" << file.self->segments
                << ",\"encrypted_segments\":" << file.self->encrypted_segments
                << ",\"compressed_segments\":" << file.self->compressed_segments << '}';
        } else {
            out << "null";
        }
        out << ",\"dependencies\":[";
        bool first_dependency = true;
        for (const auto& dependency : file.dependencies) {
            if (!std::exchange(first_dependency, false)) {
                out << ',';
            }
            out << "{\"kind\":" << JsonString(dependency.kind)
                << ",\"name\":" << JsonString(dependency.name) << ",\"id\":" << dependency.id
                << ",\"version\":" << dependency.version
                << ",\"minor_version\":" << dependency.minor_version
                << ",\"resolution\":\"not_evaluated\","
                << "\"filename_candidates\":";
            StringArray(out, dependency.filename_candidates);
            out << ",\"filename_candidates_truncated\":" << dependency.candidates_truncated << '}';
        }
        out << "],\"module_identity\":";
        if (file.module_identity) {
            Identity(out, *file.module_identity);
        } else {
            out << "null";
        }
        out << ",\"exported_libraries\":";
        Identities(out, file.exported_libraries);
        out << ",\"exported_modules\":";
        Identities(out, file.exported_modules);
        out << ",\"symbol_status\":" << JsonString(file.symbol_status) << ",\"symbols\":[";
        bool first_symbol = true;
        for (const auto& symbol : file.symbols) {
            if (!std::exchange(first_symbol, false)) {
                out << ',';
            }
            out << "{\"name\":" << JsonString(symbol.name)
                << ",\"library\":" << JsonString(symbol.library)
                << ",\"module\":" << JsonString(symbol.module)
                << ",\"type\":" << unsigned{symbol.type}
                << ",\"library_version\":" << symbol.library_version << ",\"weak\":" << symbol.weak
                << ",\"qualified\":" << symbol.qualified << ",\"imported\":" << symbol.imported
                << '}';
        }
        out << "],\"notes\":";
        StringArray(out, file.notes);
        out << '}';
    }
    out << "\n],\"analysis\":{\"performed\":" << report.analysis.performed
        << ",\"complete\":" << report.analysis.complete
        << ",\"hle_registry_connected\":" << report.analysis.hle_registry_connected
        << ",\"hle_registry_checked\":" << report.analysis.hle_registry_checked
        << ",\"hle_query_count\":" << report.analysis.hle_query_count
        << ",\"hle_query_failures\":" << report.analysis.hle_query_failures << ",\"edges\":[";
    first = true;
    for (const auto& edge : report.analysis.edges) {
        if (!std::exchange(first, false)) {
            out << ',';
        }
        out << "{\"from\":" << JsonString(edge.from) << ",\"name\":" << JsonString(edge.name)
            << ",\"kind\":" << JsonString(edge.kind) << ",\"status\":" << JsonString(edge.status)
            << ",\"targets\":";
        StringArray(out, edge.targets);
        out << '}';
    }
    out << "],\"imports\":[";
    first = true;
    for (const auto& symbol : report.analysis.imports) {
        if (!std::exchange(first, false)) {
            out << ',';
        }
        out << "{\"from\":" << JsonString(symbol.from) << ",\"name\":" << JsonString(symbol.name)
            << ",\"library\":" << JsonString(symbol.library)
            << ",\"module\":" << JsonString(symbol.module)
            << ",\"status\":" << JsonString(symbol.status)
            << ",\"provider\":" << JsonString(symbol.provider) << '}';
    }
    out << "],\"cycles\":[";
    first = true;
    for (const auto& cycle : report.analysis.cycles) {
        if (!std::exchange(first, false)) {
            out << ',';
        }
        StringArray(out, cycle);
    }
    out << "]}";
    WriteServiceJson(out, report);
    out << ",\"diagnostics\":[";
    first = true;
    for (const auto& diagnostic : report.diagnostics) {
        if (!std::exchange(first, false)) {
            out << ',';
        }
        out << "{\"category\":" << JsonString(diagnostic.category)
            << ",\"message\":" << JsonString(diagnostic.message)
            << ",\"operation\":" << JsonString(diagnostic.operation)
            << ",\"related\":" << JsonString(diagnostic.related)
            << ",\"result\":" << JsonString(diagnostic.result) << ",\"service\":";
        if (diagnostic.service) {
            WriteServiceEventJson(out, *diagnostic.service);
        } else {
            out << "null";
        }
        out << '}';
    }
    out << "]}\n";
    out.close();
    if (!out) {
        throw std::runtime_error("report write failed; output may be incomplete");
    }
}

void Firmware900Manager::WriteTextReport(const ScanReport& report,
                                         const std::filesystem::path& output) {
    if (report.input_valid && IsWithin(std::filesystem::weakly_canonical(output), report.root)) {
        throw std::runtime_error("report must be outside the scanned directory");
    }
    std::ofstream out(output, std::ios::out | std::ios::noreplace);
    if (!out) {
        throw std::runtime_error("cannot create text report; output must be new");
    }
    out << "Firmware 9.00 Dashboard — Experimental\nOffline metadata only. Boot disabled. "
           "Firmware version and runtime symbol resolution not verified.\n";
    for (const auto& file : report.files) {
        out << "\n" << JsonString(PathText(file.path)) << " [" << file.kind << "] " << file.status;
        if (file.elf) {
            out << " entry=" << Hex(file.elf->entry) << " tls=" << file.elf->tls_present;
        }
        out << '\n';
    }
    for (const auto& edge : report.analysis.edges) {
        out << JsonString(edge.from) << " -> " << JsonString(edge.name) << ": " << edge.status
            << '\n';
    }
    for (const auto& symbol : report.analysis.imports) {
        out << JsonString(symbol.from) << " imports " << JsonString(symbol.name) << ": "
            << symbol.status << '\n';
    }
    for (const auto& cycle : report.analysis.cycles) {
        out << "Cycle: ";
        StringArray(out, cycle);
        out << '\n';
    }
    WriteServiceText(out, report);
    Emit(report, [&](const Diagnostic& diagnostic) {
        out << '[' << diagnostic.category << "] operation=" << JsonString(diagnostic.operation)
            << " related=" << JsonString(diagnostic.related)
            << " result=" << JsonString(diagnostic.result) << ' ' << JsonString(diagnostic.message);
        if (diagnostic.service) {
            out << " service=";
            WriteServiceEventJson(out, *diagnostic.service);
        }
        out << '\n';
    });
    out.close();
    if (!out) {
        throw std::runtime_error("text report write failed");
    }
}

void Firmware900Manager::Emit(const ScanReport& report, const DiagnosticSink& sink) {
    if (sink) {
        for (const auto& diagnostic : report.diagnostics) {
            sink(diagnostic);
        }
    }
}

} // namespace PiStation
