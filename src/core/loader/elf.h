// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>
#include <vector>

#include "common/io_file.h"
#include "common/types.h"
#include "core/file_sys/ifile.h"

#include "core/loader/elf_types.h"

namespace Core::Loader {

class Elf {
public:
    Elf() = default;
    ~Elf();

    void Open(const std::filesystem::path& file_name);
    void Open(std::unique_ptr<Core::FileSys::IFile> handle);
    bool IsSelfFile() const;
    bool IsElfFile() const;

    [[nodiscard]] self_header GetSElfHeader() const {
        return m_self;
    }

    [[nodiscard]] elf_header GetElfHeader() const {
        return m_elf_header;
    }

    [[nodiscard]] std::span<const elf_program_header> GetProgramHeader() const {
        return m_elf_phdr;
    }

    [[nodiscard]] std::span<const self_segment_header> GetSegmentHeader() const {
        return m_self_segments;
    }

    [[nodiscard]] u64 GetElfEntry() const {
        return m_elf_header.e_entry;
    }

    [[nodiscard]] bool IsSharedLib() const {
        return m_elf_header.e_type == ET_SCE_DYNAMIC;
    }

    std::string SElfHeaderStr();
    std::string SELFSegHeader(u16 no);
    std::string ElfHeaderStr();
    std::string ElfPHeaderStr(u16 no);
    std::string_view ElfPheaderTypeStr(u32 type);
    std::string ElfPheaderFlagsStr(u32 flags);

    void LoadSegment(u64 virtual_addr, u64 file_offset, u64 size);
    bool TryLoadSegment(u64 virtual_addr, u64 file_offset, u64 size);
    bool IsSharedLib();
    void ElfHeaderDebugDump(const std::filesystem::path& file_name);
    void SelfHeaderDebugDump(const std::filesystem::path& file_name);
    void SelfSegHeaderDebugDump(const std::filesystem::path& file_name);
    void PHeaderDebugDump(const std::filesystem::path& file_name);

private:
    Core::FileSys::FileReader m_f{};
    bool is_self{};
    self_header m_self{};
    std::vector<self_segment_header> m_self_segments;
    elf_header m_elf_header{};
    std::vector<elf_program_header> m_elf_phdr;
    std::vector<elf_section_header> m_elf_shdr;
    elf_program_id_header m_self_id_header{};
};

} // namespace Core::Loader
