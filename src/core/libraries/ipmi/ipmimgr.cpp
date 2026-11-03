// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-only

#include <atomic>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/libraries/ipmi/ipmimgr.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/pistation/native_boot_backend.h"

namespace Libraries::Ipmi {

namespace {

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
    {0x201, "ServerImpl::receivePacket (dispatcher)"},
    {0x202, "ServerImpl::shutdownDispatcher"},
    {0x212, "ServerImpl::sendConnectResult (dispatcher)"},
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
    {0x400, "ClientImpl::connect"},
    {0x463, "SessionImpl::getClientAppId"},
    {0x464, "SessionImpl::isPeerPrivileged"},
    {0x465, "SessionImpl::getServer"},
    {0x466, "ServerImpl::getUserData"},
    {0x467, "ClientImpl::getUserData"},
    {0x468, "SessionImpl::getUserData"},
    {0x46a, "ServerImpl::getName"},
    {0x46b, "ClientImpl::getName"},
    {0x490, "ClientImpl::waitEventFlag"},
    {0x491, "ClientImpl::pollEventFlag"},
    {0x493, "SessionImpl::setEventFlag"},
    {0x520, "ClientImpl::terminateConnection"},
};


constexpr u64 ImplementedOps[] = {
    OpCreateServer,        OpDestroyServer,    OpCreateClient,      OpDestroyClient,
    OpCreateSession,       OpDestroySession,   OpTrace,             OpServerReceivePacket,
    OpSendConnectResult,   OpSessionRespondSync, OpSessionGetClientPid, OpClientDisconnect,
    OpClientInvokeSync,    OpClientConnect,    OpSessionGetUserData, OpServerGetName,
    OpClientGetName,
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

bool IpmiOperationImplemented(u64 op) {
    for (const u64 implemented : ImplementedOps) {
        if (implemented == op) {
            return true;
        }
    }
    return false;
}

IpmiKernel& GetIpmiKernel() {
    static IpmiKernel kernel;
    return kernel;
}

s32 PS4_SYSV_ABI ipmimgr_call(u64 op, s32 handle, s32* result, const void* params, u64 params_size,
                              u64 sentinel) {
    static std::atomic<u32> traced{0};
    auto& kernel = GetIpmiKernel();
    const s32 error = kernel.Call(op, handle, result, params, params_size);
    if (traced++ < 256) {
        const char* name = IpmiOperationName(op);
        const auto service = kernel.NameOf(error == 0 && result && op <= OpCreateClient ? *result : handle);
        const char* tag = op == OpClientConnect                      ? "IPMI_CONNECT"
                          : op == OpCreateClient || op == OpDestroyClient ? "IPMI_CLIENT"
                          : op == OpCreateServer || op == OpDestroyServer ? "IPMI_SERVER"
                                                                          : "IPMI";
        const char* outcome = error == 0 ? "ok"
                              : error == POSIX_ENOSYS
                                  ? "unimplemented_operation (host: not ported)"
                              : error == POSIX_ENOENT && op == OpClientConnect
                                  ? "server_not_registered"
                                  : "rejected";
        const std::string detail = fmt::format("op={:#x}{} handle={} service={} size={:#x}", op,
                                               name ? fmt::format(" ({})", name) : std::string{},
                                               handle, service ? *service : std::string{"-"},
                                               params_size);
        LOG_INFO(Lib_Kernel, "[{}] {} -> {} (errno {})", tag, detail, outcome, error);
        PiStation::NativeBootBackend::Instance().Record(
            tag, "ipmimgr_call", detail, fmt::format("{} errno {}", outcome, error));
    }
    if (error != 0) {
        *Libraries::Kernel::__Error() = error;
        return -1;
    }
    return 0;
}

} // namespace Libraries::Ipmi
