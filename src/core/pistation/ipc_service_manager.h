// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include "common/types.h"

namespace PiStation::Ipc {

enum class Result : u32 {
    Ok,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    LimitReached,
    QueueFull,
    MessageTooLarge,
    Timeout,
    Disconnected,
    ShuttingDown,
    NotOwner,
};

const char* ResultName(Result result);

struct Limits {
    size_t max_endpoints{64};
    size_t max_connections{256};
    size_t max_queue_depth{64};
    size_t max_message_size{0x10000};
    size_t trace_capacity{256};
};

struct Message {
    u32 method{};
    std::vector<u8> payload;
};

struct Request {
    u32 connection_id{};
    u32 client_id{};
    Message message;
};

struct TraceEntry {
    u64 sequence{};
    std::string operation;
    std::string endpoint;
    u32 connection_id{};
    Result result{};
};

class ServiceManager {
public:
    explicit ServiceManager(Limits limits = {});
    ~ServiceManager();

    ServiceManager(const ServiceManager&) = delete;
    ServiceManager& operator=(const ServiceManager&) = delete;

    Result RegisterEndpoint(const std::string& name, u32 owner_id);
    Result UnregisterEndpoint(const std::string& name, u32 owner_id);
    std::vector<std::string> ListEndpoints() const;

    Result Connect(const std::string& endpoint, u32 client_id, u32* connection_id);
    Result Disconnect(u32 connection_id);

    Result SendRequest(u32 connection_id, Message message);
    Result ReceiveRequest(const std::string& endpoint, u32 owner_id, Request* out,
                          std::chrono::milliseconds timeout);
    Result SendReply(u32 connection_id, u32 owner_id, std::span<const u8> payload);
    Result ReceiveReply(u32 connection_id, std::vector<u8>* out,
                        std::chrono::milliseconds timeout);

    void Shutdown();

    size_t ConnectionCount() const;
    std::vector<TraceEntry> Trace() const;

private:
    struct Endpoint {
        u32 owner_id{};
        std::deque<Request> requests;
    };
    struct Connection {
        std::string endpoint;
        u32 client_id{};
        std::deque<std::vector<u8>> replies;
    };

    Result Note(const char* operation, const std::string& endpoint, u32 connection_id,
                Result result);
    void CloseConnectionLocked(u32 connection_id);

    Limits limits;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string, Endpoint> endpoints;
    std::map<u32, Connection> connections;
    std::deque<TraceEntry> trace;
    u64 next_sequence{};
    u32 next_connection_id{1};
    bool shutting_down{};
};

class ScopedConnection {
public:
    ScopedConnection(ServiceManager& manager, const std::string& endpoint, u32 client_id);
    ~ScopedConnection();

    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;

    Result Status() const {
        return status;
    }
    u32 Id() const {
        return id;
    }

private:
    ServiceManager& manager;
    u32 id{};
    Result status;
};

} // namespace PiStation::Ipc
