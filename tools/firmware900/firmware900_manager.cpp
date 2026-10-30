// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmware900_manager.h"

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

constexpr std::uint64_t MaxMetadata = 4 * 1024 * 1024;
constexpr std::uint64_t MaxHeaders = 4096;
constexpr std::uint64_t MaxDependencies = 4096;
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

bool InRange(std::uint64_t offset, std::uint64_t count, std::uint64_t size) {
    return offset <= size && count <= size - offset;
}

std::uint64_t ReadLe(const std::vector<unsigned char>& data, std::size_t offset,
                     std::size_t width) {
    if (!InRange(offset, width, data.size()) || width > 8) {
        throw std::runtime_error("truncated metadata field");
    }
    std::uint64_t value{};
    for (std::size_t i = 0; i < width; ++i) {
        value |= std::uint64_t{data[offset + i]} << (8 * i);
    }
    return value;
}

class Reader {
public:
    Reader(const std::filesystem::path& path, std::uint64_t size)
        : stream{path, std::ios::binary}, size{size} {
        if (!stream) {
            throw std::runtime_error("cannot open file for reading");
        }
    }

    std::vector<unsigned char> Read(std::uint64_t offset, std::uint64_t count) {
        if (count > MaxMetadata || !InRange(offset, count, size) ||
            offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            throw std::runtime_error("metadata range invalid or diagnostic limit exceeded");
        }
        std::vector<unsigned char> bytes(static_cast<std::size_t>(count));
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset));
        if (count != 0) {
            stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
        }
        if (!stream || static_cast<std::uint64_t>(stream.gcount()) != count) {
            if (count != 0 || !stream) {
                throw std::runtime_error("metadata read failed; file may have changed");
            }
        }
        return bytes;
    }

private:
    std::ifstream stream;
    std::uint64_t size;
};

struct Segment {
    std::uint64_t type{};
    std::uint64_t offset{};
    std::uint64_t address{};
    std::uint64_t size{};
};

