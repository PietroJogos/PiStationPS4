// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmware900_boot_diagnostic.h"
#include "firmware900_manager.h"
#include "firmware900_service_report.h"
#include "read_only_firmware_vfs.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

int Run(int argc, char* argv[]) {
    using PiStation::Firmware900Manager;
    try {
        std::filesystem::path directory;
        std::filesystem::path output;
        std::filesystem::path text_output;
        std::vector<std::string> mounts;
        std::vector<std::string> probes;
        std::size_t max_entries = 10000;
        bool boot_diagnostic{};
        bool attempt_boot{};
        for (int i = 1; i < argc; ++i) {
            const std::string_view option{argv[i]};
            if (option == "--help" && argc == 2) {
                std::cout
                    << "PiStation Firmware 9.00 Dashboard — Experimental: offline diagnostics\n"
                       "Usage: pistation-firmware900 --firmware-dir PATH --report NEW_JSON "
                       "[--text-report NEW_TEXT] [--max-entries N] "
                       "[--mount /guest=relative_host] [--vfs-path /guest/file] "
                       "[--boot-diagnostic] [--attempt-boot]\n"
                       "--boot-diagnostic: PS4 Firmware 9.00 Boot Diagnostic (static plan only).\n"
                       "The report must be outside PATH. No firmware version verification, "
                       "decryption, loading or execution is performed.\n"
                       "Exit codes: 0 scan without errors; 1 scan errors/incomplete; "
                       "2 arguments or output failure.\n";
                return 0;
            }
            if (option == "--attempt-boot" && !attempt_boot) {
                attempt_boot = true;
                boot_diagnostic = true;
                continue;
            }
            if (option == "--boot-diagnostic" && !boot_diagnostic) {
                boot_diagnostic = true;
                continue;
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
        if (report.input_valid && (!mounts.empty() || !probes.empty())) {
            try {
                PiStation::ReadOnlyFirmwareVfs vfs(report.root);
                for (const auto& mount : mounts) {
                    const auto separator = mount.find('=');
                    if (separator == std::string::npos) {
                        throw std::runtime_error("VFS mount syntax is /guest=relative_host");
                    }
                    vfs.Mount(mount.substr(0, separator),
                              std::filesystem::path{std::u8string(
                                  mount.begin() + static_cast<std::ptrdiff_t>(separator + 1),
                                  mount.end())});
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
        Firmware900Manager::DiagnoseServices(report);
        if (boot_diagnostic) {
            PiStation::Firmware900BootDiagnostic{}.Analyze(report);
        }
        if (attempt_boot) {
            report.boot.state = "experimental_boot_blocked_before_execution";
            ++report.error_count;
            report.diagnostics.push_back({"PS4_BOOT",
                                          "PS4 Firmware 9.00 Experimental Boot",
                                          "request_boot",
                                          {},
                                          "explicit_request_received_no_guest_execution"});
            report.diagnostics.push_back(
                {"ERROR",
                 "Native emulator execution backend is not linked into this offline tool; "
                 "full emulator build is required before preparing guest memory or calling Linker",
                 "prepare_native_execution_backend",
                 {},
                 "boot_backend_unavailable"});
        }
        Firmware900Manager::Emit(report, [&](const PiStation::Diagnostic& diagnostic) {
            std::cout << '[' << diagnostic.category << "] "
                      << Firmware900Manager::JsonString(diagnostic.message)
                      << " operation=" << Firmware900Manager::JsonString(diagnostic.operation)
                      << " related=" << Firmware900Manager::JsonString(diagnostic.related)
                      << " result=" << Firmware900Manager::JsonString(diagnostic.result);
            if (diagnostic.service) {
                std::cout << " service=";
                PiStation::WriteServiceEventJson(std::cout, *diagnostic.service);
            }
            std::cout << '\n';
        });
        Firmware900Manager::WriteReport(report, output);
        if (!text_output.empty()) {
            Firmware900Manager::WriteTextReport(report, text_output);
        }
        std::cout << "[PISTATION] report exported: "
                  << Firmware900Manager::JsonString(Firmware900Manager::PathText(output)) << '\n';
        return report.input_valid && report.complete && report.analysis.complete &&
                       report.services.complete && report.error_count == 0
                   ? 0
                   : 1;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << Firmware900Manager::JsonString(error.what()) << '\n';
        return 2;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    const auto old_page = GetConsoleOutputCP();
    DWORD mode{};
    const bool console = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
    if (console) {
        SetConsoleOutputCP(CP_UTF8);
    }
    std::vector<std::string> arguments;
    std::vector<char*> pointers;
    for (int i = 0; i < argc; ++i) {
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr,
                                               0, nullptr, nullptr);
        if (!length) {
            if (console) {
                SetConsoleOutputCP(old_page);
            }
            return 2;
        }
        std::string value(static_cast<std::size_t>(length), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(), length,
                                 nullptr, nullptr)) {
            if (console) {
                SetConsoleOutputCP(old_page);
            }
            return 2;
        }
        value.pop_back();
        arguments.push_back(std::move(value));
    }
    for (auto& value : arguments) {
        pointers.push_back(value.data());
    }
    pointers.push_back(nullptr);
    const int result = Run(argc, pointers.data());
    std::cout.flush();
    std::cerr.flush();
    if (console) {
        SetConsoleOutputCP(old_page);
    }
    return result;
}
#else
int main(int argc, char* argv[]) {
    return Run(argc, argv);
}
#endif
