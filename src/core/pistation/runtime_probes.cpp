// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstring>
#include <array>
#include <vector>
#include <string>
#include <chrono>
#include <fstream>
#include <map>
#include <thread>
#include <fmt/format.h>
#include "common/elf_info.h"
#include "common/singleton.h"
#include "core/libraries/ipmi/ipmi_kernel.h"
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

struct IpmiHarness {
    Libraries::Ipmi::IpmiKernel kernel{[](const void*, size_t) { return true; }};

    template <typename T>
    s32 Call(u64 op, s32 handle, s32& result, T& params) {
        return kernel.Call(op, handle, &result, &params, sizeof(T));
    }
    s32 CallRaw(u64 op, s32 handle, s32& result, const void* params, u64 size) {
        return kernel.Call(op, handle, &result, params, size);
    }
};

s32 IpmiCreateServer(IpmiHarness& h, const char* name, u64 impl, s32& handle) {
    using namespace Libraries::Ipmi;
    IpmiServerConfig config{};
    config.size = sizeof(config);
    IpmiCreateParams params{impl, reinterpret_cast<u64>(name), reinterpret_cast<u64>(&config)};
    handle = -1;
    return h.Call(OpCreateServer, 0, handle, params);
}

s32 IpmiCreateClient(IpmiHarness& h, const char* name, s32& handle) {
    using namespace Libraries::Ipmi;
    IpmiClientConfig config{};
    config.size = sizeof(config);
    IpmiCreateParams params{0x1000, reinterpret_cast<u64>(name), reinterpret_cast<u64>(&config)};
    handle = -1;
    return h.Call(OpCreateClient, 0, handle, params);
}

// Serves one endpoint: accepts connections and answers synchronous calls by echoing the first
// input buffer prefixed with "pong:". It uses only the kernel ABI, so every message really goes
// through the kernel's packet queue.
void IpmiServeLoop(IpmiHarness& h, s32 server_handle, std::atomic<bool>& stop,
                   std::atomic<u32>& connections, std::atomic<u32>& calls) {
    using namespace Libraries::Ipmi;
    std::map<u64, s32> sessions;
    u64 next_impl = 0x2000;
    while (!stop) {
        std::array<u8, 0x400> buffer{};
        IpmiPacketInfo info{};
        struct Args {
            u64 buffer, size, info, unk;
        } args{reinterpret_cast<u64>(buffer.data()), buffer.size(), reinterpret_cast<u64>(&info), 0};
        s32 result = -1;
        if (h.Call(OpServerReceivePacket, server_handle, result, args) != 0) {
            return;
        }
        if (info.type == 1) {
            struct UserData {
                u64 size, data;
            } user{sizeof(UserData), 0};
            struct SessionArgs {
                u64 impl, user;
            } session_args{next_impl, reinterpret_cast<u64>(&user)};
            s32 session = -1;
            if (h.Call(OpCreateSession, 0, session, session_args) != 0) {
                return;
            }
            sessions[next_impl++] = session;
            s32 status = 0;
            s32 ignored = -1;
            h.CallRaw(OpSendConnectResult, session, ignored, &status, sizeof(status));
            connections++;
            continue;
        }
        IpmiSyncMessageHeader header;
        std::memcpy(&header, buffer.data(), sizeof(header));
        u32 in_length;
        std::memcpy(&in_length, buffer.data() + sizeof(header), sizeof(in_length));
        std::string reply = "pong:";
        reply.append(reinterpret_cast<const char*>(buffer.data() + sizeof(header) + 4), in_length);
        IpmiBufferInfo out{reinterpret_cast<u64>(reply.data()), reply.size(), reply.size()};
        struct Respond {
            s32 error_code;
            u32 count;
            u64 buffers;
            u32 flags, padding;
        } respond{0, 1, reinterpret_cast<u64>(&out), 0, 0};
        s32 ignored = -1;
        h.Call(OpSessionRespondSync, sessions.at(header.session_impl), ignored, respond);
        calls++;
    }
}

struct ClientSession {
    s32 handle{-1};
    s32 connect_status{-1};
};

s32 IpmiConnect(IpmiHarness& h, ClientSession& client) {
    using namespace Libraries::Ipmi;
    IpmiClientConnectParams params{};
    params.status = reinterpret_cast<u64>(&client.connect_status);
    s32 result = -1;
    return h.Call(OpClientConnect, client.handle, result, params);
}

s32 IpmiInvoke(IpmiHarness& h, s32 client_handle, const std::string& input, std::string& output,
               s32& method_result) {
    using namespace Libraries::Ipmi;
    IpmiDataInfo in{reinterpret_cast<u64>(input.data()), input.size()};
    std::array<char, 64> out_storage{};
    IpmiBufferInfo out{reinterpret_cast<u64>(out_storage.data()), out_storage.size(), 0};
    IpmiSyncCallParams params{};
    params.method = 7;
    params.num_in_data = 1;
    params.num_out_data = 1;
    params.in_data = reinterpret_cast<u64>(&in);
    params.out_data = reinterpret_cast<u64>(&out);
    params.result = reinterpret_cast<u64>(&method_result);
    params.flags = 0;
    s32 result = -1;
    const s32 error = h.Call(OpClientInvokeSync, client_handle, result, params);
    output.assign(out_storage.data(), out.size);
    return error;
}

