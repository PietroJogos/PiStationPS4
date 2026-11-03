// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdexcept>
#include "firmware900_dependency_analyzer.h"

int main() {
    PiStation::ScanReport report;
    PiStation::FileRecord file;
    file.path = "synthetic.elf";
    file.status = "metadata_validated";
    file.elf = PiStation::ElfMetadata{};
    file.elf->machine = 62;
    PiStation::SymbolMetadata symbol;
    symbol.name = "synthetic_registration";
    symbol.type = 2;
    symbol.imported = true;
    file.symbols.push_back(symbol);
    report.files.push_back(file);
    PiStation::Firmware900DependencyAnalyzer analyzer;
    analyzer.Analyze(
        report, [](const auto& imported) { return imported.name == "synthetic_registration"; });
    if (!report.analysis.hle_registry_checked || report.analysis.imports.size() != 1 ||
        report.analysis.imports[0].status != "hle_registered_functionality_unknown") {
        throw std::runtime_error("HLE registration must not imply implemented functionality");
    }
    std::size_t emitted{};
    PiStation::Firmware900Manager::Emit(report, [&](const auto&) { ++emitted; });
    if (emitted != report.diagnostics.size()) {
        throw std::runtime_error("diagnostic sink did not receive all events");
    }
    analyzer.Analyze(
        report, [](const auto&) -> bool { throw std::runtime_error("synthetic lookup failure"); });
    if (report.analysis.complete || report.analysis.imports[0].status != "hle_registry_error") {
        throw std::runtime_error("failed registry lookup was not isolated");
    }
}
