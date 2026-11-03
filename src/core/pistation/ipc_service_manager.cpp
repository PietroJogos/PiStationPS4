// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include "core/pistation/ipc_service_manager.h"

namespace PiStation::Ipc {

const char* ResultName(Result result) {
    switch (result) {
    case Result::Ok:
        return "ok";
    case Result::InvalidArgument:
        return "invalid_argument";
    case Result::NotFound:
        return "not_found";
    case Result::AlreadyExists:
        return "already_exists";
    case Result::LimitReached:
        return "limit_reached";
    case Result::QueueFull:
        return "queue_full";
    case Result::MessageTooLarge:
        return "message_too_large";
    case Result::Timeout:
        return "timeout";
    case Result::Disconnected:
        return "disconnected";
    case Result::ShuttingDown:
        return "shutting_down";
    case Result::NotOwner:
        return "not_owner";
    }
    return "unknown";
}

ServiceManager::ServiceManager(Limits limits_) : limits{limits_} {}

ServiceManager::~ServiceManager() {
    Shutdown();
}

Result ServiceManager::Note(const char* operation, const std::string& endpoint, u32 connection_id,
                            Result result) {
    if (trace.size() >= limits.trace_capacity) {
        trace.pop_front();
    }
    trace.push_back({next_sequence++, operation, endpoint, connection_id, result});
    return result;
}

Result ServiceManager::RegisterEndpoint(const std::string& name, u32 owner_id) {
    std::scoped_lock lk{mutex};
    if (shutting_down) {
        return Note("register", name, 0, Result::ShuttingDown);
    }
    if (name.empty() || name.size() > 128) {
        return Note("register", name, 0, Result::InvalidArgument);
    }
    if (endpoints.contains(name)) {
        return Note("register", name, 0, Result::AlreadyExists);
    }
    if (endpoints.size() >= limits.max_endpoints) {
        return Note("register", name, 0, Result::LimitReached);
    }
    endpoints[name].owner_id = owner_id;
    return Note("register", name, 0, Result::Ok);
}

void ServiceManager::CloseConnectionLocked(u32 connection_id) {
    connections.erase(connection_id);
}

Result ServiceManager::UnregisterEndpoint(const std::string& name, u32 owner_id) {
    std::scoped_lock lk{mutex};
    const auto it = endpoints.find(name);
    if (it == endpoints.end()) {
        return Note("unregister", name, 0, Result::NotFound);
    }
    if (it->second.owner_id != owner_id) {
        return Note("unregister", name, 0, Result::NotOwner);
    }
    std::erase_if(connections, [&name](const auto& entry) { return entry.second.endpoint == name; });
    endpoints.erase(it);
    changed.notify_all();
    return Note("unregister", name, 0, Result::Ok);
}

std::vector<std::string> ServiceManager::ListEndpoints() const {
    std::scoped_lock lk{mutex};
    std::vector<std::string> names;
    for (const auto& [name, endpoint] : endpoints) {
        names.push_back(name);
    }
    return names;
}

Result ServiceManager::Connect(const std::string& endpoint, u32 client_id, u32* connection_id) {
    std::scoped_lock lk{mutex};
    if (!connection_id) {
        return Note("connect", endpoint, 0, Result::InvalidArgument);
    }
    *connection_id = 0;
    if (shutting_down) {
        return Note("connect", endpoint, 0, Result::ShuttingDown);
    }
    if (!endpoints.contains(endpoint)) {
        return Note("connect", endpoint, 0, Result::NotFound);
    }
    if (connections.size() >= limits.max_connections) {
        return Note("connect", endpoint, 0, Result::LimitReached);
    }
    const u32 id = next_connection_id++;
    connections[id] = {endpoint, client_id, {}};
    *connection_id = id;
    return Note("connect", endpoint, id, Result::Ok);
}

Result ServiceManager::Disconnect(u32 connection_id) {
    std::scoped_lock lk{mutex};
    const auto it = connections.find(connection_id);
    if (it == connections.end()) {
        return Note("disconnect", {}, connection_id, Result::NotFound);
    }
    const std::string endpoint = it->second.endpoint;
    if (const auto ep = endpoints.find(endpoint); ep != endpoints.end()) {
        std::erase_if(ep->second.requests,
                      [connection_id](const Request& r) { return r.connection_id == connection_id; });
    }
    CloseConnectionLocked(connection_id);
    changed.notify_all();
    return Note("disconnect", endpoint, connection_id, Result::Ok);
}

Result ServiceManager::SendRequest(u32 connection_id, Message message) {
    std::scoped_lock lk{mutex};
    if (shutting_down) {
        return Note("send_request", {}, connection_id, Result::ShuttingDown);
    }
    const auto conn = connections.find(connection_id);
    if (conn == connections.end()) {
        return Note("send_request", {}, connection_id, Result::Disconnected);
    }
    const std::string& name = conn->second.endpoint;
    if (message.payload.size() > limits.max_message_size) {
        return Note("send_request", name, connection_id, Result::MessageTooLarge);
    }
    const auto ep = endpoints.find(name);
    if (ep == endpoints.end()) {
        return Note("send_request", name, connection_id, Result::Disconnected);
    }
    if (ep->second.requests.size() >= limits.max_queue_depth) {
        return Note("send_request", name, connection_id, Result::QueueFull);
    }
    ep->second.requests.push_back({connection_id, conn->second.client_id, std::move(message)});
    changed.notify_all();
    return Note("send_request", name, connection_id, Result::Ok);
}

Result ServiceManager::ReceiveRequest(const std::string& endpoint, u32 owner_id, Request* out,
                                      std::chrono::milliseconds timeout) {
    std::unique_lock lk{mutex};
    if (!out) {
        return Note("receive_request", endpoint, 0, Result::InvalidArgument);
    }
    const auto ready = [&] {
        const auto ep = endpoints.find(endpoint);
        return shutting_down || ep == endpoints.end() || !ep->second.requests.empty();
    };
    if (!changed.wait_for(lk, timeout, ready)) {
        return Note("receive_request", endpoint, 0, Result::Timeout);
    }
    if (shutting_down) {
        return Note("receive_request", endpoint, 0, Result::ShuttingDown);
    }
    const auto ep = endpoints.find(endpoint);
    if (ep == endpoints.end()) {
        return Note("receive_request", endpoint, 0, Result::NotFound);
    }
    if (ep->second.owner_id != owner_id) {
        return Note("receive_request", endpoint, 0, Result::NotOwner);
    }
    *out = std::move(ep->second.requests.front());
    ep->second.requests.pop_front();
    return Note("receive_request", endpoint, out->connection_id, Result::Ok);
}

Result ServiceManager::SendReply(u32 connection_id, u32 owner_id, std::span<const u8> payload) {
    std::scoped_lock lk{mutex};
    const auto conn = connections.find(connection_id);
    if (conn == connections.end()) {
        return Note("send_reply", {}, connection_id, Result::Disconnected);
    }
    const std::string& name = conn->second.endpoint;
    const auto ep = endpoints.find(name);
    if (ep == endpoints.end()) {
        return Note("send_reply", name, connection_id, Result::NotFound);
    }
    if (ep->second.owner_id != owner_id) {
        return Note("send_reply", name, connection_id, Result::NotOwner);
    }
    if (payload.size() > limits.max_message_size) {
        return Note("send_reply", name, connection_id, Result::MessageTooLarge);
    }
    if (conn->second.replies.size() >= limits.max_queue_depth) {
        return Note("send_reply", name, connection_id, Result::QueueFull);
    }
    conn->second.replies.emplace_back(payload.begin(), payload.end());
    changed.notify_all();
    return Note("send_reply", name, connection_id, Result::Ok);
}

Result ServiceManager::ReceiveReply(u32 connection_id, std::vector<u8>* out,
                                    std::chrono::milliseconds timeout) {
    std::unique_lock lk{mutex};
    if (!out) {
        return Note("receive_reply", {}, connection_id, Result::InvalidArgument);
    }
    const auto ready = [&] {
        const auto conn = connections.find(connection_id);
        return shutting_down || conn == connections.end() || !conn->second.replies.empty();
    };
    if (!changed.wait_for(lk, timeout, ready)) {
        return Note("receive_reply", {}, connection_id, Result::Timeout);
    }
    const auto conn = connections.find(connection_id);
    if (conn != connections.end() && !conn->second.replies.empty()) {
        *out = std::move(conn->second.replies.front());
        conn->second.replies.pop_front();
        return Note("receive_reply", conn->second.endpoint, connection_id, Result::Ok);
    }
    return Note("receive_reply", {}, connection_id,
                shutting_down ? Result::ShuttingDown : Result::Disconnected);
}

void ServiceManager::Shutdown() {
    std::scoped_lock lk{mutex};
    if (shutting_down) {
        return;
    }
    shutting_down = true;
    connections.clear();
    endpoints.clear();
    changed.notify_all();
}

size_t ServiceManager::ConnectionCount() const {
    std::scoped_lock lk{mutex};
    return connections.size();
}

std::vector<TraceEntry> ServiceManager::Trace() const {
    std::scoped_lock lk{mutex};
    return {trace.begin(), trace.end()};
}

ScopedConnection::ScopedConnection(ServiceManager& manager_, const std::string& endpoint,
                                   u32 client_id)
    : manager{manager_}, status{manager.Connect(endpoint, client_id, &id)} {}

ScopedConnection::~ScopedConnection() {
    if (status == Result::Ok) {
        manager.Disconnect(id);
    }
}

} // namespace PiStation::Ipc
