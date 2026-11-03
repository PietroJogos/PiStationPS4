// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright RPCSX contributors (kernel/orbis/src/ipmi.cpp, RPCSX/rpcsx)
// SPDX-License-Identifier: GPL-2.0-only
//
// Kernel side of IPMI (clients, servers, sessions, connection handshake, synchronous calls).
// The structure layouts, the packet types and the connection protocol are taken from the Orbis
// kernel of the RPCSX project (https://github.com/RPCSX/rpcsx, GPL-2.0), which reverse engineered
// them. The code was rewritten for this emulator: a single mutex and condition variable, shared
// pointers, and direct access to guest memory behind a validity check.

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <optional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>
#include "common/types.h"

namespace Libraries::Ipmi {

constexpr u64 OpCreateServer = 0x0;
constexpr u64 OpDestroyServer = 0x1;
constexpr u64 OpCreateClient = 0x2;
constexpr u64 OpDestroyClient = 0x3;
constexpr u64 OpCreateSession = 0x4;
constexpr u64 OpDestroySession = 0x5;
constexpr u64 OpTrace = 0x10;
constexpr u64 OpServerReceivePacket = 0x201;
constexpr u64 OpSendConnectResult = 0x212;
constexpr u64 OpSessionRespondSync = 0x232;
constexpr u64 OpSessionGetClientPid = 0x302;
constexpr u64 OpClientDisconnect = 0x310;
constexpr u64 OpClientInvokeSync = 0x320;
constexpr u64 OpClientConnect = 0x400;
constexpr u64 OpSessionGetUserData = 0x468;
constexpr u64 OpServerGetName = 0x46a;
constexpr u64 OpClientGetName = 0x46b;

constexpr u32 IpmiHostPid = 1;
constexpr size_t IpmiMaxNameLength = 24;

#pragma pack(push, 1)
struct IpmiCreateParams {
    u64 impl;
    u64 name;
    u64 config;
};
struct IpmiServerConfig {
    u64 size;
    u32 unk1, unk2, unk3, unk4, enable_multiple_server_threads, unk5;
    u64 unk6;
    u64 user_data;
    u64 event_handler;
};
struct IpmiClientConfig {
    u64 size;
    u32 unk[80];
    u64 user_data;
};
struct IpmiBufferInfo {
    u64 data;
    u64 capacity;
    u64 size;
};
struct IpmiDataInfo {
    u64 data;
    u64 size;
};
struct IpmiPacketInfo {
    u64 input_size;
    u32 type;
    u32 client_kid;
    u64 event_handler;
};
struct IpmiClientConnectParams {
    u64 user_data;
    u64 user_data_len;
    u64 status;
    u64 arg3;
};
struct IpmiSyncCallParams {
    u32 method;
    u32 num_in_data;
    u32 num_out_data;
    u32 unk;
    u64 in_data;
    u64 out_data;
    u64 result;
    u32 flags;
    u32 padding;
};
struct IpmiSyncMessageHeader {
    u64 session_impl;
    u32 pid;
    u32 method_id;
    u32 num_in_data;
    u32 num_out_data;
};
#pragma pack(pop)

static_assert(sizeof(IpmiCreateParams) == 0x18);
static_assert(sizeof(IpmiServerConfig) == 0x38);
static_assert(sizeof(IpmiClientConfig) == 0x150);
static_assert(sizeof(IpmiBufferInfo) == 0x18);
static_assert(sizeof(IpmiDataInfo) == 0x10);
static_assert(sizeof(IpmiPacketInfo) == 0x18);
static_assert(sizeof(IpmiClientConnectParams) == 0x20);
static_assert(sizeof(IpmiSyncCallParams) == 0x30);
static_assert(sizeof(IpmiSyncMessageHeader) == 0x18);

class IpmiKernel {
public:
    using Validator = std::function<bool(const void* address, size_t size)>;

    explicit IpmiKernel(Validator validator = {});
    ~IpmiKernel();

    // Executes one operation as the calling thread. Returns 0 and writes *result on success, or an
    // errno (FreeBSD numbering) on failure, in which case *result is left untouched.
    s32 Call(u64 op, s32 handle, s32* result, const void* params, u64 params_size);

    // Wakes every blocked call with an error and rejects further calls.
    void Shutdown();

    size_t ObjectCount() const;
    std::optional<std::string> NameOf(s32 handle) const;
    bool IsServerRegistered(const std::string& name) const;

private:
    struct Session;
    struct Server;
    struct Client;

    struct Packet {
        IpmiPacketInfo info;
        u32 client_tid{};
        std::shared_ptr<Session> session;
        std::vector<u8> message;
    };
    struct ConnectionRequest {
        std::shared_ptr<Client> client;
        u32 client_tid{};
        u32 server_tid{};
    };
    struct SyncResponse {
        s32 error_code{};
        u32 caller_tid{};
        std::vector<std::vector<u8>> data;
    };

    struct Server {
        std::string name;
        u64 impl{}, event_handler{}, user_data{};
        bool destroyed{};
        std::deque<Packet> packets;
        std::vector<ConnectionRequest> requests;
        std::map<u32, u32> tid_to_client_tid;
    };
    struct Client {
        std::string name;
        u64 impl{}, user_data{};
        std::shared_ptr<Session> session;
        std::optional<s32> connection_status;
        bool destroyed{};
    };
    struct Session {
        u64 impl{}, user_data{};
        std::shared_ptr<Client> client;
        std::shared_ptr<Server> server;
        std::deque<SyncResponse> sync_responses;
    };

    using Object = std::variant<std::shared_ptr<Client>, std::shared_ptr<Server>,
                                std::shared_ptr<Session>>;

    template <typename T>
    bool Read(const void* address, T& out) const;
    template <typename T>
    bool Write(u64 address, const T& value) const;
    bool ReadBytes(u64 address, void* out, size_t size) const;
    bool WriteBytes(u64 address, const void* data, size_t size) const;
    std::optional<std::string> ReadName(u64 address) const;
    template <typename T>
    std::shared_ptr<T> Get(s32 handle) const;
    s32 Insert(Object object);

    s32 CreateServer(s32* result, const void* params, u64 size);
    s32 CreateClient(s32* result, const void* params, u64 size);
    s32 CreateSession(s32* result, const void* params, u64 size);
    s32 ReceivePacket(std::unique_lock<std::mutex>& lk, s32 handle, s32* result, const void* params,
                      u64 size);
    s32 SendConnectResult(s32 handle, s32* result, const void* params, u64 size);
    s32 RespondSync(s32 handle, s32* result, const void* params, u64 size);
    s32 InvokeSync(std::unique_lock<std::mutex>& lk, s32 handle, s32* result, const void* params,
                   u64 size);
    s32 Connect(std::unique_lock<std::mutex>& lk, s32 handle, s32* result, const void* params,
                u64 size);

    Validator validator;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<s32, Object> objects;
    std::map<std::string, std::shared_ptr<Server>> servers;
    s32 next_handle{1};
    bool shutting_down{};
};

u32 IpmiCurrentTid();

} // namespace Libraries::Ipmi
