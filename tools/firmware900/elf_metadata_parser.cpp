// SPDX-License-Identifier: GPL-2.0-or-later
#include "elf_metadata_parser.h"

#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include "core/loader/elf_types.h"
#include "firmware900_binary.h"

namespace PiStation {
namespace {

using Binary::InRange;
using Binary::ReadLe;
using Binary::ReadString;
using Input = ReadOnlyFirmwareVfs::File;
constexpr std::size_t MaxEntries = 4096;
static_assert(sizeof(elf_header) == 64 && sizeof(elf_program_header) == 56 &&
              sizeof(elf_symbol) == 24 && sizeof(elf_relocation) == 24);

std::string EncodeId(std::uint16_t id) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string value;
    do {
        value.insert(value.begin(), alphabet[id & 63]);
        id >>= 6;
    } while (id);
    return value;
}

void Symbols(const Input& input, FileRecord& file, std::uint64_t offset, std::uint64_t count,
             std::uint64_t entry_size, const std::vector<unsigned char>& names, bool sce) {
    if (entry_size != sizeof(elf_symbol) || count > MaxEntries) {
        throw std::runtime_error("unsupported symbol table size or entry count");
    }
    const auto data = input.Read(offset, count * sizeof(elf_symbol));
    file.elf->symbol_table_entries = count;
    for (std::size_t i = 0; i < count; ++i) {
        const auto base = i * sizeof(elf_symbol);
        const auto binding = data[base + 4] >> 4;
        const auto visibility = data[base + 5] & 3;
        const auto section = ReadLe(data, base + 6, 2);
        const bool imported = sce ? ReadLe(data, base + 8, 8) == 0 : section == 0;
        const auto name = ReadString(names, ReadLe(data, base, 4));
        if (name.empty() || (binding != STB_GLOBAL && binding != STB_WEAK) ||
            (!imported && visibility != STV_DEFAULT && visibility != STV_PROTECTED)) {
            continue;
        }
        if (section == 0xffff) {
            throw std::runtime_error("extended symbol section indexes are unsupported");
        }
        SymbolMetadata symbol;
        symbol.name = name;
        symbol.type = data[base + 4] & 15;
        symbol.weak = binding == STB_WEAK;
        symbol.imported = imported;
        if (sce) {
            const auto first = name.find('#');
            const auto second = first == std::string::npos ? first : name.find('#', first + 1);
            if (first != std::string::npos && second != std::string::npos &&
                name.find('#', second + 1) == std::string::npos) {
                const auto lib_id = name.substr(first + 1, second - first - 1);
                const auto mod_id = name.substr(second + 1);
                if (imported) {
                    for (const auto& dep : file.dependencies) {
                        if (dep.kind == "DT_SCE_IMPORT_LIB" && EncodeId(dep.id) == lib_id) {
                            symbol.library = dep.name;
                            symbol.library_version = dep.version;
                        }
                        if (dep.kind == "DT_SCE_NEEDED_MODULE" && EncodeId(dep.id) == mod_id) {
                            symbol.module = dep.name;
                        }
                    }
                } else {
                    for (const auto& lib : file.exported_libraries) {
                        if (EncodeId(lib.id) == lib_id) {
                            symbol.library = lib.name;
                            symbol.library_version = lib.version;
                        }
                    }
                    for (const auto& mod : file.exported_modules) {
                        if (EncodeId(mod.id) == mod_id) {
                            symbol.module = mod.name;
                        }
                    }
                }
                if (!symbol.library.empty() && !symbol.module.empty() && first > 0) {
                    symbol.qualified = true;
                    symbol.name = name.substr(0, first);
                }
            }
        }
        file.symbols.push_back(std::move(symbol));
    }
    file.symbol_status = "parsed_dynamic_symbols";
}

void Sections(const Input& input, FileRecord& file, const std::vector<unsigned char>& header) {
    const auto count = ReadLe(header, 60, 2);
    if (count == 0) {
        if (ReadLe(header, 40, 8) != 0) {
            throw std::runtime_error("extended ELF section counts are unsupported");
        }
        return;
    }
    if (count > MaxEntries || ReadLe(header, 58, 2) != sizeof(elf_section_header)) {
        throw std::runtime_error("unsupported section table count or size");
    }
    const auto offset = ReadLe(header, 40, 8);
    if (offset < sizeof(elf_header)) {
        throw std::runtime_error("section table overlaps ELF header");
    }
    const auto sections = input.Read(offset, count * sizeof(elf_section_header));
    for (std::size_t i = 0; i < count; ++i) {
        const auto base = i * sizeof(elf_section_header);
        const auto type = ReadLe(sections, base + 4, 4);
        const auto start = ReadLe(sections, base + 24, 8);
        const auto size = ReadLe(sections, base + 32, 8);
        if (type != 8 && size && !InRange(start, size, file.size)) {
            throw std::runtime_error("section range exceeds file");
        }
        if (type != 11 || file.symbol_status == "parsed_dynamic_symbols") {
            continue;
        }
        const auto link = ReadLe(sections, base + 40, 4);
        const auto entry_size = ReadLe(sections, base + 56, 8);
        if (link >= count || entry_size != sizeof(elf_symbol) || size % entry_size != 0) {
            throw std::runtime_error("invalid dynamic symbol section or linked string table");
        }
        const auto strings_base = static_cast<std::size_t>(link) * sizeof(elf_section_header);
        if (ReadLe(sections, strings_base + 4, 4) != 3) {
            throw std::runtime_error("dynamic symbols do not link to SHT_STRTAB");
        }
        const auto names = input.Read(ReadLe(sections, strings_base + 24, 8),
                                      ReadLe(sections, strings_base + 32, 8));
        Symbols(input, file, start, size / entry_size, entry_size, names, false);
    }
}

} // namespace

