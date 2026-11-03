# Phase 9 — Boot progress, IPMI kernel contract, graphics state

Based on shadPS4 (GPL-2.0-or-later). Windows only. Logs and reports named here are local, untracked files under `build-emulator/local-validation/phase9/`; nothing proprietary is in the repository.

Status labels: **C** = confirmed by test or run, **P** = partially implemented, **B** = blocked, **N** = not tested / not done.

## 1. Confirmed state of Phase 8 (C)

Branch `feature/ps4-mono-assembly-loading` (`bc524609`) was re-run. With the Mono/Psm modules loaded as custom modules and `--mount-ro <firmware>/system/common/lib /sys/common/lib`, ShellUI reaches `initializeSystem`, prints `[SceMorpheusUpdClient] Initialize: create client failed: 0x8002004e` and `init_system.cpp(665) initializeSystem: LX_ISOK2::0x8002004e`, then the guest aborts (`shellui-hle-mono`, exit `0x80000003`). The `mscorlib.dll` failure from Phase 7 does not occur. That is the current first blocker: ShellUI cannot create an `IPMI::Client`.

## 2. New finding: libSceIpmi runs as LLE and its kernel contract is small (C)

The firmware contains a real `libSceIpmi.sprx`. The load probe (no execution) shows it imports only three non-libc symbols from libkernel: `ipmimgr_call`, `get_self_auth_info` and `sceKernelGetUtokenUseSoftwagnerForAcmgr`, plus basic libc. So the user-space IPMI library is self-contained, and everything it needs from the system goes through one kernel multiplexer, `ipmimgr_call`.

Change: `ipmimgr_call` (NID `Hk7iHmGxB18`, `libkernel`) is now registered in `src/core/libraries/kernel/process.cpp`. It traces its six integer arguments (first 256 calls, also recorded in the JSON report) and returns `ORBIS_KERNEL_ERROR_ENOSYS`. It does not pretend to be an IPMI server.

Experiment: `libSceIpmi.sprx` copied to `sys_modules/NPXS20001/` (makes the existing HLE yield to it, the `LLEd` message in the log) and to `custom_modules/NPXS20001/` (so it is loaded and ShellUI is re-relocated against it). The real library then called, in order (`phase9/ipmi-lle-run`):

| # | arg1 (op) | arg2 | arg3 | arg4 | arg5 | arg6 |
|---|---|---|---|---|---|---|
| 0 | 0x2 | 0 | ptr | ptr (arg3 - 0x1c) | 0x18 | 0xdeadbadecafebeaf |
| 1 | 0x400 | 0 | ptr | ptr (arg3 - 0x24) | 0x20 | 0xdeadbadecafebeaf |
| 2, 3 | same pair again on another thread | | | | | |
| 4 | 0x3 | 0 | ptr | 0 | 0 | 0xdeadbadecafebeaf |

Reading (inference, not verified): arg1 is an operation code, arg4 points to an input structure whose size is arg5, and arg3 points just past it, probably a result or output field. `0xdeadbadecafebeaf` looks like a filler for unused argument registers. What operations 0x2, 0x400 and 0x3 mean in the kernel is not known, so no server was implemented.

Result of that run: after these calls fail, ShellUI no longer aborts in `initializeSystem`. A different thread then dereferences a null IPMI client object (`mov rax, [rdi]; ... call [rax+0x58]` in `eboot.bin`, the vtable slot used with selector `0x160009`), faults with SIGSEGV, and Mono's own signal handler aborts with `ud2` (`libmonosgen` + `0xd2533`, exit `0xc000001d`). So with the LLE route the failure just moves to unchecked client use. Both routes end at the same dependency: IPMI must work.

The previous HLE `IPMI::Client::create`/`Server::create` (Phase 7) are unchanged and remain the default.

## 3. Items not done, with reasons

- **IPMI kernel semantics (B).** Needs reverse engineering of what ops 0x2/0x400/0x3 do in the kernel and of the server side, which lives in ShellCore or other system processes. Nothing in the evidence so far defines the reply formats, so I did not invent them. A server cannot be emulated before ShellCore can run.
- **ShellCore execution (B).** Unchanged from Phase 7: loads and relocates, 2074 imports stub-resolved, never executed. Running it needs per-process state.
- **`PiStationProcessContext` / multiprocess (N).** Not implemented. `Linker`, `MemoryManager`, `ElfInfo`, handle tables and HLE tables are process-wide singletons, so no honest isolation test can be written yet. Plan: (1) move module list, TLS state and handle tables behind a context pointer reachable from the current guest thread; (2) give each context its own address-space window; (3) only then add the isolation test. This is a large refactor and was not started.
- **Module unload and the `SplitRegion` assert (B).** No change since Phase 7; not re-investigated this phase.
- **"Visual Boot Experimental" mode (N).** The existing opt-in flags (`--game`, `--mount-ro`, `--pistation-boot-report`) are the entry point; no new UI was added because the run does not reach graphics.
- **Synthetic framebuffer present test (N).** Not written. It needs guest code issuing VideoOut/GNM calls (or a new host-side harness), which was not built.
- **Mono/Psm stubs, `dlerror`, `shm_open`, etc. (N).** Not changed; not shown to be blocking.

## 4. Graphics state (from the ShellUI run log)

| Milestone | Status |
|---|---|
| Emulator window created | C (the emulator creates it before running guest code) |
| Vulkan instance and device (AMD Radeon RX 6700 XT, driver 2.0.353, Vulkan 1.4.315) | C |
| Swapchain created (present mode fell back from Mailbox to Fifo) | C |
| Synthetic framebuffer presented | N |
| Guest `sceVideoOut*` / GNM submission reached | B: not observed in any run; ShellUI stops in `initializeSystem` first |
| Real guest framebuffer produced / first guest frame presented / dashboard | B |

A window with a swapchain is not evidence of rendering by the guest. No frame from the guest has been produced.

## 5. Tests executed (Windows, this machine)

| Item | Result |
|---|---|
| Debug build of `shadps4.exe` | exit 0 |
| Phase 6 synthetic ELF self-test | exit 0, guest returned `0x61747c64` |
| IPC self-test | 33 passed, 0 failed |
| `ctest` of `build-firmware900` | 7 of 7 passed (offline tools only) |
| `libSceIpmi.sprx` load probe | mapped and relocated, 55 imports stub-resolved, not executed |
| ShellUI, Mono + HLE IPMI (Phase 8 configuration) | reproduces the `initializeSystem` abort |
| ShellUI, Mono + LLE IPMI | `ipmimgr_call` trace as above, then null-client SIGSEGV and abort |
| `tests/` unit-test project | not run (has uncommitted local edits unrelated to this work) |

## 6. Remaining errors and next blocker

First blocker: the IPMI kernel interface (`ipmimgr_call`, ops 0x2, 0x400, 0x3) and its server side. Recommended next step: disassemble the `ipmimgr_call` call sites in `libSceIpmi.sprx` (the user's own copy, locally) to name the operations and the input/output structures; then decide whether a host-side `PiStation::Ipc::ServiceManager` can host the minimal server endpoints ShellUI needs before ShellCore is running. In parallel, start the per-process context refactor, since ShellCore and ShellUI cannot both be hosted without it.
