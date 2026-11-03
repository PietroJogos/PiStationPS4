// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <mutex>
#include "common/types.h"

namespace Libraries::Ipmi {

constexpr s32 IpmiResultUnset = -1;
constexpr u64 IpmiCreateOp = 0x2;
constexpr u64 IpmiDestroyOp = 0x3;
constexpr size_t IpmiCreateArgsSize = 0x18;

// Kernel side of libSceIpmi's single system call. Calling convention as issued by the library:
// op, handle, s32* result (pre-set to -1), u64* args, args size in bytes. Success returns 0 with
// *result set; failure returns -1 with an errno and leaves *result untouched.
class IpmiKernelManager {
public:
    s32 Call(u64 op, s32 handle, s32* result, const u64* args, u64 args_size);

    size_t HandleCount() const;

private:
    mutable std::mutex mutex;
    std::map<s32, u64> objects;
    s32 next_handle{1};
};

IpmiKernelManager& GetIpmiKernelManager();

s32 PS4_SYSV_ABI ipmimgr_call(u64 op, s32 handle, s32* result, const u64* args, u64 args_size,
                              u64 sentinel);

} // namespace Libraries::Ipmi
