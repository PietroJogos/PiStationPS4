// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include "common/types.h"

namespace Libraries::Ipmi {

constexpr s32 IpmiResultUnset = -1;
constexpr u64 IpmiCreateOp = 0x2;
constexpr u64 IpmiDestroyOp = 0x3;
constexpr size_t IpmiCreateArgsSize = 0x18;

// Kernel side of libSceIpmi's single system call. Calling convention as issued by the library:
// op, handle, s32* result (pre-set to -1), u64* args, args size in bytes. Success returns 0 with
// *result set; failure returns -1 with an errno and leaves *result untouched.
using IpmiNameReader = std::function<std::optional<std::string>(u64 address)>;

// Name of an operation, taken from the exported C++ symbols of libSceIpmi that reach its wrapper.
// Operations that only the server dispatcher uses, or whose callers are unnamed, return nullptr.
const char* IpmiOperationName(u64 op);

class IpmiKernelManager {
public:
    explicit IpmiKernelManager(IpmiNameReader reader = {});

    std::optional<std::string> ServiceNameOf(s32 handle) const;

    s32 Call(u64 op, s32 handle, s32* result, const u64* args, u64 args_size);

    size_t HandleCount() const;

private:
    mutable std::mutex mutex;
    struct Object {
        u64 user_pointer{};
        std::string service_name;
    };
    IpmiNameReader read_name;
    std::map<s32, Object> objects;
    s32 next_handle{1};
};

IpmiKernelManager& GetIpmiKernelManager();

s32 PS4_SYSV_ABI ipmimgr_call(u64 op, s32 handle, s32* result, const u64* args, u64 args_size,
                              u64 sentinel);

} // namespace Libraries::Ipmi