void ParseElf(Reader& reader, FileRecord& file) {
    const auto ident = reader.Read(0, 16);
    if ((ident[4] != 1 && ident[4] != 2) || (ident[5] != 1 && ident[5] != 2) || ident[6] != 1) {
        throw std::runtime_error("invalid ELF identification fields");
    }
    if (ident[4] != 2 || ident[5] != 1) {
        file.status = "unsupported";
        file.notes.emplace_back("Only ELF64 little-endian metadata is supported.");
        return;
    }
    const auto header = reader.Read(0, 64);
    if (header[6] != 1 || ReadLe(header, 20, 4) != 1 || ReadLe(header, 52, 2) != 64) {
        throw std::runtime_error("invalid ELF version or ELF64 header size");
    }
    file.elf = ElfMetadata{static_cast<std::uint16_t>(ReadLe(header, 16, 2)),
                           static_cast<std::uint16_t>(ReadLe(header, 18, 2)), ReadLe(header, 24, 8),
                           static_cast<std::uint16_t>(ReadLe(header, 56, 2))};
    if (file.elf->machine != 62) {
        file.status = "unsupported";
        file.notes.emplace_back("ELF machine is not x86-64.");
        return;
    }
    const auto phoff = ReadLe(header, 32, 8);
    const auto phsize = ReadLe(header, 54, 2);
    const auto phcount = file.elf->program_headers;
    if (phcount > MaxHeaders || (phcount && (phsize != 56 || phoff < 64))) {
        throw std::runtime_error("invalid or unsupported ELF program header table");
    }
    const auto table = reader.Read(phcount ? phoff : 0, phcount * 56ULL);
    std::vector<Segment> segments;
    std::optional<Segment> dynamic;
    std::optional<Segment> sce_data;
    for (std::size_t i = 0; i < phcount; ++i) {
        const auto base = i * 56;
        Segment segment{ReadLe(table, base, 4), ReadLe(table, base + 8, 8),
                        ReadLe(table, base + 16, 8), ReadLe(table, base + 32, 8)};
        if (segment.size && !InRange(segment.offset, segment.size, file.size)) {
            throw std::runtime_error("ELF segment extends beyond the file");
        }
        if (segment.type == 1 && ReadLe(table, base + 40, 8) < segment.size) {
            throw std::runtime_error("ELF PT_LOAD memory size is smaller than file size");
        }
        if (segment.type == 2) {
            if (dynamic) {
                throw std::runtime_error("ambiguous multiple PT_DYNAMIC segments");
            }
            dynamic = segment;
        }
        if (segment.type == 0x61000000) {
            if (sce_data) {
                throw std::runtime_error("ambiguous multiple PT_SCE_DYNLIBDATA segments");
            }
            sce_data = segment;
        }
        segments.push_back(segment);
    }
    file.status = "metadata_validated";
    file.notes.emplace_back("Metadata checks do not establish executability or firmware version.");
    if (!dynamic) {
        file.dependency_status = "no_dynamic_table";
        return;
    }
    if (dynamic->size == 0 || dynamic->size % 16 != 0) {
        throw std::runtime_error("invalid ELF dynamic table size");
    }
    const auto entries = reader.Read(dynamic->offset, dynamic->size);
    std::map<std::uint64_t, std::uint64_t> strings;
    std::vector<std::pair<std::string, std::uint64_t>> needed;
    bool terminated = false;
    for (std::size_t offset = 0; offset < entries.size(); offset += 16) {
        const auto tag = ReadLe(entries, offset, 8);
        const auto value = ReadLe(entries, offset + 8, 8);
        if (tag == 0) {
            terminated = true;
            break;
        }
        if (tag == 1 || tag == 0x6100000f || tag == 0x61000015) {
            if (needed.size() >= MaxDependencies) {
                throw std::runtime_error("dependency count exceeds diagnostic limit");
            }
            const auto kind = tag == 1            ? "DT_NEEDED"
                              : tag == 0x6100000f ? "DT_SCE_NEEDED_MODULE"
                                                  : "DT_SCE_IMPORT_LIB";
            needed.emplace_back(kind, tag == 1 ? value : value & 0xffffffffULL);
        } else if (tag == 5 || tag == 10 || tag == 0x61000035 || tag == 0x61000037) {
            const auto [it, added] = strings.emplace(tag, value);
            if (!added && it->second != value) {
                throw std::runtime_error("conflicting ELF string table metadata");
            }
        }
    }
    if (!terminated) {
        throw std::runtime_error("ELF dynamic table has no DT_NULL terminator");
    }
    file.dependency_status = "declared_names_only";
    if (needed.empty()) {
        return;
    }
    std::uint64_t string_offset{};
    std::uint64_t string_size{};
    if (strings.contains(0x61000035) && strings.contains(0x61000037)) {
        if (!sce_data || !InRange(strings.at(0x61000035), strings.at(0x61000037), sce_data->size)) {
            throw std::runtime_error("SCE string table outside PT_SCE_DYNLIBDATA");
        }
        string_offset = sce_data->offset + strings.at(0x61000035);
        string_size = strings.at(0x61000037);
    } else if (strings.contains(5) && strings.contains(10)) {
        const auto address = strings.at(5);
        string_size = strings.at(10);
        std::size_t matches{};
        for (const auto& segment : segments) {
            if (segment.type == 1 && address >= segment.address &&
                InRange(address - segment.address, string_size, segment.size)) {
                string_offset = segment.offset + (address - segment.address);
                ++matches;
            }
        }
        if (matches != 1) {
            throw std::runtime_error("ELF string table has no unambiguous file-backed PT_LOAD");
        }
    } else {
        throw std::runtime_error("dependency declarations lack a supported string table");
    }
    const auto names = reader.Read(string_offset, string_size);
    for (const auto& [kind, offset] : needed) {
        if (offset >= names.size()) {
            throw std::runtime_error("dependency name offset is outside string table");
        }
        const auto begin = names.begin() + static_cast<std::ptrdiff_t>(offset);
        const auto end = std::find(begin, names.end(), 0);
        if (end == names.end() || end == begin || end - begin > 1024) {
            throw std::runtime_error(
                "dependency name is empty, unterminated or exceeds 1024 bytes");
        }
        file.dependencies.push_back({kind, std::string(begin, end), {}});
    }
}

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
    report.diagnostics.push_back({std::move(category), std::move(message)});
}

