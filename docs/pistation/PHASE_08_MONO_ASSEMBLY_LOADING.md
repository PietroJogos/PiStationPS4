# Phase 8 — Mono assembly loading for ShellUI

Based on shadPS4 (GPL-2.0-or-later). Windows only. Reports and logs named here are local, untracked files under `build-emulator/local-validation/phase8/`. No firmware or module bytes are stored in the repository.

## 1. Question

Phase 7 left ShellUI stopped at the Mono runtime not finding `mscorlib.dll`. This phase asks what is needed for the real Mono runtime (`libmonosgen-2.0.sprx`, run as guest code) to load the real assemblies of the user's firmware.

## 2. Evidence-based findings

1. Mono does not read `mscorlib.dll` first. For each search directory it calls `sceKernelLoadStartModuleInternalForMono("<dir>/mscorlib.dll.sprx")` with `flags = 0x20000`, and only then tries to open the plain `.dll`. In the firmware the assemblies are `*.dll.sprx` files in `system/common/lib`, so the module call is the real path. The earlier stub returned 0 without loading anything.
2. After `mscorlib.dll.sprx` loaded, Mono called `readlink` 128 times. `readlink` was a generic stub returning 0 (an empty link target), so a symlink-following loop ran to its limit and produced an empty AOT module name (`/.sprx`). Mono then reported an AOT-only error and aborted with `ud2` (`libmonosgen` + `0xd2533`).
3. `access` already had an implementation, registered only for the library name `libkernel_psmkit`. The imports here name `libkernel`, so it resolved to a stub that returned success for files that do not exist.

## 3. Changes

- `sceKernelLoadStartModuleInternalForMono` (NID `Y1nEpkCieOY`, `libkernel`): new HLE in `src/core/libraries/kernel/process.cpp`. It shares the path loading logic of `sceKernelLoadStartModule` through a static helper, does not assert on `flags`, logs `[KERNEL]` lines, and returns the handle or `ORBIS_KERNEL_ERROR_ENOENT`. The argument layout is assumed equal to `sceKernelLoadStartModule`; the observed calls (path, `args = 0`, `flags = 0x20000`) are consistent with it, but `pOpt` and `pRes` usage by Mono is unverified.
- `posix_readlink` (NID `pd02UI9TbOA`, `libkernel`): new in `src/core/libraries/kernel/file_system.cpp`. Returns `-1` with `ENOENT` for a missing path and `EINVAL` for an existing one, because guest-visible paths are never symlinks; `EFAULT` and `ENAMETOOLONG` for bad arguments. It never exposes host paths.
- `access`: also registered under `libkernel`, using the existing `posix_access`.
- `--mount-ro <host> <guest>`: new CLI option and `Emulator::readOnlyMounts`, mounted with the existing read-only flag of `MntPoints::Mount`. Used to expose the user's own `system/common/lib` at `/sys/common/lib` without allowing guest writes. Nothing is mounted unless the option is given.

## 4. Result of the experiment

Command shape (paths are the user's local ones; the four Psm/Mono modules are copied into an isolated profile's `custom_modules/NPXS20001/`, plus the staged `libSceLibcInternal.sprx`):

```
shadps4 --game <NPXS20001>/eboot.bin --mount-ro <firmware>/system/common/lib /sys/common/lib --pistation-boot-report <dir>
```

Observed in `phase8/mono-run2/stdout2.txt`:

- The real `mscorlib.dll.sprx`, `System.dll.sprx`, `Sce.PlayStation.Json.dll.sprx`, `Sce.Vsh.Lx.dll.sprx` and others loaded and started through the real Linker (module handles are returned and used).
- The AOT-only error and the `ud2` abort from Phase 7 no longer occur. The log is about 6000 lines versus about 5500 before.
- Mono prints `[PSM] AOT mode`, and the Mono-side probing for optional assemblies (for example `policy.2.0.System`) fails with ENOENT as expected.
- ShellUI's own native code then ran far enough to print its source location: `shell_ui\src\shell_ui_main\init_system.cpp(665) initializeSystem: LX_ISOK2::0x8002004e` and `[SceMorpheusUpdClient] Initialize: create client failed: 0x8002004e`. `0x8002004e` is the `ENOSYS` value returned by the Phase 7 `IPMI::Client::create`. The guest then raised `sceKernelDebugRaiseExceptionOnReleaseMode` and the host aborted (exit `0x80000003`).

So the Mono blocker is resolved to the point where it no longer stops the main thread, and the next blocker is mandatory IPMI client creation. This confirms with evidence that ShellUI needs a working `IPMI::Client` to finish `initializeSystem`. It also shows the explicit `ENOSYS` is propagated by the guest instead of being masked.

Still stubbed on this path (from `stub_calls`): `dlerror`, `sysctl`, `shm_open`/`shm_unlink`, `sceKernelIsDevelopmentMode`, `sceVideoOutSysAddSetModeEvent`, `sceAudiodecInitLibrary`, `pthread_spin_init`, `sceMbusEventCreate_`, `sceRegMgrGetInt`/`GetStr`, the `IPMI::Client::Config` members, `sceMetadataReaderWriter`, `AbstractStorage::StorageManager`, `sceUpsrvInitialize`, `scePatchCheckerInitialize`. Their effect was not individually evaluated. `[PSM] failed to dlsym for scePsmUtilGetPsmIniFile()` and `...RegisterCallbacks()` are printed by the guest; the cause was not investigated.

## 5. Tests (Windows, this machine)

| Item | Result |
|---|---|
| Debug build | exit 0 |
| Phase 6 synthetic ELF self-test | exit 0, guest returned `0x61747c64` |
| IPC self-test | 33 passed, 0 failed |
| ShellCore load probe | exit 0, 2074 stub-resolved imports (was 2075), not executed |
| `ctest` of `build-firmware900` | 7 of 7 passed (offline tools; they do not cover this phase's code) |
| `readlink` / `access` / `ForMono` | verified only through the integration run above; no isolated unit test was written |

Not run: the `tests/` unit-test project (it has uncommitted local edits that are not part of this work), and anything on Linux.

## 6. Limits

- The experiment depends on the user's firmware copies loaded as custom modules and on `--mount-ro`; default behaviour is unchanged.
- The run is ShellUI alone. SceShellCore is not started, there is no multiprocess support, no framebuffer, and no dashboard.
- Module unload remains unsolved (see Phase 7).

## 7. Next step

Establish the `IPMI::Client` contract. The call site is known (Phase 7): `create` returns a polymorphic object whose `vtable[+0x10]` is called with `(0, 0, int*)`. Capture the full sequence of client calls from ShellUI and from ShellCore's server side, then decide whether a host-side `PiStation::Ipc::ServiceManager` can back a faithful implementation or whether ShellCore must run first.
