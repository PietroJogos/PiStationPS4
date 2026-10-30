// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmware900_dependency_analyzer.h"
#include "firmware900_manager.h"
#include "read_only_firmware_vfs.h"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char* argv[]) {
    using PiStation::Firmware900Manager;
    try {
        std::filesystem::path directory;
        std::filesystem::path output;
        std::filesystem::path text_output;
        std::vector<std::string> mounts;
        std::vector<std::string> probes;
        std::size_t max_entries = 10000;
        for (int i = 1; i < argc; ++i) {
            const std::string_view option{argv[i]};
            if (option == "--help" && argc == 2) {
                std::cout
                    << "PiStation Firmware 9.00 Dashboard — Experimental: offline diagnostics\n"
                       "Usage: pistation-firmware900 --firmware-dir PATH --report NEW_JSON "
                       "[--text-report NEW_TEXT] [--max-entries N] "
                       "[--mount /guest=relative_host] [--vfs-path /guest/file]\n"
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
            } else if (option == "--text-report" && text_output.empty()) {
                text_output = std::filesystem::path(std::u8string(value.begin(), value.end()));
            } else if (option == "--mount" && mounts.size() < 32) {
                mounts.emplace_back(value);
            } else if (option == "--vfs-path" && probes.size() < 32) {
                probes.emplace_back(value);
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
        auto report = Firmware900Manager{}.Scan(directory, max_entries);
        PiStation::Firmware900DependencyAnalyzer{}.Analyze(report);
        if (report.input_valid && (!mounts.empty() || !probes.empty())) {
            try {
                PiStation::ReadOnlyFirmwareVfs vfs(report.root);
                for (const auto& mount : mounts) {
                    const auto separator = mount.find('=');
                    if (separator == std::string::npos) {
                        throw std::runtime_error("VFS mount syntax is /guest=relative_host");
                    }
                    vfs.Mount(mount.substr(0, separator),
                              std::filesystem::path{mount.substr(separator + 1)});
                    report.diagnostics.push_back({"VFS", "read-only mapping configured",
                                                  "mount_readonly", mount.substr(0, separator),
                                                  "configured"});
                }
                for (const auto& path : probes) {
                    try {
                        auto file = vfs.OpenVirtual(path);
                        file.Read(0, std::min<std::uint64_t>(16, file.Size()));
                        report.diagnostics.push_back(
                            {"VFS", "bounded metadata read", "open_readonly", path, "allowed"});
                    } catch (const std::exception& error) {
                        ++report.error_count;
                        report.diagnostics.push_back(
                            {"ERROR", error.what(), "VFS_open_readonly", path, "denied"});
                        report.diagnostics.push_back(
                            {"VFS", "access denied", "open_readonly", path, "denied"});
                    }
                }
            } catch (const std::exception& error) {
                ++report.error_count;
                report.diagnostics.push_back(
                    {"ERROR", error.what(), "VFS_mount_readonly", {}, "denied"});
            }
        }
        Firmware900Manager::Emit(report, [&](const PiStation::Diagnostic& diagnostic) {
            std::cout << '[' << diagnostic.category << "] "
                      << Firmware900Manager::JsonString(diagnostic.message)
                      << " operation=" << Firmware900Manager::JsonString(diagnostic.operation)
                      << " related=" << Firmware900Manager::JsonString(diagnostic.related)
                      << " result=" << Firmware900Manager::JsonString(diagnostic.result) << '\n';
        });
        Firmware900Manager::WriteReport(report, output);
        if (!text_output.empty()) {
            Firmware900Manager::WriteTextReport(report, text_output);
        }
        std::cout << "[PISTATION] report exported: "
                  << Firmware900Manager::JsonString(Firmware900Manager::PathText(output)) << '\n';
        return report.input_valid && report.complete && report.analysis.complete &&
                       report.error_count == 0
                   ? 0
                   : 1;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << Firmware900Manager::JsonString(error.what()) << '\n';
        return 2;
    }
}
