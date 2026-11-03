# Phase 7 — IPMI/IPC, SceShellCore load probe and ShellUI blocker analysis

Based on shadPS4 (GPL-2.0-or-later). Windows 10/11 x64 only; nothing here was run on Linux. No firmware, keys or module bytes are stored in the repository or the reports. All logs and reports named below are local, untracked files under `build-emulator/local-validation/phase7/`.

## 1. Starting point (Phase 6)

Phase 6 (`8e2bf455`) proved guest execution only with a synthetic ELF. The NPXS20001 run ended right after `IPMI::Client::create` was stub-called, and the Phase 6 document named IPMI as the first blocker. This phase tested that claim.

## 2. Correction to the Phase 6 diagnosis

The Phase 6 log ends with `sceKernelDebugRaiseExceptionOnReleaseMode: Unreachable code!` on `Game:Main` (`real-attempt-phase6/stdout.txt`, line 3446), shortly after the stub call to `sce::pss::orbis::framework::PsmFramework::Run`. IPMI runs on a different thread (`SceSystemLoggerPullJob`). So the terminating event on the main path is the guest aborting after `PsmFramework::Run` (libScePsm, not emulated) returned immediately, not the IPMI call. IPMI remains a real unimplemented dependency but is not the first thing that stops the main thread.

## 3. IPMI functions investigated