void Inspect(ScanReport& report, const std::filesystem::directory_entry& entry,
             std::size_t& total_dependencies) {
    FileRecord file;
    file.path = entry.path().lexically_relative(report.root);
    const auto quoted = Firmware900Manager::JsonString(Firmware900Manager::PathText(file.path));
    const auto filename = Lower(Firmware900Manager::PathText(file.path.filename()));
    if (filename.find("sceshellui") != std::string::npos) {
        file.shell_role = "SceShellUI_name_candidate";
    } else if (filename.find("sceshellcore") != std::string::npos) {
        file.shell_role = "SceShellCore_name_candidate";
    }
    try {
        file.size = entry.file_size();
        Reader reader(entry.path(), file.size);
        const auto prefix = reader.Read(0, std::min<std::uint64_t>(file.size, 4));
        if (prefix == std::vector<unsigned char>{0x7f, 'E', 'L', 'F'}) {
            file.kind = "ELF";
            ParseElf(reader, file);
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
        file.notes.emplace_back(error.what());
        Log(report, "ERROR", quoted + ": " + error.what());
    }
    if (file.dependencies.size() > MaxTotalDependencies - total_dependencies) {
        file.dependencies.resize(MaxTotalDependencies - total_dependencies);
        file.dependency_status = "incomplete";
        report.complete = false;
        Log(report, "ERROR", quoted + ": total dependency limit reached");
    }
    total_dependencies += file.dependencies.size();
    Log(report, "LOADER", quoted + " kind=" + file.kind + " status=" + file.status);
    if (!file.shell_role.empty()) {
        Log(report, "MODULES", quoted + " " + file.shell_role + "; boot not evaluated");
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
    Log(report, "PISTATION", "Firmware 9.00 Dashboard: offline diagnostic mode");
    Log(report, "FIRMWARE900", "Firmware version, loader compatibility and boot are not validated");
    try {
        if (directory.empty() || !std::filesystem::is_directory(directory) || max_entries == 0) {
            throw std::runtime_error(
                "input must be an existing directory; entry limit must be positive");
        }
        report.root = std::filesystem::canonical(directory);
        report.input_valid = true;
        std::error_code ec;
        std::filesystem::recursive_directory_iterator iterator(report.root, ec), end;
        std::size_t visited{};
        std::size_t total_dependencies{};
        while (!ec && iterator != end) {
            if (visited++ >= max_entries) {
                report.complete = false;
                Log(report, "ERROR", "entry limit reached; scan is incomplete");
                break;
            }
            const auto entry = *iterator;
            const auto status = entry.symlink_status(ec);
            if (ec) {
                break;
            }
            if (std::filesystem::is_symlink(status)) {
                iterator.disable_recursion_pending();
                Log(report, "FIRMWARE900",
                    "symlink skipped: " +
                        JsonString(PathText(entry.path().lexically_relative(report.root))));
            } else if (std::filesystem::is_regular_file(status)) {
                Inspect(report, entry, total_dependencies);
            } else if (!std::filesystem::is_directory(status)) {
                Log(report, "FIRMWARE900",
                    "special file skipped: " +
                        JsonString(PathText(entry.path().lexically_relative(report.root))));
            }
            iterator.increment(ec);
        }
        if (ec) {
            report.complete = false;
            Log(report, "ERROR", "directory traversal failed: " + ec.message());
        }
        if (report.files.empty()) {
            Log(report, "ERROR", "input contains no scanned regular files");
        }
    } catch (const std::exception& error) {
        report.complete = false;
        Log(report, "ERROR", error.what());
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
    out << std::boolalpha << "{\n\"schema_version\":1,\"mode\":\"Firmware 9.00 Dashboard\","
        << "\"firmware_version_verified\":false,\"boot_attempted\":false,"
        << "\"symbol_resolution\":\"not_evaluated\",\"root\":" << JsonString(PathText(report.root))
        << ",\"input_valid\":" << report.input_valid << ",\"scan_complete\":" << report.complete
        << ",\"error_count\":" << report.error_count << ",\"files\":[\n";
    bool first = true;
    for (const auto& file : report.files) {
        if (!std::exchange(first, false)) {
            out << ",\n";
        }
        out << "{\"path\":" << JsonString(PathText(file.path)) << ",\"size\":" << file.size
            << ",\"kind\":" << JsonString(file.kind) << ",\"status\":" << JsonString(file.status)
            << ",\"shell_role\":" << JsonString(file.shell_role)
            << ",\"dependency_status\":" << JsonString(file.dependency_status) << ",\"elf\":";
        if (file.elf) {
            out << "{\"type\":" << file.elf->type << ",\"machine\":" << file.elf->machine
                << ",\"entry\":" << JsonString(Hex(file.elf->entry))
                << ",\"program_headers\":" << file.elf->program_headers << '}';
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
                << ",\"name\":" << JsonString(dependency.name)
                << ",\"resolution\":\"not_evaluated\","
                << "\"filename_candidates\":";
            StringArray(out, dependency.filename_candidates);
            out << ",\"filename_candidates_truncated\":" << dependency.candidates_truncated << '}';
        }
        out << "],\"notes\":";
        StringArray(out, file.notes);
        out << '}';
    }
    out << "\n],\"diagnostics\":[";
    first = true;
    for (const auto& diagnostic : report.diagnostics) {
        if (!std::exchange(first, false)) {
            out << ',';
        }
        out << "{\"category\":" << JsonString(diagnostic.category)
            << ",\"message\":" << JsonString(diagnostic.message) << '}';
    }
    out << "]}\n";
    out.close();
    if (!out) {
        throw std::runtime_error("report write failed; output may be incomplete");
    }
}

} // namespace PiStation