void RunIpmiKernelChecks(CheckLog& check) {
    using namespace Libraries::Ipmi;
    {
        IpmiHarness h;
        s32 server = -1;
        s32 client = -1;
        check("ipmi_absent_server_connect_enoent", [&] {
            IpmiCreateClient(h, "NoSuchService", client);
            ClientSession c{client};
            return IpmiConnect(h, c) == POSIX_ENOENT && c.connect_status == -1;
        }());
        check("ipmi_create_server", IpmiCreateServer(h, "SyntheticIpmi", 0x1, server) == 0 &&
                                        server > 0 && h.kernel.IsServerRegistered("SyntheticIpmi"));
        s32 duplicate = -1;
        check("ipmi_duplicate_server_rejected",
              IpmiCreateServer(h, "SyntheticIpmi", 0x2, duplicate) == POSIX_EEXIST);
        check("ipmi_server_name", [&] {
            char name[32]{};
            struct Args {
                u64 name;
            } args{reinterpret_cast<u64>(name)};
            s32 result = -1;
            return h.Call(OpServerGetName, server, result, args) == 0 &&
                   std::string{name} == "SyntheticIpmi";
        }());
        check("ipmi_unreadable_name_efault", [&] {
            IpmiClientConfig config{};
            IpmiCreateParams params{0x1000, 0, reinterpret_cast<u64>(&config)};
            s32 result = -1;
            return h.Call(OpCreateClient, 0, result, params) == POSIX_EFAULT && result == -1;
        }());
        check("ipmi_wrong_params_size_einval", [&] {
            u64 dummy[2]{};
            s32 result = -1;
            return h.CallRaw(OpCreateClient, 0, result, dummy, sizeof(dummy)) == POSIX_EINVAL;
        }());
        check("ipmi_invalid_handle_einval", [&] {
            ClientSession c{9999};
            return IpmiConnect(h, c) == POSIX_EINVAL;
        }());
        check("ipmi_unported_op_enosys", [&] {
            u64 dummy[4]{};
            s32 result = -1;
            return h.CallRaw(0x251, client, result, dummy, 0x28) == POSIX_ENOSYS;
        }());
    }

    {
        IpmiHarness h;
        s32 server = -1;
        IpmiCreateServer(h, "SyntheticIpmi", 0x1, server);
        std::atomic<bool> stop{false};
        std::atomic<u32> connections{0};
        std::atomic<u32> calls{0};
        std::thread serve{[&] { IpmiServeLoop(h, server, stop, connections, calls); }};

        constexpr u32 ClientCount = 4;
        constexpr u32 CallsPerClient = 25;
        std::atomic<u32> good{0};
        std::atomic<u32> bad{0};
        std::vector<std::thread> clients;
        for (u32 i = 0; i < ClientCount; i++) {
            clients.emplace_back([&, i] {
                s32 handle = -1;
                ClientSession c;
                if (IpmiCreateClient(h, "SyntheticIpmi", handle) != 0) {
                    bad += CallsPerClient;
                    return;
                }
                c.handle = handle;
                if (IpmiConnect(h, c) != 0 || c.connect_status != 0) {
                    bad += CallsPerClient;
                    return;
                }
                for (u32 n = 0; n < CallsPerClient; n++) {
                    const std::string input = fmt::format("c{}-{}", i, n);
                    std::string output;
                    s32 method_result = -1;
                    if (IpmiInvoke(h, handle, input, output, method_result) == 0 &&
                        method_result == 0 && output == "pong:" + input) {
                        good++;
                    } else {
                        bad++;
                    }
                }
            });
        }
        for (auto& thread : clients) {
            thread.join();
        }
        check("ipmi_connect_handshake_through_queue", connections == ClientCount,
              fmt::format("connections={}", connections.load()));
        check("ipmi_sync_calls_roundtrip", good == ClientCount * CallsPerClient && bad == 0,
              fmt::format("good={} bad={}", good.load(), bad.load()));
        check("ipmi_server_saw_every_call", calls == ClientCount * CallsPerClient);

        stop = true;
        s32 result = -1;
        h.kernel.Shutdown();
        serve.join();
        check("ipmi_shutdown_stops_server_loop",
              h.CallRaw(OpTrace, 0, result, nullptr, 0) == POSIX_ECONNABORTED);
    }

    {
        IpmiHarness h;
        s32 server = -1;
        s32 client = -1;
        IpmiCreateServer(h, "NobodyServes", 0x1, server);
        IpmiCreateClient(h, "NobodyServes", client);
        std::atomic<s32> outcome{-12345};
        std::thread connecting{[&] {
            ClientSession c{client};
            outcome = IpmiConnect(h, c);
        }};
        std::this_thread::sleep_for(100ms);
        check("ipmi_connect_blocks_until_server_answers", outcome == -12345);
        h.kernel.Shutdown();
        connecting.join();
        check("ipmi_shutdown_wakes_blocked_connect", outcome == POSIX_ECONNABORTED);
    }
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