void AnalyzeElfMetadata(const Input& input, FileRecord& file) {
    const auto ident = input.Read(0, 16);
    if ((ident[4] != 1 && ident[4] != 2) || (ident[5] != 1 && ident[5] != 2) || ident[6] != 1) {
        throw std::runtime_error("invalid ELF identification fields");
    }
    if (ident[4] != 2 || ident[5] != 1) {
        file.status = "unsupported";
        file.notes.emplace_back("Only ELF64 little-endian metadata is supported.");
        return;
    }
    const auto header = input.Read(0, sizeof(elf_header));
    if (ReadLe(header, 20, 4) != 1 || ReadLe(header, 52, 2) != sizeof(elf_header)) {
        throw std::runtime_error("invalid ELF version or header size");
    }
    file.elf = ElfMetadata{};
    auto& elf = *file.elf;
    elf.type = static_cast<std::uint16_t>(ReadLe(header, 16, 2));
    elf.machine = static_cast<std::uint16_t>(ReadLe(header, 18, 2));
    elf.entry = ReadLe(header, 24, 8);
    elf.program_headers = static_cast<std::uint16_t>(ReadLe(header, 56, 2));
    if (elf.machine != EM_X86_64 ||
        (elf.type != ET_EXEC && elf.type != ET_DYN && elf.type != ET_SCE_EXEC &&
         elf.type != ET_SCE_DYNEXEC && elf.type != ET_SCE_DYNAMIC)) {
        file.status = "unsupported";
        file.notes.emplace_back("Unsupported machine or executable type.");
        return;
    }
    file.module_role =
        elf.type == ET_DYN || elf.type == ET_SCE_DYNAMIC ? "shared_module" : "executable_candidate";
    const auto phoff = ReadLe(header, 32, 8);
    if (elf.program_headers > MaxEntries ||
        (elf.program_headers &&
         (ReadLe(header, 54, 2) != sizeof(elf_program_header) || phoff < sizeof(elf_header)))) {
        throw std::runtime_error("invalid or unsupported ELF program header table");
    }
    const auto ph = input.Read(elf.program_headers ? phoff : 0,
                               elf.program_headers * sizeof(elf_program_header));
    std::optional<SegmentMetadata> dynamic;
    std::optional<SegmentMetadata> sce_data;
    for (std::size_t i = 0; i < elf.program_headers; ++i) {
        const auto b = i * sizeof(elf_program_header);
        SegmentMetadata s{ReadLe(ph, b, 4),      ReadLe(ph, b + 4, 4),  ReadLe(ph, b + 8, 8),
                          ReadLe(ph, b + 16, 8), ReadLe(ph, b + 32, 8), ReadLe(ph, b + 40, 8),
                          ReadLe(ph, b + 48, 8)};
        if ((s.file_size && !InRange(s.offset, s.file_size, file.size)) ||
            !InRange(s.address, s.memory_size, std::numeric_limits<std::uint64_t>::max()) ||
            (s.alignment && (s.alignment & (s.alignment - 1)))) {
            throw std::runtime_error("ELF segment bounds, virtual range or alignment invalid");
        }
        if ((s.type == PT_LOAD || s.type == PT_TLS) &&
            (s.memory_size < s.file_size ||
             (s.alignment > 1 && s.offset % s.alignment != s.address % s.alignment))) {
            throw std::runtime_error("ELF load/TLS sizes or alignment are inconsistent");
        }
        if (s.type == PT_LOAD && (s.flags & 1) && elf.entry >= s.address &&
            elf.entry - s.address < s.memory_size) {
            elf.entry_in_executable_segment = true;
        }
        elf.tls_present |= s.type == PT_TLS;
        if (s.type == PT_DYNAMIC || s.type == PT_SCE_DYNLIBDATA) {
            auto& target = s.type == PT_DYNAMIC ? dynamic : sce_data;
            if (target) {
                throw std::runtime_error("ambiguous duplicate dynamic metadata segments");
            }
            target = s;
        }
        elf.segments.push_back(s);
    }
    file.status = "metadata_validated";
    file.symbol_status = "not_present";
    file.dependency_status = dynamic ? "declared_names_only" : "no_dynamic_table";
    if (!elf.entry_in_executable_segment) {
        file.notes.emplace_back("Entry point is not covered by an executable PT_LOAD segment.");
    }
    if (!dynamic) {
        Sections(input, file, header);
        return;
    }
    if (!dynamic->file_size || dynamic->file_size % sizeof(elf_dynamic)) {
        throw std::runtime_error("invalid ELF dynamic table size");
    }
    const auto entries = input.Read(dynamic->offset, dynamic->file_size);
    std::map<std::uint64_t, std::uint64_t> tags;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> identities;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> needed;
    const std::set<std::uint64_t> supported_tags = {4,
                                                    5,
                                                    6,
                                                    7,
                                                    8,
                                                    9,
                                                    10,
                                                    11,
                                                    14,
                                                    2,
                                                    20,
                                                    23,
                                                    DT_SCE_STRTAB,
                                                    DT_SCE_STRSZ,
                                                    DT_SCE_SYMTAB,
                                                    DT_SCE_SYMTABSZ,
                                                    DT_SCE_SYMENT,
                                                    DT_SCE_RELA,
                                                    DT_SCE_RELASZ,
                                                    DT_SCE_RELAENT,
                                                    DT_SCE_JMPREL,
                                                    DT_SCE_PLTRELSZ,
                                                    DT_SCE_PLTREL};
    bool terminated{};
    for (std::size_t b = 0; b < entries.size(); b += sizeof(elf_dynamic)) {
        const auto tag = ReadLe(entries, b, 8);
        const auto value = ReadLe(entries, b + 8, 8);
        if (tag == DT_NULL) {
            terminated = true;
            break;
        }
        if (tag == DT_NEEDED || tag == DT_SCE_NEEDED_MODULE || tag == DT_SCE_IMPORT_LIB) {
            if (needed.size() >= MaxEntries) {
                throw std::runtime_error("dependency count exceeds diagnostic limit");
            }
            needed.emplace_back(tag, value);
        } else if (tag == DT_SCE_MODULE_INFO || tag == DT_SCE_EXPORT_LIB) {
            if (identities.size() >= MaxEntries) {
                throw std::runtime_error("module identity count exceeds diagnostic limit");
            }
            identities.emplace_back(tag, value);
        } else if (supported_tags.contains(tag)) {
            const auto [it, added] = tags.emplace(tag, value);
            if (!added && it->second != value) {
                throw std::runtime_error("conflicting dynamic metadata");
            }
        }
    }
    if (!terminated) {
        throw std::runtime_error("ELF dynamic table has no DT_NULL terminator");
    }
    for (const auto& [pointer, size] : std::array<std::pair<std::uint64_t, std::uint64_t>, 3>{
             {{5, 10}, {DT_SCE_STRTAB, DT_SCE_STRSZ}, {DT_SCE_SYMTAB, DT_SCE_SYMTABSZ}}}) {
        if (tags.contains(pointer) != tags.contains(size)) {
            throw std::runtime_error("incomplete dynamic pointer/size metadata");
        }
    }
    const auto locate = [&](std::uint64_t address, std::uint64_t size) {
        std::optional<std::uint64_t> offset;
        for (const auto& segment : elf.segments) {
            if (segment.type == PT_LOAD && address >= segment.address &&
                InRange(address - segment.address, size, segment.file_size)) {
                if (offset) {
                    throw std::runtime_error("ambiguous virtual metadata range");
                }
                offset = segment.offset + address - segment.address;
            }
        }
        if (!offset) {
            throw std::runtime_error("metadata address has no file-backed PT_LOAD");
        }
        return *offset;
    };
    const auto sce_offset = [&](std::uint64_t offset, std::uint64_t size) {
        if (!sce_data || !InRange(offset, size, sce_data->file_size)) {
            throw std::runtime_error("SCE metadata outside PT_SCE_DYNLIBDATA");
        }
        return sce_data->offset + offset;
    };
    std::vector<unsigned char> names;
    if (tags.contains(DT_SCE_STRTAB) && tags.contains(DT_SCE_STRSZ)) {
        names = input.Read(sce_offset(tags.at(DT_SCE_STRTAB), tags.at(DT_SCE_STRSZ)),
                           tags.at(DT_SCE_STRSZ));
    } else if (tags.contains(5) && tags.contains(10)) {
        names = input.Read(locate(tags.at(5), tags.at(10)), tags.at(10));
    }
    const auto name_at = [&](std::uint64_t offset) {
        auto name = ReadString(names, offset);
        if (name.empty()) {
            throw std::runtime_error("empty module or dependency name");
        }
        return name;
    };
    for (const auto& [tag, value] : needed) {
        Dependency d;
        d.name = name_at(tag == DT_NEEDED ? value : value & 0xffffffffULL);
        d.kind = tag == DT_NEEDED              ? "DT_NEEDED"
                 : tag == DT_SCE_NEEDED_MODULE ? "DT_SCE_NEEDED_MODULE"
                                               : "DT_SCE_IMPORT_LIB";
        d.id = static_cast<std::uint16_t>(value >> 48);
        d.version = static_cast<std::uint16_t>(value >> (tag == DT_SCE_IMPORT_LIB ? 32 : 40));
        if (tag == DT_SCE_NEEDED_MODULE) {
            d.version &= 255;
            d.minor_version = static_cast<std::uint16_t>((value >> 32) & 255);
        }
        file.dependencies.push_back(std::move(d));
    }
    for (const auto& [tag, value] : identities) {
        ModuleIdentity identity;
        identity.name = name_at(value & 0xffffffffULL);
        identity.id = static_cast<std::uint16_t>(value >> 48);
        identity.version = static_cast<std::uint16_t>((value >> 32) & 0xffff);
        if (tag == DT_SCE_MODULE_INFO) {
            identity.version = static_cast<std::uint16_t>((value >> 40) & 255);
            identity.minor_version = static_cast<std::uint16_t>((value >> 32) & 255);
            if (file.module_identity) {
                throw std::runtime_error("duplicate module identity");
            }
            file.module_identity = identity;
            file.exported_modules.push_back(identity);
        } else {
            file.exported_libraries.push_back(identity);
        }
    }
    std::set<std::pair<std::string, std::uint16_t>> imported_ids;
    for (const auto& dependency : file.dependencies) {
        if (dependency.kind != "DT_NEEDED" &&
            !imported_ids.emplace(dependency.kind, dependency.id).second) {
            throw std::runtime_error("duplicate SCE import identifier");
        }
    }
    std::set<std::uint16_t> exported_ids;
    for (const auto& library : file.exported_libraries) {
        if (!exported_ids.insert(library.id).second) {
            throw std::runtime_error("duplicate SCE export library identifier");
        }
    }
    if (tags.contains(14) && !file.module_identity) {
        file.module_identity = ModuleIdentity{name_at(tags.at(14))};
    }
    if (tags.contains(DT_SCE_SYMTAB) && tags.contains(DT_SCE_SYMTABSZ)) {
        const auto size = tags.at(DT_SCE_SYMTABSZ);
        const auto entry_size = tags.contains(DT_SCE_SYMENT) ? tags.at(DT_SCE_SYMENT) : 24;
        if (entry_size != sizeof(elf_symbol) || size % entry_size != 0) {
            throw std::runtime_error("invalid SCE symbol table entry size");
        }
        Symbols(input, file, sce_offset(tags.at(DT_SCE_SYMTAB), size), size / entry_size,
                entry_size, names, true);
    } else if (tags.contains(6) && tags.contains(4)) {
        const auto hash = input.Read(locate(tags.at(4), 8), 8);
        const auto buckets = ReadLe(hash, 0, 4);
        const auto count = ReadLe(hash, 4, 4);
        const auto entry_size = tags.contains(11) ? tags.at(11) : 24;
        if (entry_size != sizeof(elf_symbol) || count > MaxEntries || buckets > MaxEntries) {
            throw std::runtime_error("invalid ELF hash symbol count or symbol entry size");
        }
        locate(tags.at(4), 8 + 4 * (buckets + count));
        Symbols(input, file, locate(tags.at(6), count * entry_size), count, entry_size, names,
                false);
    } else if (tags.contains(6)) {
        file.symbol_status = "count_unavailable";
    }
    const auto relocations = [&](std::uint64_t ptr, std::uint64_t size_tag, std::uint64_t entry_tag,
                                 bool sce, bool plt) {
        if (!tags.contains(ptr) && !tags.contains(size_tag)) {
            return;
        }
        if (!tags.contains(ptr) || !tags.contains(size_tag) ||
            (tags.contains(entry_tag) && tags.at(entry_tag) != (plt ? DT_RELA : 24))) {
            throw std::runtime_error("unsupported or incomplete relocation metadata");
        }
        const auto size = tags.at(size_tag);
        if (size % sizeof(elf_relocation) || size / sizeof(elf_relocation) > MaxEntries) {
            throw std::runtime_error("relocation table exceeds size/count limit");
        }
        const auto data =
            input.Read(sce ? sce_offset(tags.at(ptr), size) : locate(tags.at(ptr), size), size);
        for (std::size_t b = 0; b < data.size(); b += sizeof(elf_relocation)) {
            const auto address = ReadLe(data, b, 8);
            const auto info = ReadLe(data, b + 8, 8);
            const auto type = static_cast<std::uint32_t>(info);
            if ((info >> 32) && file.elf->symbol_table_entries &&
                (info >> 32) >= file.elf->symbol_table_entries) {
                throw std::runtime_error("relocation symbol index outside symbol table");
            }
            const bool supported = type == R_X86_64_64 || type == R_X86_64_GLOB_DAT ||
                                   type == R_X86_64_JUMP_SLOT || type == R_X86_64_RELATIVE ||
                                   type == R_X86_64_DTPMOD64;
            bool mapped = type == 0;
            for (const auto& segment : elf.segments) {
                mapped |=
                    segment.type == PT_LOAD && address >= segment.address &&
                    InRange(address - segment.address, supported ? 8 : 1, segment.memory_size);
            }
            if (!mapped) {
                throw std::runtime_error("relocation destination is outside PT_LOAD");
            }
            ++elf.relocation_types[type];
        }
        elf.relocation_status = "metadata_only_not_applied";
    };
    relocations(DT_SCE_RELA, DT_SCE_RELASZ, DT_SCE_RELAENT, true, false);
    relocations(DT_SCE_JMPREL, DT_SCE_PLTRELSZ, DT_SCE_PLTREL, true, true);
    relocations(7, 8, 9, false, false);
    relocations(23, 2, 20, false, true);
    Sections(input, file, header);
}

} // namespace PiStation
