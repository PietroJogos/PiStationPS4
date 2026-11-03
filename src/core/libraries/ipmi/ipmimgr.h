// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include "common/types.h"
#include "core/libraries/ipmi/ipmi_kernel.h"

namespace Libraries::Ipmi {

// Name of an operation. The first group comes from the exported symbols of libSceIpmi that reach
// the wrapper; ops 0x201, 0x212, 0x400, 0x463, 0x46a and 0x46b from the RPCSX kernel dispatcher.
const char* IpmiOperationName(u64 op);

// True for the operations the emulated kernel implements.
bool IpmiOperationImplemented(u64 op);

IpmiKernel& GetIpmiKernel();

// Single system call used by libSceIpmi: op, handle, s32* result (pre-set to -1), parameters and
// their size. Success returns 0 with *result set; failure returns -1 with errno set.
s32 PS4_SYSV_ABI ipmimgr_call(u64 op, s32 handle, s32* result, const void* params, u64 params_size,
                              u64 sentinel);

} // namespace Libraries::Ipmi
