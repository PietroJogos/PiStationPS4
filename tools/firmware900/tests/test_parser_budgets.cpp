// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include "firmware900_boot_diagnostic.h"

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void Put(std::vector<unsigned char>& bytes, std::size_t offset, std::uint64_t value,
         std::size_t width = 8) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes.at(offset + i) = static_cast<unsigned char>(value >> (i * 8));
    }
}
void Fixture(const std::filesystem::path& path, std::size_t relocations) {
    constexpr std::uint64_t Base = 0x400000;
    std::vector<unsigned char> bytes(std::max<std::size_t>(0x2000, 0x1500 + 24 * relocations));
    const std::vector<unsigned char> ident{0x7f, 'E', 'L', 'F', 2, 1, 1};
    std::copy(ident.begin(), ident.end(), bytes.begin());
    Put(bytes, 16, 3, 2);
    Put(bytes, 18, 62, 2);
    Put(bytes, 20, 1, 4);
    Put(bytes, 24, Base + 0x180);
    Put(bytes, 32, 64);
    Put(bytes, 52, 64, 2);
    Put(bytes, 54, 56, 2);
    Put(bytes, 56, 2, 2);
    Put(bytes, 64, 1, 4);
    Put(bytes, 68, 5, 4);
    Put(bytes, 80, Base);
    Put(bytes, 96, bytes.size());
    Put(bytes, 104, bytes.size());
    Put(bytes, 112, 1);
    std::string names(1, '\0');
    const auto library = names.size();
    names += "required.sprx";
    names.push_back('\0');
    const auto function = names.size();
    names += "function";
    names.push_back('\0');
    std::copy(names.begin(), names.end(), bytes.begin() + 0x1000);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> dynamic{{1, library},
                                                                       {14, library},
                                                                       {5, Base + 0x1000},
                                                                       {10, names.size()},
                                                                       {6, Base + 0x1200},
                                                                       {11, 24},
                                                                       {4, Base + 0x1400},
                                                                       {7, Base + 0x1500},
                                                                       {8, relocations * 24},
                                                                       {9, 24},
                                                                       {0, 0}};
    Put(bytes, 120, 2, 4);
    Put(bytes, 124, 4, 4);
    Put(bytes, 128, 0x200);
    Put(bytes, 136, Base + 0x200);
    Put(bytes, 152, dynamic.size() * 16);
    Put(bytes, 160, dynamic.size() * 16);
    Put(bytes, 168, 8);
    for (std::size_t i = 0; i < dynamic.size(); ++i) {
        Put(bytes, 0x200 + i * 16, dynamic[i].first);
        Put(bytes, 0x208 + i * 16, dynamic[i].second);
    }
    Put(bytes, 0x1218, function, 4);
    bytes[0x121c] = 0x12;
    Put(bytes, 0x1400, 1, 4);
    Put(bytes, 0x1404, 2, 4);
    for (std::size_t i = 0; i < relocations; ++i) {
        Put(bytes, 0x1500 + i * 24, Base + 0x180);
        Put(bytes, 0x1508 + i * 24, 8);
    }
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    Check(static_cast<bool>(output), "fixture write failed");
}
struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};
void Essential(const PiStation::ScanReport& report) {
    Check(report.files.size() == 1, "expected one fixture");
    const auto& file = report.files.front();
    Check(file.elf && file.elf->segments_validated && file.elf->segments.size() == 2 &&
              file.dependencies.size() == 1 && file.module_identity &&
              file.module_identity->name == "required.sprx",
          "essential metadata was erased");
}
} // namespace
int main() {
    using namespace PiStation;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    Cleanup cleanup{std::filesystem::temp_directory_path() /
                    ("pistation-budgets-" + std::to_string(suffix))};
    Check(std::filesystem::create_directory(cleanup.root), "fixture root already exists");
    const auto input = cleanup.root / "input";
    std::filesystem::create_directory(input);
    const auto path = input / "SceShellCore.synthetic.elf";
    Fixture(path, 1);
    auto baseline = Firmware900Manager{}.Scan(input);
    Check(baseline.complete && !baseline.error_count, "baseline fixture invalid");
    ParserBudget budget;
    budget.symbol_bytes = 0;
    auto report = Firmware900Manager{}.Scan(input, 100, budget);
    Essential(report);
    Check(!report.complete && report.error_count == 1 && report.files[0].symbols.empty() &&
              report.files[0].elf->symbol_records_omitted == 1 &&
              report.files[0].elf->symbol_entries_examined == 2 &&
              report.files[0].elf->relocation_entries_examined == 1,
          "retention exhaustion must not stop validation or erase structure");
    Firmware900Manager::DiagnoseServices(report);
    Check(!report.analysis.complete && !report.services.complete,
          "partial analysis claimed complete");
    Firmware900BootDiagnostic{}.Analyze(report);
    Check(report.boot.images[0].entry == 0x400180 && report.boot.images[0].load_segments == 1,
          "known memory plan disappeared when symbol storage ended");
    Firmware900Manager::WriteReport(report, cleanup.root / "report.json");
    Firmware900Manager::WriteTextReport(report, cleanup.root / "report.txt");

    budget = {};
    budget.symbol_entries = 0;
    report = Firmware900Manager{}.Scan(input, 100, budget);
    Essential(report);
    Check(!report.complete && report.files[0].status == "diagnostic_limit_exceeded" &&
              report.files[0].elf->symbol_entries_examined == 0,
          "symbol work limit ignored");
    Firmware900BootDiagnostic{}.Analyze(report);
    Check(report.boot.images[0].entry == 0x400180,
          "partial parser metadata was hidden in boot plan");

    budget = {};
    budget.validation_steps = 0;
    report = Firmware900Manager{}.Scan(input, 100, budget);
    Essential(report);
    Check(!report.complete && report.files[0].elf->relocation_entries_examined == 0,
          "segment validation work limit ignored");

    Fixture(path, 1025);
    budget = {};
    budget.relocation_entries = 1024;
    report = Firmware900Manager{}.Scan(input, 100, budget);
    Essential(report);
    Check(!report.complete && report.files[0].elf->relocation_entries_examined == 1024 &&
              report.files[0].elf->relocation_table_entries == 1025 &&
              report.parser_budget_remaining.relocation_entries == 0,
          "global relocation budget failed across a chunk boundary");

    budget = {};
    budget.structure_bytes = 0;
    report = Firmware900Manager{}.Scan(input, 100, budget);
    Check(!report.complete && !report.files[0].elf->segments_validated &&
              report.files[0].elf->segments.empty(),
          "essential storage limit ignored");

    budget = {};
    budget.symbol_bytes = ParserBudget::MaxSymbolBytes + 1;
    report = Firmware900Manager{}.Scan(input, 100, budget);
    Check(!report.input_valid && report.files.empty(), "library caller bypassed hard scan limits");

    Fixture(path, 1);
    Fixture(input / "00-library.sprx", 1);
    budget = {};
    budget.symbol_bytes = 2 * sizeof(SymbolMetadata) + 96 + std::string("function").size();
    report = Firmware900Manager{}.Scan(input, 100, budget);
    Check(report.files.size() == 2 && report.files[0].path == "00-library.sprx" &&
              report.files[0].symbols.size() == 1 && report.files[1].symbols.empty(),
          "symbol retention budget was not shared in deterministic path order");
    const auto& core = report.files[1];
    Check(core.elf->segments_validated && core.elf->segments.size() == 2 &&
              core.dependencies.size() == 1 && core.module_identity &&
              core.elf->symbol_records_omitted == 1 && core.elf->relocation_entries_examined == 1,
          "later core lost essential data when earlier symbols exhausted storage");
    budget = {};
    budget.symbol_entries = 2;
    report = Firmware900Manager{}.Scan(input, 100, budget);
    Check(report.files[0].status == "metadata_validated" &&
              report.files[1].status == "diagnostic_limit_exceeded" &&
              report.files[1].elf->segments_validated && report.files[1].dependencies.size() == 1,
          "global symbol work budget must bound the whole corpus without erasing later headers");
}
