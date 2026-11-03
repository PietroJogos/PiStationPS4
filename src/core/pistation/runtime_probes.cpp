// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <thread>
#include <fmt/format.h>
#include "common/elf_info.h"
#include "common/singleton.h"
#include "core/libraries/ipmi/ipmimgr.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/libs.h"
#include "core/linker.h"
#include "core/pistation/ipc_service_manager.h"
#include "core/pistation/native_boot_backend.h"
#include "core/pistation/runtime_probes.h"
#include "input/controller.h"
#include "sdl_window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern Frontend::WindowSDL* g_window;

namespace PiStation {

namespace {

using namespace std::chrono_literals;

struct CheckLog {
    NativeBootBackend& backend;
    u32 passed{};
    u32 failed{};

    void operator()(const char* name, bool condition, const std::string& detail = {}) {
        if (condition) {
            passed++;
            backend.Record("IPC", name, detail, "pass");
        } else {
            failed++;
            backend.RecordFailure("IPC", name, detail);
        }
    }
};

u32 Checksum(const std::vector<u8>& data) {
    u32 sum = 2166136261u;
    for (const u8 byte : data) {
        sum = (sum ^ byte) * 16777619u;
    }
    return sum;
}

void RunBasicChecks(CheckLog& check) {
    Ipc::ServiceManager manager{Ipc::Limits{.max_endpoints = 2, .max_queue_depth = 2,
                                            .max_message_size = 16}};
    check("register_endpoint", manager.RegisterEndpoint("svc.a", 1) == Ipc::Result::Ok);
    check("register_duplicate_rejected",
          manager.RegisterEndpoint("svc.a", 2) == Ipc::Result::AlreadyExists);
    check("register_empty_name_rejected",
          manager.RegisterEndpoint("", 1) == Ipc::Result::InvalidArgument);
    check("register_second_endpoint", manager.RegisterEndpoint("svc.b", 1) == Ipc::Result::Ok);
    check("endpoint_limit", manager.RegisterEndpoint("svc.c", 1) == Ipc::Result::LimitReached);
    check("list_endpoints", manager.ListEndpoints() == std::vector<std::string>{"svc.a", "svc.b"});

    u32 id = 99;
    check("connect_unknown_endpoint",
          manager.Connect("missing", 7, &id) == Ipc::Result::NotFound && id == 0);
    check("connect_null_out", manager.Connect("svc.a", 7, nullptr) ==
                                  Ipc::Result::InvalidArgument);

    check("connect_ok", manager.Connect("svc.a", 7, &id) == Ipc::Result::Ok && id != 0);
    check("message_too_large",
          manager.SendRequest(id, {1, std::vector<u8>(17)}) == Ipc::Result::MessageTooLarge);
    check("send_1", manager.SendRequest(id, {1, {1}}) == Ipc::Result::Ok);
    check("send_2", manager.SendRequest(id, {2, {2}}) == Ipc::Result::Ok);
    check("queue_full", manager.SendRequest(id, {3, {3}}) == Ipc::Result::QueueFull);

    Ipc::Request request;
    check("receive_wrong_owner",
          manager.ReceiveRequest("svc.a", 2, &request, 0ms) == Ipc::Result::NotOwner);
    check("receive_request",
          manager.ReceiveRequest("svc.a", 1, &request, 0ms) == Ipc::Result::Ok &&
              request.message.method == 1 && request.client_id == 7 &&
              request.connection_id == id);
    check("reply_wrong_owner", manager.SendReply(id, 2, std::vector<u8>{9}) == Ipc::Result::NotOwner);
    check("reply_ok", manager.SendReply(id, 1, std::vector<u8>{9, 8}) == Ipc::Result::Ok);

    std::vector<u8> reply;
    check("receive_reply", manager.ReceiveReply(id, &reply, 0ms) == Ipc::Result::Ok &&
                               reply == std::vector<u8>{9, 8});

    const auto before = std::chrono::steady_clock::now();
    const auto timeout_result = manager.ReceiveReply(id, &reply, 50ms);
    const auto waited = std::chrono::steady_clock::now() - before;
    check("reply_timeout", timeout_result == Ipc::Result::Timeout && waited >= 45ms,
          fmt::format("waited {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(waited)
                                          .count()));

    check("disconnect_ok", manager.Disconnect(id) == Ipc::Result::Ok);
    check("disconnect_twice", manager.Disconnect(id) == Ipc::Result::NotFound);
    check("send_after_disconnect",
          manager.SendRequest(id, {1, {1}}) == Ipc::Result::Disconnected);
    check("disconnect_drops_pending_requests",
          manager.ReceiveRequest("svc.a", 1, &request, 0ms) == Ipc::Result::Timeout);

    {
        Ipc::ScopedConnection scoped{manager, "svc.a", 8};
        check("scoped_connection_open", scoped.Status() == Ipc::Result::Ok &&
                                            manager.ConnectionCount() == 1);
    }
    check("scoped_connection_released", manager.ConnectionCount() == 0);

    check("unregister_wrong_owner",
          manager.UnregisterEndpoint("svc.a", 2) == Ipc::Result::NotOwner);
    check("unregister_ok", manager.UnregisterEndpoint("svc.a", 1) == Ipc::Result::Ok);
    check("trace_recorded", !manager.Trace().empty());
}

void RunDisconnectWakeCheck(CheckLog& check) {
    Ipc::ServiceManager manager;
    manager.RegisterEndpoint("svc.wake", 1);
    u32 id{};
    manager.Connect("svc.wake", 5, &id);
    std::atomic<Ipc::Result> outcome{Ipc::Result::Ok};
    std::thread waiter{[&] {
        std::vector<u8> reply;
        outcome = manager.ReceiveReply(id, &reply, 5000ms);
    }};
    std::this_thread::sleep_for(50ms);
    manager.Disconnect(id);
    waiter.join();
    check("disconnect_wakes_blocked_receiver", outcome == Ipc::Result::Disconnected);

    std::atomic<Ipc::Result> server_outcome{Ipc::Result::Ok};
    std::thread server{[&] {
        Ipc::Request request;
        server_outcome = manager.ReceiveRequest("svc.wake", 1, &request, 5000ms);
    }};
    std::this_thread::sleep_for(50ms);
    manager.Shutdown();
    server.join();
    check("shutdown_wakes_blocked_server", server_outcome == Ipc::Result::ShuttingDown);
    check("connect_after_shutdown",
          manager.Connect("svc.wake", 5, &id) == Ipc::Result::ShuttingDown);
}

void RunConcurrencyCheck(CheckLog& check) {
    constexpr u32 ClientCount = 8;
    constexpr u32 RequestsPerClient = 50;
    Ipc::ServiceManager manager{Ipc::Limits{.max_queue_depth = 1024}};
    manager.RegisterEndpoint("svc.echo", 1);

    std::atomic<bool> stop{false};
    std::thread server{[&] {
        Ipc::Request request;
        while (!stop) {
            if (manager.ReceiveRequest("svc.echo", 1, &request, 20ms) != Ipc::Result::Ok) {
                continue;
            }
            std::vector<u8> payload = request.message.payload;
            const u32 sum = Checksum(payload);
            payload.insert(payload.end(), reinterpret_cast<const u8*>(&sum),
                           reinterpret_cast<const u8*>(&sum) + sizeof(sum));
            manager.SendReply(request.connection_id, 1, payload);
        }
    }};

    std::atomic<u32> good{};
    std::atomic<u32> bad{};
    std::vector<std::thread> clients;
    for (u32 c = 0; c < ClientCount; c++) {
        clients.emplace_back([&, c] {
            Ipc::ScopedConnection connection{manager, "svc.echo", 100 + c};
            if (connection.Status() != Ipc::Result::Ok) {
                bad += RequestsPerClient;
                return;
            }
            for (u32 i = 0; i < RequestsPerClient; i++) {
                std::vector<u8> payload(16, static_cast<u8>(c * 31 + i));
                if (manager.SendRequest(connection.Id(), {i, payload}) != Ipc::Result::Ok) {
                    bad++;
                    continue;
                }
                std::vector<u8> reply;
                if (manager.ReceiveReply(connection.Id(), &reply, 5000ms) != Ipc::Result::Ok ||
                    reply.size() != payload.size() + sizeof(u32)) {
                    bad++;
                    continue;
                }
                u32 sum{};
                std::memcpy(&sum, reply.data() + payload.size(), sizeof(sum));
                reply.resize(payload.size());
                (reply == payload && sum == Checksum(payload) ? good : bad)++;
            }
        });
    }
    for (auto& client : clients) {
        client.join();
    }
    stop = true;
    server.join();
    check("concurrent_echo", good == ClientCount * RequestsPerClient && bad == 0,
          fmt::format("good={} bad={}", good.load(), bad.load()));
    check("concurrent_connections_released", manager.ConnectionCount() == 0);
}

void RunIpmiKernelChecks(CheckLog& check) {
    using namespace Libraries::Ipmi;
    static const char ServiceName[] = "SyntheticService";
    IpmiKernelManager kernel{[](u64 address) -> std::optional<std::string> {
        if (address == reinterpret_cast<u64>(ServiceName)) {
            return std::string{ServiceName};
        }
        return std::nullopt;
    }};
    s32 result = IpmiResultUnset;
    const u64 create_args[3] = {0x1234, 0x5678, 0};

    check("ipmi_null_result_rejected",
          kernel.Call(IpmiCreateOp, 0, nullptr, create_args, IpmiCreateArgsSize) == POSIX_EFAULT);
    check("ipmi_create_wrong_size_rejected",
          kernel.Call(IpmiCreateOp, 0, &result, create_args, 0x10) == POSIX_EINVAL &&
              result == IpmiResultUnset);
    const u64 null_config[3] = {0x1234, 0, 0};
    check("ipmi_create_null_config_rejected",
          kernel.Call(IpmiCreateOp, 0, &result, null_config, IpmiCreateArgsSize) == POSIX_EINVAL);
    check("ipmi_create", kernel.Call(IpmiCreateOp, 0, &result, create_args, IpmiCreateArgsSize) ==
                             0 && result > 0 && kernel.HandleCount() == 1);
    const s32 first = result;
    result = IpmiResultUnset;
    check("ipmi_second_handle_differs",
          kernel.Call(IpmiCreateOp, 0, &result, create_args, IpmiCreateArgsSize) == 0 &&
              result > 0 && result != first && kernel.HandleCount() == 2);
    result = IpmiResultUnset;
    const u64 named_args[3] = {0x1234, reinterpret_cast<u64>(ServiceName), 0};
    s32 named_result = IpmiResultUnset;
    check("ipmi_create_records_service_name",
          kernel.Call(IpmiCreateOp, 0, &named_result, named_args, IpmiCreateArgsSize) == 0 &&
              kernel.ServiceNameOf(named_result) == std::string{ServiceName});
    check("ipmi_unreadable_name_leaves_none", !kernel.ServiceNameOf(first).has_value());
    check("ipmi_operation_names",
          std::string{IpmiOperationName(0x310)} == "ClientImpl::disconnect" &&
              std::string{IpmiOperationName(0x2)} == "ClientImpl::initialize (Client::create)" &&
              IpmiOperationName(0x400) == nullptr);
    check("ipmi_unknown_op_enosys",
          kernel.Call(0x400, first, &result, create_args, 0x20) == POSIX_ENOSYS &&
              result == IpmiResultUnset);
    check("ipmi_destroy_with_args_rejected",
          kernel.Call(IpmiDestroyOp, first, &result, create_args, 8) == POSIX_EINVAL);
    check("ipmi_destroy", kernel.Call(IpmiDestroyOp, first, &result, nullptr, 0) == 0 &&
                              result == 0 && kernel.HandleCount() == 2);
    check("ipmi_destroy_twice_ebadf",
          kernel.Call(IpmiDestroyOp, first, &result, nullptr, 0) == POSIX_EBADF);
}

} // namespace

int RunIpcSelfTest(const std::filesystem::path& report_directory) {
    auto& backend = NativeBootBackend::Instance();
    backend.Enable(report_directory);
    CheckLog check{backend};
    RunBasicChecks(check);
    RunDisconnectWakeCheck(check);
    RunConcurrencyCheck(check);
    RunIpmiKernelChecks(check);
    backend.Record("IPC", "summary", fmt::format("passed={} failed={}", check.passed, check.failed),
                   check.failed == 0 ? "pass" : "fail");
    backend.WriteReports();
    return check.failed == 0 ? 0 : 1;
}

namespace {

bool RunRenderCycle(NativeBootBackend& backend, u32 cycle) {
    const std::string tag = fmt::format("cycle {}", cycle);
    auto* controllers = Common::Singleton<Input::GameControllers>::Instance();
    auto window = std::make_unique<Frontend::WindowSDL>(1280, 720, controllers,
                                                        "PiStation synthetic render self-test");
    if (!window->GetSDLWindow() || !window->IsOpen()) {
        backend.RecordFailure("VULKAN", "window", tag + ": SDL window was not created");
        return false;
    }
    g_window = window.get();
    backend.Record("PISTATION_BOOT", "window_created", tag + ": SDL window 1280x720", "ok");

    bool ok = true;
    {
        auto liverpool = std::make_unique<AmdGpu::Liverpool>();
        auto presenter = std::make_unique<Vulkan::Presenter>(*window, liverpool.get());
        backend.Record("VULKAN", "renderer_initialized",
                       tag + ": instance, device, schedulers, swapchain and rasterizer constructed",
                       "ok");
        if (auto* frame = presenter->PrepareBlankFrame(false)) {
            presenter->Present(frame, false, false);
            backend.Record("FRAMEBUFFER", "synthetic_blank_frame_presented",
                           tag + ": blank frame submitted to the swapchain; not guest output", "ok");
        } else {
            backend.RecordFailure("FRAMEBUFFER", "prepare_blank_frame", tag + ": returned null");
            ok = false;
        }
        presenter.reset();
        backend.Record("VULKAN_SHUTDOWN", "presenter_destroyed", tag, "ok");
        liverpool.reset();
        backend.Record("VULKAN_SHUTDOWN", "liverpool_destroyed", tag, "ok");
    }
    g_window = nullptr;
    window.reset();
    backend.Record("PISTATION_BOOT", "window_destroyed", tag, "ok");
    return ok;
}

} // namespace

int RunRenderSelfTest(const std::filesystem::path& report_directory) {
    auto& backend = NativeBootBackend::Instance();
    backend.Enable(report_directory);
    Common::ElfInfo::Instance().InitializeSynthetic(Common::ElfInfo::FW_900);

    constexpr u32 Cycles = 2;
    for (u32 cycle = 0; cycle < Cycles; cycle++) {
        if (!RunRenderCycle(backend, cycle)) {
            backend.WriteReports();
            return 1;
        }
    }
    backend.Record("PISTATION_BOOT", "render_selftest", fmt::format("{} cycles completed", Cycles),
                   "ok");
    backend.WriteReports();
    return 0;
}

int RunModuleLoadProbe(const std::filesystem::path& module_path,
                       const std::filesystem::path& report_directory) {
    auto& backend = NativeBootBackend::Instance();
    backend.Enable(report_directory);

    std::error_code ec;
    if (!std::filesystem::is_regular_file(module_path, ec)) {
        backend.RecordFailure("PISTATION_BOOT", "module_not_found", "path is not a regular file");
        return 1;
    }
    std::array<u8, 20> header{};
    {
        std::ifstream in(module_path, std::ios::binary);
        in.read(reinterpret_cast<char*>(header.data()), header.size());
        if (in.gcount() != static_cast<std::streamsize>(header.size())) {
            backend.RecordFailure("ELF_LOADER", "header_read", "file shorter than an ELF header");
            return 1;
        }
    }
    if (header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F') {
        backend.RecordFailure("ELF_LOADER", "not_plain_elf",
                              "file is not a plain ELF; SELF or encrypted input is not loaded");
        return 1;
    }
    backend.ReachPhase(BootPhase::FileIdentified,
                       fmt::format("plain ELF header accepted, e_type={:#x}",
                                   static_cast<u32>(header[16] | (header[17] << 8))));

    Common::ElfInfo::Instance().InitializeSynthetic(Common::ElfInfo::FW_900);
    backend.SkipGraphicsHle();
    backend.Record("HLE", "graphics_hle", "libSceGnmDriver and libSceVideoOut not registered: no window or presenter in probe",
                   "imports of those libraries will be counted as unresolved");
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    Libraries::InitHLELibs(&linker->GetHLESymbols());
    backend.Record("HLE", "init_hle_libraries", "registered", "ok");

    const s32 id = linker->LoadModule(module_path, false);
    auto* module = linker->GetModule(id);
    if (id < 0 || !module || !module->IsValid()) {
        backend.RecordFailure("ELF_LOADER", "load_module", "Linker::LoadModule failed");
        return 1;
    }
    backend.ReachPhase(BootPhase::FileLoaded,
                       fmt::format("module '{}' mapped at {:#x}, size {:#x}", module->name,
                                   module->GetBaseAddress(), module->aligned_base_size));
    backend.ReachPhase(BootPhase::MemoryPrepared,
                       fmt::format("{} segment records, proc_param={}, tls image size {:#x}",
                                   module->info.num_segments,
                                   module->proc_param_virtual_addr != 0 ? "present" : "absent",
                                   module->tls.image_size));
    backend.Record("LINKER", "imports",
                   fmt::format("{} import modules, {} import libraries, entry {:#x}",
                               module->GetImportModules().size(), module->GetImportLibs().size(),
                               module->GetEntryAddress()),
                   "entry_address_computed_not_executed");

    linker->RelocateAllImports();
    backend.Record("LINKER", "relocate_all_imports", "completed", "ok");
    backend.WriteReports();
    return 0;
}

} // namespace PiStation
