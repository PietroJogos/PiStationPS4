// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <string>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/libraries/ipmi/ipmimgr.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/memory.h"
#include "core/pistation/native_boot_backend.h"

namespace Libraries::Ipmi {

namespace {

std::string DescribeArgument(u64 value) {
    constexpr u64 MaxText = 64;
    auto* memory = Core::Memory::Instance();
    if (value < 0x10000 || !memory->IsValidMapping(value, MaxText)) {
        return fmt::format("{:#x}", value);
    }
    const auto* text = reinterpret_cast<const char*>(value);
    std::string out;
    for (u64 i = 0; i < MaxText && text[i] != '\0'; i++) {
        if (text[i] < 0x20 || text[i] > 0x7e) {
            return fmt::format("{:#x}", value);
        }
        out.push_back(text[i]);
    }
    return out.empty() ? fmt::format("{:#x}", value) : fmt::format("{:#x}:'{}'", value, out);
}

std::optional<std::string> ReadGuestString(u64 address) {
    constexpr u64 MaxText = 64;
    if (address < 0x10000 || !Core::Memory::Instance()->IsValidMapping(address, MaxText)) {
        return std::nullopt;
    }
    const auto* text = reinterpret_cast<const char*>(address);
    std::string out;
    for (u64 i = 0; i < MaxText && text[i] != '\0'; i++) {
        if (text[i] < 0x20 || text[i] > 0x7e) {
            return std::nullopt;
        }
        out.push_back(text[i]);
    }
    return out.empty() ? std::nullopt : std::optional{out};
}

std::string DescribeArguments(const u64* args, u64 args_size) {
    std::string out;
    if (!args) {
        return out;
    }
    for (u64 i = 0; i < args_size / sizeof(u64) && i < 6; i++) {
        out += (i ? " " : "") + DescribeArgument(args[i]);
    }
    return out;
}

struct OperationName {
    u64 op;
    const char* name;
};

constexpr OperationName OperationNames[] = {
    {0x0, "ServerImpl::initialize (Server::create)"},
    {0x1, "ServerImpl::destroy"},
    {0x2, "ClientImpl::initialize (Client::create)"},
    {0x3, "ClientImpl::destroy"},
    {0x4, "ServerImpl::createSession"},
    {0x5, "SessionImpl::destroy"},
    {0x202, "ServerImpl::shutdownDispatcher"},
    {0x232, "SessionImpl::respondToSyncMethodRequest"},
    {0x241, "ClientImpl::invokeAsyncMethod"},
    {0x242, "SessionImpl::respondToAsyncMethodRequest"},
    {0x243, "ClientImpl::tryGetResult"},
    {0x251, "ClientImpl::getMsg"},
    {0x252, "ClientImpl::tryGetMsg"},
    {0x253, "SessionImpl::sendMsg"},
    {0x254, "SessionImpl::trySendMsg"},
    {0x255, "SessionImpl::emptyMsgQueue"},
    {0x302, "SessionImpl::getClientPid"},
    {0x310, "ClientImpl::disconnect"},
    {0x320, "ClientImpl::invokeSyncMethod"},
    {0x456, "SessionImpl::getMsg"},
    {0x457, "SessionImpl::tryGetMsg"},
    {0x464, "SessionImpl::isPeerPrivileged"},
    {0x465, "SessionImpl::getServer"},
    {0x466, "ServerImpl::getUserData"},
    {0x467, "ClientImpl::getUserData"},
    {0x468, "SessionImpl::getUserData"},
    {0x490, "ClientImpl::waitEventFlag"},
    {0x491, "ClientImpl::pollEventFlag"},
    {0x493, "SessionImpl::setEventFlag"},
    {0x520, "ClientImpl::terminateConnection"},
};

} // namespace

const char* IpmiOperationName(u64 op) {
    for (const auto& entry : OperationNames) {
        if (entry.op == op) {
            return entry.name;
        }
    }
    return nullptr;
}

IpmiKernelManager::IpmiKernelManager(IpmiNameReader reader)
    : read_name{reader ? std::move(reader) : IpmiNameReader{ReadGuestString}} {}

std::optional<std::string> IpmiKernelManager::ServiceNameOf(s32 handle) const {
    std::scoped_lock lk{mutex};
    const auto it = objects.find(handle);
    if (it == objects.end() || it->second.service_name.empty()) {
        return std::nullopt;
    }
    return it->second.service_name;
}

s32 IpmiKernelManager::Call(u64 op, s32 handle, s32* result, const u64* args, u64 args_size) {
    if (!result) {
        return POSIX_EFAULT;
    }
    std::scoped_lock lk{mutex};
    switch (op) {
    case IpmiCreateOp: {
        if (args_size != IpmiCreateArgsSize || !args || args[1] == 0) {
            return POSIX_EINVAL;
        }
        const s32 id = next_handle++;
        objects[id] = {args[0], read_name(args[1]).value_or(std::string{})};
        *result = id;
        return 0;
    }
    case IpmiDestroyOp: {
        if (args_size != 0) {
            return POSIX_EINVAL;
        }
        if (objects.erase(handle) == 0) {
            return POSIX_EBADF;
        }
        *result = 0;
        return 0;
    }
    default:
        return POSIX_ENOSYS;
    }
}

size_t IpmiKernelManager::HandleCount() const {
    std::scoped_lock lk{mutex};
    return objects.size();
}

IpmiKernelManager& GetIpmiKernelManager() {
    static IpmiKernelManager manager;
    return manager;
}

s32 PS4_SYSV_ABI ipmimgr_call(u64 op, s32 handle, s32* result, const u64* args, u64 args_size,
                              u64 sentinel) {
    static std::atomic<u32> traced{0};
    auto& manager = GetIpmiKernelManager();
    const s32 error = manager.Call(op, handle, result, args, args_size);
    if (traced++ < 256) {
        const char* name = IpmiOperationName(op);
        const auto service = manager.ServiceNameOf(handle);
        const char* tag = op == 0x400 ? "IPMI_CONNECT"
                          : op <= IpmiDestroyOp ? "IPMI_CLIENT"
                                                : "IPMI";
        const char* outcome = error == 0 ? "ok"
                              : error == POSIX_ENOSYS
                                  ? "unimplemented_operation (host: no handler for this op)"
                                  : "rejected_arguments_or_handle";
        const std::string detail = fmt::format(
            "op={:#x}{} handle={} service={} args_size={:#x} args=[{}]", op,
            name ? fmt::format(" ({})", name) : std::string{}, handle,
            service ? *service : std::string{"-"}, args_size, DescribeArguments(args, args_size));
        LOG_INFO(Lib_Kernel, "[{}] {} -> {} (errno {})", tag, detail, outcome, error);
        PiStation::NativeBootBackend::Instance().Record(tag, "ipmimgr_call", detail,
                                                        fmt::format("{} errno {}", outcome, error));
    }
    if (error != 0) {
        *Libraries::Kernel::__Error() = error;
        return -1;
    }
    return 0;
}

} // namespace Libraries::Ipmi
