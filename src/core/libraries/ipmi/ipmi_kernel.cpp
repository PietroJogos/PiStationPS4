// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright RPCSX contributors (kernel/orbis/src/ipmi.cpp, RPCSX/rpcsx)
// SPDX-License-Identifier: GPL-2.0-only

#include <algorithm>
#include <atomic>
#include <cstring>
#include "core/libraries/ipmi/ipmi_kernel.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/memory.h"

namespace Libraries::Ipmi {

namespace {

constexpr u32 PacketConnect = 0x1;
constexpr u32 PacketSyncBase = 0x41;
constexpr u32 PacketSyncReplyWanted = 0x10;
constexpr u32 PacketSameProcess = 0x8000;
constexpr u64 MaxSyncDataCount = 32;
constexpr u64 MaxMessageSize = 0x10000;

struct ConnectMessageHeader {
    u32 client_pid;
    u32 client_kid;
    struct Queue {
        u32 max_outstanding;
        u32 unk;
        u64 in_data_size_hard_limit;
        u64 out_data_size_hard_limit;
    } sync, async;
    u32 num_event_flag;
    u32 unk1;
    u32 num_msg_queue;
    u32 unk2;
    u64 msg_queue_size[32];
    u64 memory_size;
};
static_assert(sizeof(ConnectMessageHeader) == 0x150);

bool DefaultValidator(const void* address, size_t size) {
    return Core::Memory::Instance()->IsValidMapping(reinterpret_cast<u64>(address), size);
}

} // namespace

u32 IpmiCurrentTid() {
    static std::atomic<u32> next{1};
    thread_local const u32 tid = next++;
    return tid;
}

IpmiKernel::IpmiKernel(Validator v) : validator{v ? std::move(v) : Validator{DefaultValidator}} {}

IpmiKernel::~IpmiKernel() {
    Shutdown();
}

void IpmiKernel::Shutdown() {
    std::scoped_lock lk{mutex};
    shutting_down = true;
    changed.notify_all();
}

size_t IpmiKernel::ObjectCount() const {
    std::scoped_lock lk{mutex};
    return objects.size();
}

bool IpmiKernel::IsServerRegistered(const std::string& name) const {
    std::scoped_lock lk{mutex};
    return servers.contains(name);
}

std::optional<std::string> IpmiKernel::NameOf(s32 handle) const {
    std::scoped_lock lk{mutex};
    const auto it = objects.find(handle);
    if (it == objects.end()) {
        return std::nullopt;
    }
    if (const auto* client = std::get_if<std::shared_ptr<Client>>(&it->second)) {
        return (*client)->name;
    }
    if (const auto* server = std::get_if<std::shared_ptr<Server>>(&it->second)) {
        return (*server)->name;
    }
    return std::nullopt;
}

bool IpmiKernel::ReadBytes(u64 address, void* out, size_t size) const {
    if (size == 0) {
        return true;
    }
    const void* source = reinterpret_cast<const void*>(address);
    if (address == 0 || !validator(source, size)) {
        return false;
    }
    std::memcpy(out, source, size);
    return true;
}

bool IpmiKernel::WriteBytes(u64 address, const void* data, size_t size) const {
    if (size == 0) {
        return true;
    }
    void* destination = reinterpret_cast<void*>(address);
    if (address == 0 || !validator(destination, size)) {
        return false;
    }
    std::memcpy(destination, data, size);
    return true;
}

template <typename T>
bool IpmiKernel::Read(const void* address, T& out) const {
    return ReadBytes(reinterpret_cast<u64>(address), &out, sizeof(T));
}

template <typename T>
bool IpmiKernel::Write(u64 address, const T& value) const {
    return WriteBytes(address, &value, sizeof(T));
}

std::optional<std::string> IpmiKernel::ReadName(u64 address) const {
    char text[IpmiMaxNameLength + 1]{};
    if (address == 0) {
        return std::nullopt;
    }
    for (size_t i = 0; i <= IpmiMaxNameLength; i++) {
        if (!ReadBytes(address + i, &text[i], 1)) {
            return std::nullopt;
        }
        if (text[i] == '\0') {
            return std::string{text};
        }
    }
    return std::nullopt;
}

template <typename T>
std::shared_ptr<T> IpmiKernel::Get(s32 handle) const {
    const auto it = objects.find(handle);
    if (it == objects.end()) {
        return nullptr;
    }
    if (const auto* ptr = std::get_if<std::shared_ptr<T>>(&it->second)) {
        return *ptr;
    }
    return nullptr;
}

s32 IpmiKernel::Insert(Object object) {
    const s32 handle = next_handle++;
    objects.emplace(handle, std::move(object));
    return handle;
}

s32 IpmiKernel::Call(u64 op, s32 handle, s32* result, const void* params, u64 params_size) {
    if (!result) {
        return POSIX_EFAULT;
    }
    std::unique_lock lk{mutex};
    if (shutting_down) {
        return POSIX_ECONNABORTED;
    }
    switch (op) {
    case OpCreateServer:
        return CreateServer(result, params, params_size);
    case OpCreateClient:
        return CreateClient(result, params, params_size);
    case OpCreateSession:
        return CreateSession(result, params, params_size);
    case OpDestroyServer: {
        auto server = Get<Server>(handle);
        if (!server) {
            return POSIX_EBADF;
        }
        server->destroyed = true;
        servers.erase(server->name);
        objects.erase(handle);
        changed.notify_all();
        *result = 0;
        return 0;
    }
    case OpDestroyClient: {
        auto client = Get<Client>(handle);
        if (!client) {
            return POSIX_EBADF;
        }
        client->destroyed = true;
        objects.erase(handle);
        changed.notify_all();
        *result = 0;
        return 0;
    }
    case OpDestroySession: {
        if (!Get<Session>(handle)) {
            return POSIX_EBADF;
        }
        objects.erase(handle);
        *result = 0;
        return 0;
    }
    case OpTrace:
        *result = 0;
        return 0;
    case OpServerReceivePacket:
        return ReceivePacket(lk, handle, result, params, params_size);
    case OpSendConnectResult:
        return SendConnectResult(handle, result, params, params_size);
    case OpSessionRespondSync:
        return RespondSync(handle, result, params, params_size);
    case OpClientInvokeSync:
        return InvokeSync(lk, handle, result, params, params_size);
    case OpClientConnect:
        return Connect(lk, handle, result, params, params_size);
    case OpClientDisconnect: {
        struct Args {
            u64 status;
        } args;
        if (params_size != sizeof(args) || !Get<Client>(handle)) {
            return POSIX_EINVAL;
        }
        if (!Read(params, args) || !Write<s32>(args.status, 0)) {
            return POSIX_EFAULT;
        }
        *result = 0;
        return 0;
    }
    case OpSessionGetClientPid: {
        struct Args {
            u64 pid;
        } args;
        if (params_size != sizeof(args) || !Get<Session>(handle)) {
            return POSIX_EINVAL;
        }
        if (!Read(params, args) || !Write<u32>(args.pid, IpmiHostPid)) {
            return POSIX_EFAULT;
        }
        *result = 0;
        return 0;
    }
    case OpSessionGetUserData: {
        struct Args {
            u64 data;
        } args;
        auto session = Get<Session>(handle);
        if (params_size != sizeof(args) || !session) {
            return POSIX_EINVAL;
        }
        if (!Read(params, args) || !Write<u64>(args.data, session->user_data)) {
            return POSIX_EFAULT;
        }
        *result = 0;
        return 0;
    }
    case OpServerGetName:
    case OpClientGetName: {
        struct Args {
            u64 name;
        } args;
        if (params_size != sizeof(args)) {
            return POSIX_EINVAL;
        }
        std::string name;
        if (op == OpServerGetName) {
            auto server = Get<Server>(handle);
            if (!server) {
                return POSIX_EINVAL;
            }
            name = server->name;
        } else {
            auto client = Get<Client>(handle);
            if (!client) {
                return POSIX_EINVAL;
            }
            name = client->name;
        }
        if (!Read(params, args) || !WriteBytes(args.name, name.c_str(), name.size() + 1)) {
            return POSIX_EFAULT;
        }
        *result = 0;
        return 0;
    }
    default:
        return POSIX_ENOSYS;
    }
}

s32 IpmiKernel::CreateServer(s32* result, const void* params, u64 size) {
    IpmiCreateParams p;
    IpmiServerConfig config;
    if (size != sizeof(p)) {
        return POSIX_EINVAL;
    }
    if (!Read(params, p) || !ReadBytes(p.config, &config, sizeof(config))) {
        return POSIX_EFAULT;
    }
    const auto name = ReadName(p.name);
    if (!name || name->empty()) {
        return POSIX_EFAULT;
    }
    if (servers.contains(*name)) {
        return POSIX_EEXIST;
    }
    auto server = std::make_shared<Server>();
    server->name = *name;
    server->impl = p.impl;
    server->user_data = config.user_data;
    server->event_handler = config.event_handler;
    servers.emplace(*name, server);
    *result = Insert(server);
    return 0;
}

s32 IpmiKernel::CreateClient(s32* result, const void* params, u64 size) {
    IpmiCreateParams p;
    IpmiClientConfig config;
    if (size != sizeof(p)) {
        return POSIX_EINVAL;
    }
    if (!Read(params, p) || !ReadBytes(p.config, &config, sizeof(config))) {
        return POSIX_EFAULT;
    }
    const auto name = ReadName(p.name);
    if (!name || name->empty()) {
        return POSIX_EFAULT;
    }
    auto client = std::make_shared<Client>();
    client->name = *name;
    client->impl = p.impl;
    client->user_data = config.user_data;
    *result = Insert(client);
    return 0;
}

s32 IpmiKernel::CreateSession(s32* result, const void* params, u64 size) {
    struct UserData {
        u64 size;
        u64 data;
    };
    struct Args {
        u64 session_impl;
        u64 user_data;
    } args;
    UserData user;
    if (size != sizeof(args)) {
        return POSIX_EINVAL;
    }
    if (!Read(params, args) || !ReadBytes(args.user_data, &user, sizeof(user))) {
        return POSIX_EFAULT;
    }
    if (user.size != sizeof(UserData)) {
        return POSIX_EINVAL;
    }
    const u32 tid = IpmiCurrentTid();
    for (auto& [name, server] : servers) {
        for (auto& request : server->requests) {
            if (request.server_tid != tid || request.client->session || request.client->destroyed) {
                continue;
            }
            auto session = std::make_shared<Session>();
            session->impl = args.session_impl;
            session->user_data = user.data;
            session->client = request.client;
            session->server = server;
            request.client->session = session;
            changed.notify_all();
            *result = Insert(session);
            return 0;
        }
    }
    return POSIX_EINVAL;
}

s32 IpmiKernel::ReceivePacket(std::unique_lock<std::mutex>& lk, s32 handle, s32* result,
                              const void* params, u64 size) {
    struct Args {
        u64 buffer;
        u64 buffer_size;
        u64 info;
        u64 unk;
    } args;
    if (size != sizeof(args)) {
        return POSIX_EINVAL;
    }
    auto server = Get<Server>(handle);
    if (!server) {
        return POSIX_EINVAL;
    }
    if (!Read(params, args)) {
        return POSIX_EFAULT;
    }
    changed.wait(lk, [&] { return shutting_down || server->destroyed || !server->packets.empty(); });
    if (shutting_down || server->destroyed) {
        return POSIX_ECONNABORTED;
    }
    Packet& front = server->packets.front();
    if (args.buffer_size < front.message.size()) {
        return POSIX_EINVAL;
    }
    Packet packet = std::move(front);
    server->packets.pop_front();

    const u32 tid = IpmiCurrentTid();
    if (packet.info.type == PacketConnect) {
        for (auto& request : server->requests) {
            if (request.client_tid == packet.info.input_size) {
                request.server_tid = tid;
                packet.info.input_size = 0;
                break;
            }
        }
    }
    server->tid_to_client_tid[tid] = packet.client_tid;

    packet.info.event_handler = server->event_handler;
    args.buffer_size = packet.message.size();
    if (!WriteBytes(args.buffer, packet.message.data(), packet.message.size()) ||
        !Write(args.info, packet.info) ||
        !WriteBytes(reinterpret_cast<u64>(params), &args, sizeof(args))) {
        return POSIX_EFAULT;
    }
    *result = 0;
    return 0;
}

s32 IpmiKernel::SendConnectResult(s32 handle, s32* result, const void* params, u64 size) {
    if (size != sizeof(s32)) {
        return POSIX_EINVAL;
    }
    s32 status;
    if (!Read(params, status)) {
        return POSIX_EFAULT;
    }
    std::shared_ptr<Client> client;
    if (auto session = Get<Session>(handle)) {
        client = session->client;
    } else if (auto direct = Get<Client>(handle)) {
        client = direct;
    } else if (auto server = Get<Server>(handle)) {
        const u32 tid = IpmiCurrentTid();
        for (auto& request : server->requests) {
            if (request.server_tid == tid) {
                client = request.client;
                break;
            }
        }
    }
    if (!client) {
        return POSIX_EINVAL;
    }
    client->connection_status = status;
    changed.notify_all();
    *result = 0;
    return 0;
}

s32 IpmiKernel::RespondSync(s32 handle, s32* result, const void* params, u64 size) {
    struct Args {
        s32 error_code;
        u32 buffer_count;
        u64 buffers;
        u32 flags;
        u32 padding;
    } args;
    if (size != sizeof(args)) {
        return POSIX_EINVAL;
    }
    auto session = Get<Session>(handle);
    if (!session || !session->server) {
        return POSIX_EINVAL;
    }
    if (!Read(params, args)) {
        return POSIX_EFAULT;
    }
    if (args.buffer_count > MaxSyncDataCount) {
        return POSIX_E2BIG;
    }
    std::vector<std::vector<u8>> buffers(args.buffer_count);
    for (u32 i = 0; i < args.buffer_count; i++) {
        IpmiBufferInfo info;
        if (!ReadBytes(args.buffers + i * sizeof(info), &info, sizeof(info))) {
            return POSIX_EFAULT;
        }
        if (info.size > MaxMessageSize) {
            return POSIX_E2BIG;
        }
        buffers[i].resize(info.size);
        if (!ReadBytes(info.data, buffers[i].data(), info.size)) {
            return POSIX_EFAULT;
        }
    }
    const auto tid_it = session->server->tid_to_client_tid.find(IpmiCurrentTid());
    if (tid_it == session->server->tid_to_client_tid.end()) {
        return POSIX_EINVAL;
    }
    session->sync_responses.push_front(
        {args.error_code, tid_it->second, std::move(buffers)});
    changed.notify_all();
    *result = 0;
    return 0;
}

s32 IpmiKernel::InvokeSync(std::unique_lock<std::mutex>& lk, s32 handle, s32* result,
                           const void* params, u64 size) {
    IpmiSyncCallParams p;
    if (size != sizeof(p)) {
        return POSIX_EINVAL;
    }
    if (!Read(params, p)) {
        return POSIX_EFAULT;
    }
    if (p.flags > 1) {
        return POSIX_EINVAL;
    }
    auto client = Get<Client>(handle);
    if (!client || !client->session || !client->session->server) {
        return POSIX_EINVAL;
    }
    if (p.num_in_data > MaxSyncDataCount || p.num_out_data > MaxSyncDataCount) {
        return POSIX_E2BIG;
    }
    auto session = client->session;
    auto server = session->server;

    std::vector<IpmiDataInfo> in_data(p.num_in_data);
    std::vector<IpmiBufferInfo> out_data(p.num_out_data);
    if (!ReadBytes(p.in_data, in_data.data(), in_data.size() * sizeof(IpmiDataInfo)) ||
        !ReadBytes(p.out_data, out_data.data(), out_data.size() * sizeof(IpmiBufferInfo))) {
        return POSIX_EFAULT;
    }
    u64 in_size = 0;
    for (const auto& data : in_data) {
        in_size += data.size;
    }
    if (in_size > MaxMessageSize) {
        return POSIX_E2BIG;
    }

    const size_t header_size =
        sizeof(IpmiSyncMessageHeader) + in_size + p.num_in_data * sizeof(u32);
    std::vector<u8> message(header_size + p.num_out_data * sizeof(u32));
    IpmiSyncMessageHeader header{};
    header.session_impl = session->impl;
    header.pid = IpmiHostPid;
    header.method_id = p.method;
    header.num_in_data = p.num_in_data;
    header.num_out_data = p.num_out_data;
    std::memcpy(message.data(), &header, sizeof(header));
    u8* cursor = message.data() + sizeof(header);
    for (const auto& data : in_data) {
        const u32 length = static_cast<u32>(data.size);
        std::memcpy(cursor, &length, sizeof(length));
        cursor += sizeof(length);
        if (!ReadBytes(data.data, cursor, data.size)) {
            return POSIX_EFAULT;
        }
        cursor += data.size;
    }
    for (const auto& data : out_data) {
        const u32 capacity = static_cast<u32>(data.capacity);
        std::memcpy(cursor, &capacity, sizeof(capacity));
        cursor += sizeof(capacity);
    }

    u32 type = PacketSyncBase;
    if ((p.flags & 1) == 0) {
        type |= PacketSyncReplyWanted;
    }
    type |= PacketSameProcess;

    const u32 tid = IpmiCurrentTid();
    server->packets.push_back(
        {{header_size, type, static_cast<u32>(handle), 0}, tid, session, std::move(message)});
    changed.notify_all();

    SyncResponse response;
    bool found = false;
    changed.wait(lk, [&] {
        if (shutting_down || server->destroyed) {
            return true;
        }
        const auto it = std::ranges::find(session->sync_responses, tid, &SyncResponse::caller_tid);
        if (it == session->sync_responses.end()) {
            return false;
        }
        response = std::move(*it);
        session->sync_responses.erase(it);
        found = true;
        return true;
    });
    if (!found) {
        return POSIX_ECONNABORTED;
    }

    if (!Write<s32>(p.result, response.error_code)) {
        return POSIX_EFAULT;
    }
    const size_t count = std::min<size_t>(response.data.size(), out_data.size());
    for (size_t i = 0; i < count; i++) {
        auto& info = out_data[i];
        const auto& data = response.data[i];
        if (info.capacity < data.size()) {
            continue;
        }
        info.size = data.size();
        if (!WriteBytes(info.data, data.data(), data.size()) ||
            !Write(p.out_data + i * sizeof(info), info)) {
            return POSIX_EFAULT;
        }
    }
    *result = 0;
    return 0;
}

s32 IpmiKernel::Connect(std::unique_lock<std::mutex>& lk, s32 handle, s32* result,
                        const void* params, u64 size) {
    IpmiClientConnectParams p;
    if (size != sizeof(p)) {
        return POSIX_EINVAL;
    }
    auto client = Get<Client>(handle);
    if (!client) {
        return POSIX_EINVAL;
    }
    if (client->session) {
        return POSIX_EEXIST;
    }
    if (!Read(params, p)) {
        return POSIX_EFAULT;
    }
    const auto server_it = servers.find(client->name);
    if (server_it == servers.end()) {
        return POSIX_ENOENT;
    }
    auto server = server_it->second;
    if (p.user_data_len > MaxMessageSize) {
        return POSIX_E2BIG;
    }
    for (const auto& request : server->requests) {
        if (request.client == client) {
            return POSIX_EEXIST;
        }
    }

    const u32 tid = IpmiCurrentTid();
    std::vector<u8> message(sizeof(ConnectMessageHeader) + sizeof(u32) +
                            std::max<u64>(p.user_data_len, 0x10));
    ConnectMessageHeader header{};
    header.client_pid = IpmiHostPid;
    header.client_kid = static_cast<u32>(handle);
    header.sync = {1, 0, 0x10000, 0x10000};
    header.async = {8, 0, 0x10000, 0x10000};
    header.num_event_flag = 32;
    header.num_msg_queue = 32;
    std::ranges::fill(header.msg_queue_size, 0x10000);
    std::memcpy(message.data(), &header, sizeof(header));
    if (p.user_data_len != 0) {
        const u32 length = static_cast<u32>(p.user_data_len);
        std::memcpy(message.data() + sizeof(header), &length, sizeof(length));
        if (!ReadBytes(p.user_data, message.data() + sizeof(header) + sizeof(length),
                       p.user_data_len)) {
            return POSIX_EFAULT;
        }
    }

    server->requests.push_back({client, tid, 0});
    server->packets.push_back(
        {{tid, PacketConnect, static_cast<u32>(handle), 0}, 0, nullptr, std::move(message)});
    changed.notify_all();

    changed.wait(lk, [&] {
        return shutting_down || server->destroyed || client->destroyed ||
               client->connection_status.has_value();
    });

    std::erase_if(server->requests, [&](const ConnectionRequest& r) { return r.client == client; });
    if (!client->connection_status) {
        return POSIX_ECONNABORTED;
    }
    if (!Write<s32>(p.status, *client->connection_status)) {
        return POSIX_EFAULT;
    }
    *result = 0;
    return 0;
}

} // namespace Libraries::Ipmi
