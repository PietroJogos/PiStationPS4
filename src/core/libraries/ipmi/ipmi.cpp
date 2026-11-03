// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/ipmi/ipmi.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/pistation/native_boot_backend.h"

namespace Libraries::Ipmi {

// IPMI::Client::create(IPMI::Client** out, const IPMI::Client::Config* config, void* arg3,
// void* memory). Callers treat a negative result as failure and, on success, call through the
// returned object's vtable. The object layout and the server side are not known, so no client is
// ever produced and the call reports ENOSYS instead of a success with a null client.
s32 PS4_SYSV_ABI IpmiClientCreate(void** out, const void* config, void* arg3, void* memory) {
    if (out) {
        *out = nullptr;
    }
    LOG_ERROR(Lib_Kernel, "[IPMI] IPMI::Client::create unimplemented: no client object produced");
    PiStation::NativeBootBackend::Instance().RecordFailure(
        "IPMI", "IPMI::Client::create",
        "unimplemented: client object layout and IPMI server are not emulated");
    return ORBIS_KERNEL_ERROR_ENOSYS;
}

// IPMI::Server::create(IPMI::Server** out, const IPMI::Server::Config* config, void* arg3,
// void* memory)
s32 PS4_SYSV_ABI IpmiServerCreate(void** out, const void* config, void* arg3, void* memory) {
    if (out) {
        *out = nullptr;
    }
    LOG_ERROR(Lib_Kernel, "[IPMI] IPMI::Server::create unimplemented: no server object produced");
    PiStation::NativeBootBackend::Instance().RecordFailure(
        "IPMI", "IPMI::Server::create",
        "unimplemented: server object layout and dispatch are not emulated");
    return ORBIS_KERNEL_ERROR_ENOSYS;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("0zsTiDhM0nU", "libSceIpmi", 1, "libSceIpmi", IpmiClientCreate);
    LIB_FUNCTION("hoWSsRT0ktA", "libSceIpmi", 1, "libSceIpmi", IpmiServerCreate);
}

} // namespace Libraries::Ipmi
