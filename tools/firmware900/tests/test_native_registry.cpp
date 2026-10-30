// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <stdexcept>
#include <thread>
#include "shadps4_diagnostic_bridge.h"

namespace {

unsigned guest_calls{};

void NeverCall() {
    ++guest_calls;
}

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    Core::Loader::SymbolsResolver registry;
    Core::Loader::SymbolResolver binding{};
    binding.name = "-W4xI5aVI8w";
    binding.library = "libkernel";
    binding.library_version = 1;
    binding.module = "libkernel";
    binding.type = Core::Loader::SymbolType::Function;
    registry.AddSymbol(binding, reinterpret_cast<std::uintptr_t>(&NeverCall));
    binding.name = "NullBinding";
    registry.AddSymbol(binding, 0);
    const auto lookup = PiStation::Shadps4HleLookup(registry);
    PiStation::SymbolMetadata imported{"-W4xI5aVI8w", "libkernel", "libkernel", 1, 2,
                                       false,         true,        true};
    Check(lookup(imported), "native AddSymbol/FindSymbol lookup failed");
    imported.library_version = 99;
    Check(lookup(imported), "native resolver version matching behavior changed");
    imported.library = "another_library";
    Check(!lookup(imported), "lookup must preserve library identity");
    imported.library = "libkernel";
    imported.module = "another_module";
    Check(!lookup(imported), "lookup must preserve module identity");
    imported.module = "libkernel";
    imported.name = "NullBinding";
    Check(!lookup(imported), "null addresses must not count as registered functionality");
    imported.name = "bad";
    Check(!lookup(imported), "invalid NIDs must not query the native resolver");
    imported.name = "-W4xI5aVI8w";
    imported.type = 6;
    Check(!lookup(imported), "unsupported TLS import must not be mapped to NoType");
    imported.type = 2;
    PiStation::ScanReport report;
    report.input_valid = true;
    PiStation::FileRecord file;
    file.path = "synthetic.elf";
    file.status = "metadata_validated";
    file.symbol_status = "parsed_dynamic_symbols";
    file.elf = PiStation::ElfMetadata{};
    file.elf->machine = 62;
    file.symbols.push_back(imported);
    report.files.push_back(file);
    PiStation::Firmware900Manager::DiagnoseServices(report, lookup);
    Check(report.services.live_hle_registry_checked &&
              report.analysis.imports[0].status == "hle_registered_functionality_unknown",
          "Firmware900Manager must use the native resolver adapter without calling it");
    std::atomic<unsigned> failures{};
    std::vector<std::jthread> workers;
    for (unsigned i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            for (unsigned j = 0; j < 100; ++j) {
                if (!lookup(imported)) {
                    ++failures;
                }
            }
        });
    }
    workers.clear();
    Check(failures == 0 && guest_calls == 0,
          "read-only native registry queries must finish without invoking registered code");
}
