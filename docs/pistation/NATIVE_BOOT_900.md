# PiStation PS4 — Native Boot 9.00 (experimental, phase 6)

Based on shadPS4 (GPL-2.0-or-later). No firmware, keys or decrypted content are stored in this repository or in the reports.

## 1. Execution architecture

`PiStation::NativeBootBackend` (`src/core/pistation/native_boot_backend.{h,cpp}`) is not a second emulator. It is an opt-in recorder attached to the existing runtime: `Core::Linker`, `Core::Module`, `Core::MemoryManager` and the HLE libraries do all the work. It tracks seven evidence-based phases and writes `native_boot_report.json` and `native_boot_report.txt` after every phase change, so a crash keeps the last reached phase.

| Phase | Meaning |
|---|---|
| `file_identified` | ELF header accepted by `Elf` |
| `file_loaded` | `Core::Module` mapped segments and parsed dynamic info |
| `memory_prepared` | memory regions set up, sentinel state verified |
| `entry_reached` | host is about to transfer control (not proof of execution) |
| `guest_code_executed` | an effect only guest code could cause is observed |
| `execution_finished` | guest returned a value computed by guest code |
| `resources_released` | guest memory freed (see section 8, not reached) |

Disabled by default. Flags:

- `--pistation-synthetic-selftest <dir>`: builds, loads and runs the synthetic guest ELF.
- `--pistation-boot-report <dir>`: enables the recorder for a normal run (`Linker::Execute` reports phases, unresolved-symbol stubs).

## 2. Code changed

- New: `src/core/pistation/native_boot_backend.{h,cpp}`.
- `src/core/linker.cpp`: phase hooks in `Execute`, unresolved (stub-resolved) symbols recorded in `Resolve`.
- `src/core/memory.cpp`: `rasterizer->` calls are now null-guarded, so the memory manager works before Vulkan exists (headless). Behaviour with a renderer is unchanged.
- `src/common/elf_info.h`: `InitializeSynthetic` (required by `AddressSpace` for the SDK version).
- `src/main.cpp`, `CMakeLists.txt`: flags and sources.

## 3. Build

Debug x64, Clang 23.1.4, Windows 10/11, `shadps4.exe` links with the complete core (fdk-aac submodule present, LibreSSL endian shim from the earlier local validation). Incremental build exit code 0 (`build-emulator/local-validation/build-phase6.log`). Not built on Linux.

## 4. Synthetic ELF test

`BuildSyntheticGuestElf()` emits an `ET_SCE_DYNAMIC` x86-64 image: RX segment with 27 bytes of code, RW data segment, `PT_DYNAMIC` with `DT_INIT`, `PT_SCE_DYNLIBDATA`. The code stores a 64-bit magic and `rdi` into the data segment and returns `low32(magic) ^ edi`. It is loaded with `Linker::LoadModule` and started with `Module::Start`.

## 5. Evidence of real guest execution (Windows, run on this machine)

```
[x] file_identified : synthetic ELF written, 4112 bytes, e_type=ET_SCE_DYNAMIC
[x] file_loaded : module 'synthetic_guest.elf' loaded by Core::Linker at base 0x80000000
[x] memory_prepared : RX and RW segments mapped; sentinel at 0x80001000 is zero
[x] entry_reached : e_entry resolves to 0x80000200, DT_INIT to 0x80000200
[x] guest_code_executed : guest code wrote magic 0x5069537461746e50 and argument 0x1234 ...
[x] execution_finished : guest returned 0x61747c64, computed by guest code
[ ] resources_released
```

Exit code 0. The sentinel was zero before the call and the host never wrote it. Reports: `build-emulator/local-validation/synthetic-selftest/final/report/`.

## 6. Real firmware attempt

Target: the user's own NPXS20001 `eboot.bin` (ShellUI candidate, ELF type `0xfe10`, firmware version not confirmed), staged read-only, run with `--pistation-boot-report`, 40 s limit. Report: `build-emulator/local-validation/real-attempt-phase6/report/`.

- Phases reached: identified, loaded, memory prepared, entry handoff.
- Guest code did run: logs show `SceLibcI_Thr` and `SceSystemLoggerPullJob` guest threads, the ShellUI logger (`[DBP Message][UI][InitializeDb:205]`), `sceKernelMapNamedFlexibleMemory` for `ShellUiSystemLogHead`.
- This is ShellUI alone under HLE. SceShellCore was not started, no framebuffer was presented, no dashboard appeared.
- Only the synthetic test has the strict guest-execution proof; the real run is recorded as `entry_reached` because there is no probe that proves the entry itself ran.

## 7. First blocking dependencies

The first `Stub:` calls (stub functions returning zero, not real implementations) are `sceKernelGetSanitizerMallocReplace` and `sceKernelGetSanitizerNewReplace`. The report lists 3057 symbols resolved to stubs (244 with no known NID name), mostly libSceNpManager (380), libkernel (235), libSceJson2 (230), libSceDataTransfer, libSceAppInstUtil, libSceBgft. The first structural blocker is IPMI: `IPMI::Client::Config` / `IPMI::Client::create` are called from `SceSystemLoggerPullJob` and return zero, and `sceSysmoduleLoadModuleByNameInternal` is `STUBBED`. ShellUI also needs SceShellCore, which provides these services over IPC.

## 8. Not implemented

- Module unload: neither `Linker` nor `MemoryManager` can release a loaded module. `UnmapMemory` on Flexible module segments asserts in `AddressSpace::SplitRegion` ("Cannot split unbacked mapping"). `resources_released` is therefore reported as not reached, and the report records `not_released`.
- Multiple guest processes, a real IPMI/IPC implementation, SceShellCore start, process lifecycle, firmware-folder selection UI (the "Native Boot Experimental" mode), pre-run validation of encrypted/SELF files in the emulator (offline scanner in `tools/firmware900` still covers this), watchdog/timeout inside the emulator (the PowerShell launcher enforces the limit), per-category tags `[HLE]`, `[LLE]`, `[VULKAN]`, `[SCESHELLCORE]` and syscall tracing.
- Graphics and input for the dashboard were not investigated.

## 9. Next fixes

1. Implement `IPMI::Client` (and the SceShellCore side) as a real IPC channel instead of zero-returning stubs.
2. Replace `sceSysmoduleLoadModuleByNameInternal` stub with real module loading.
3. Add module unload to `Linker`/`MemoryManager` and finish the release phase.
4. Add a guest-execution probe for real modules (instruction-trace or breakpoint at entry).
5. Run the same tests on Linux; only Windows was exercised here.