ShellUI imports 12 libSceIpmi symbols (11 named, 1 unknown NID) (list in the report's `unresolved_symbols`). Disassembling the caller in the user's own NPXS20001 `eboot.bin` (local analysis, nothing saved to the repo) gave this contract for the call actually made:

- `IPMI::Client::Config::Config()` then `Config::estimateClientMemorySize()`; the result is passed to a `malloc`.
- `IPMI::Client::create(Client** out, const Config*, void*, void*)` returns `s32`; in this caller the third argument is null and the fourth is the buffer from the `malloc` above;; the caller branches on a negative result. On success it loads `*out` and calls `vtable[+0x10]`.

Status: all of these were generic stubs returning 0, so `create` reported success with a null `*out`. The client object layout, vtable semantics and the server side are not known and were not guessed.

Implemented (`src/core/libraries/ipmi/ipmi.cpp`): `IPMI::Client::create` and `IPMI::Server::create` are real HLE functions that write `*out = nullptr`, log `[IPMI]`, record a `[BOOT_ERROR]` entry in the report and return `ORBIS_KERNEL_ERROR_ENOSYS`. This turns a false success into an explicit, deterministic failure. It is not an IPMI implementation. `ENOSYS` is used because the real IPMI error values are not known.

## 4. Generic IPC infrastructure

`PiStation::Ipc::ServiceManager` (`src/core/pistation/ipc_service_manager.{h,cpp}`): named endpoints with owner ids, connections with client ids, bounded request and reply queues, message size limit, endpoint and connection limits, timeouts, disconnect and unregister wake-ups, owner checks, shutdown, a bounded call trace, and an RAII `ScopedConnection`. It is a host-side building block. It is not connected to the guest IPMI ABI because that contract is unknown; nothing in the guest reaches it yet.

Self-test (`--pistation-ipc-selftest <dir>`): 33 checks passed, 0 failed (duplicates, limits, wrong owner, full queue, oversized message, timeout >= 45 ms, disconnect and shutdown waking blocked threads, RAII release, 8 clients x 50 echo requests with checksum). The test found one real bug in `ScopedConnection` (member initialization order), fixed before the final run.

## 5. Stub-call diagnostic

`CommonStub` now reports each called stub to the backend (`stub_calls` in the JSON, with name, NID, call count, first thread, first return address, order). Called stubs, in order, ShellUI with HLE only: `sceKernelGetSanitizerMallocReplace`, `sceKernelGetSanitizerNewReplace`, `sceKernelInternalMemoryGetModuleSegmentInfo`, `dup2`, `scePthreadSetDefaultstacksize`, two unknown NIDs, `sceLibcMspaceCreateForMonoMutex`, `sceCoredumpAttachMemoryRegion`, `PsmInitParam::PsmInitParam`, `PsmFramework::Initialize`, `RegisterPInvokeCallTable`, `PsmFramework::Run`. The report's counts are of stubs that were actually called (13 and 19), not of all 3053 stub-resolved imports. The report tracks the first call per NID and counts later calls; Calls are attributed to the guest thread name only; there is no per-process attribution because there is a single process.

## 6. Experiment: real libScePsm family

An isolated profile loaded `libmonosgen-2.0`, `libScePsmUtil`, `libScePsmKitSystem`, `libScePsm` from the user's firmware as custom modules (existing `custom_modules` mechanism) next to the staged `libSceLibcInternal.sprx`. Observed (`final-shellui-psm/stdout.txt`):

- the four modules mapped by the real Linker;
- the Mono runtime executed guest code and printed `The assembly mscorlib.dll was not found or could not be loaded` with the search path `/app0/psm/Application:/sys/common_ex/lib:/sys/common/lib`;
- the guest then called `exit(1)` (`Exiting with status code 1` in the log) and the host aborted through the usual assert path (exit code `0x80000003`).

So the first blocker on the main path is the Mono runtime's assembly loading, not IPC. Newly called stubs on that path: `sceKernelLoadStartModuleInternalForMono`, `access`, `shm_open`, `shm_unlink`, `getrlimit`, `sysctl`, `sceKernelIsDevelopmentMode`, `sceApplicationInitialize`, `sceRegMgrGetStr`, `sceKernelGetPsmIntdevModeForRcmgr`. This was an experiment with copies of the user's files in an untracked folder; the normal path was not changed to load them.

## 7. SceShellCore

`SceShellCore.elf` (21,696,800 bytes, plain ELF, type `0xfe10`, entry `0x62df70`, SHA-256 `47078400c4c35a7aaf090c7095180080bb11488f8a86aba6c6e3ed0f7cddcc34`, four load/relro segments, `PT_SCE_PROCPARAM` present) was found at `system/vsh/`. It is not a SELF and needed no decryption.

`--pistation-load-probe <elf>` (final run, `final-shellcore-probe/report`): the real Linker mapped it (size `0x1524000`), parsed 45 import modules and 57 import libraries, computed the entry, and relocated all imports. It was **not executed**. Result: 2075 imports resolved to stubs. Largest: libSceLibcInternal 681, libSceNpCommon 508, libkernel 136, libSceNpManager 126, libSceJson2 121, libSceFsInternalForVsh 98, **libSceIpmi 98**, libSceSysCore 61. The probe skips the libSceGnmDriver and libSceVideoOut HLE because they start a presenter thread that crashes without a window (observed), so graphics imports are counted as unresolved. ShellCore start-up is therefore blocked by IPMI, NP, FS-for-VSH and SysCore services, and by multiprocess support (next section).

## 8. Multiprocess

Not implemented, and not testable honestly with the current design. `Linker`, `MemoryManager`, `ElfInfo`, HLE tables and the file/handle tables are process-wide singletons (`Common::Singleton`), and guest address space is one fixed host reservation. Two guest processes with separate memory, TLS, handles and module state would need those singletons made per-process. Running ShellCore and ShellUI "one after another" would not be multiprocess. The synthetic isolation test requested was therefore not written; it would test nothing that exists.

## 9. Memory unload

Not fixed. Findings: the assertion is `AddressSpace::SplitRegion` "Cannot split unbacked mapping" (`address_space.cpp`), reached from `MemoryManager::UnmapMemory` when a mapped region has `phys_base == -1`. Module segments are mapped by `Module::LoadModuleToMemory` as Flexible (non-system modules) inside a larger Reserved reservation, plus an 8 MB `Code` trampoline mapping. In Phase 6 unmapping the whole window and unmapping the two loaded segments both asserted. Ownership of the trampoline area, the patch registration (`RegisterPatchModule`) and `Linker::m_modules` is not released by any existing path, and I could not establish the correct teardown order. The assert was left in place and no forced release was added. `resources_released` stays unreached in the synthetic test.

## 10. Runtime changes

- `--pistation-boot-report` unchanged and still opt-in; report now includes `stub_calls`, is flushed from `Emulator::Shutdown` (including the assertion path), and records `process_exit`.
- `--pistation-synthetic-selftest` unchanged in behaviour.
- New opt-in flags: `--pistation-ipc-selftest`, `--pistation-load-probe`, `--pistation-load-probe-report`.
- `src/core/libraries/libs.cpp`: skips two graphics HLE modules only when the load probe asks for it.

## 11. Test results (Windows, this machine)

| Item | Result |
|---|---|
| Debug build of `shadps4.exe` | exit 0 |
| Phase 6 synthetic ELF self-test (regression) | exit 0; guest executed, returned `0x61747c64`; `resources_released` still not reached |
| IPC self-test | 33 passed, 0 failed |
| ShellCore load probe | mapped and relocated, exit 0, not executed |
| `ctest` of `build-firmware900` (7 tests) | 7 passed; these cover the offline tools and do not exercise the new emulator code |
| ShellUI, HLE only | exits via guest abort (`0x80000003`) after `PsmFramework::Run` stub |
| ShellUI, real Mono/Psm modules | Mono runs, `mscorlib.dll` not found, guest `exit(1)` |

The unit test project under `tests/` was not run: it has uncommitted local edits from earlier work that I did not touch.

## 12. Status labels

Loader started: yes. Entry reached: yes (ShellUI). Guest code executed: yes (observed Mono and ShellUI output; strict proof only in the synthetic test). System service initialized: no. ShellUI functional: no. ShellCore functional: no (not started). Framebuffer: none. Dashboard: not shown. Graphics: not investigated beyond observing that the HLE presenter needs a window.

## 13. First remaining blocker and next step

First blocker on the main path: Mono's assembly loading for ShellUI (`mscorlib.dll`, via `sceKernelLoadStartModuleInternalForMono`, `/sys/common/lib` mount, `access`, `shm_*`). Recommended next steps, in order:

1. Implement `sceKernelLoadStartModuleInternalForMono` for real and make the firmware's `/sys/common/lib` readable to the guest through a read-only mount of the user's own files.
2. Then re-run and take the next stub from the `stub_calls` list.
3. After ShellUI gets past Mono start-up, return to IPMI with a captured call sequence to establish the client object contract.
4. Make Linker/MemoryManager state per-process before attempting ShellCore.
