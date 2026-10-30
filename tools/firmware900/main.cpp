// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmware900_manager.h"

#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char* argv[]) {
    using PiStation::Firmware900Manager;
    try {
        std::filesystem::path directory;
        std::filesystem::path output;
        std::size_t max_entries = 10000;
        for (int i = 1; i < argc; ++i) {
            const std::string_view option{argv[i]};
            if (option == "--help" && argc == 2) {
                std::cout << "PiStation Firmware 9.00 Dashboard — offline diagnostics\n"
                             "Usage: pistation-firmware900 --firmware-dir PATH --report NEW_JSON "
                             "[--max-entries N]\n"
                             "The report must be outside PATH. No firmware version verification, "
                             "decryption, loading or execution is performed.\n"
                             "Exit codes: 0 scan without errors; 1 scan errors/incomplete; "
                             "2 arguments or output failure.\n";
                return 0;
            }
            if (i + 1 >= argc) {
                throw std::runtime_error("missing option value; use --help");
            }
            const std::string_view value{argv[++i]};
            if (option == "--firmware-dir" && directory.empty()) {
                directory = std::filesystem::path(std::u8string(value.begin(), value.end()));
            } else if (option == "--report" && output.empty()) {
                output = std::filesystem::path(std::u8string(value.begin(), value.end()));
            } else if (option == "--max-entries") {
                const auto [end, ec] =
                    std::from_chars(value.data(), value.data() + value.size(), max_entries);
                if (ec != std::errc{} || end != value.data() + value.size() || max_entries == 0 ||
                    max_entries > 100000) {
                    throw std::runtime_error("max-entries must be an integer from 1 to 100000");
                }
            } else {
                throw std::runtime_error("unknown or duplicate option; use --help");
            }
        }
        if (directory.empty() || output.empty()) {
            throw std::runtime_error("--firmware-dir and --report are required; use --help");
        }
        const auto report = Firmware900Manager{}.Scan(directory, max_entries);
        for (const auto& diagnostic : report.diagnostics) {
            std::cout << '[' << diagnostic.category << "] "
                      << Firmware900Manager::JsonString(diagnostic.message) << '\n';
        }
        Firmware900Manager::WriteReport(report, output);
        std::cout << "[PISTATION] report exported: "
                  << Firmware900Manager::JsonString(Firmware900Manager::PathText(output)) << '\n';
        return report.input_valid && report.complete && report.error_count == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << Firmware900Manager::JsonString(error.what()) << '\n';
        return 2;
    }
}
