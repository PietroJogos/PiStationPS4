# Phase 10 — Renderer self-test and boot state

Windows only (Radeon RX 6700 XT). Logs are local, untracked files under `build-emulator/local-validation/phase11/`. Nothing proprietary is committed. Labels: **C** confirmed by test or run, **P** partial, **B** blocked, **N** not done.

## 1. State of Phase 9 and Phase 10 IPMI work (C)

Verified from the branches and docs: Phase 9 added the traced `ipmimgr_call`; the follow-up branch `feature/ps4-ipmi-kernel-manager` (`d32a25d2`) added a handle-backed kernel manager and `IPMI_KERNEL_ABI.md`. Neither produced a guest framebuffer. This branch builds on that work. With Mono loaded and `libSceIpmi` as LLE, ShellUI creates seven IPMI client objects (services `SceSystemLoggerService`, `SceDbPreparationServer`, `SceAppDbIpc`, `SceMorpheusUpdService`), the connect (`0x400`) fails with `0x8002004e`, and `initializeSystem` aborts. The first boot blocker is therefore unchanged: no IPMI server answers. That needs other system processes (ShellCore and more) and per-process state, which does not exist; nothing in this phase changes it (**B**).

## 2. Implemented: synthetic render self-test (C)

`--pistation-render-selftest <dir>` (`src/core/pistation/runtime_probes.cpp`). It uses the real classes: `Frontend::WindowSDL`, `AmdGpu::Liverpool` and `Vulkan::Presenter` (instance, device, schedulers, swapchain, rasterizer). It then calls `PrepareBlankFrame` and `Present` once. The frame is blank; it is not guest output and not an image of the PS4 interface.

Observed in the report (`final-render`): SDL window created (1280x720); renderer initialized; blank frame submitted to the swapchain; exit code 0. In the earlier ShellUI logs the same stack reports the GPU as AMD Radeon RX 6700 XT, Vulkan 1.4.315, present mode falling back from Mailbox to Fifo.

Limits of this test: it checks that these calls complete without a crash or Vulkan abort, with validation layers off (the default); it does not read the image back, so it does not prove the swapchain image contents. It does not run with validation enabled.

## 3. Bug found: the renderer cannot be torn down (B)

Destroying the `Presenter` asserts in `ImGui::DestroyContext` ("Backend or app forgot to call DestroyPlatformWindows()", `imgui.cpp:4636`, viewport `PlatformUserData` still set). The normal runtime never destroys the presenter, so this was never visible. The self-test therefore records `renderer_teardown ... unsupported`, leaks the three objects on purpose and exits through `std::quick_exit`, like the runtime does. Without `quick_exit` the process did not terminate (renderer threads keep running after `main` returns); one such hung process had to be killed. The assert was not disabled; the root cause (the ImGui platform backend shutdown order) was not fixed.

## 4. Not done

- **Guest framebuffer (B).** ShellUI stops in `initializeSystem` before any `sceVideoOut*` or GNM call, so no VideoOut event was captured and no guest frame exists. The "first real frame" step was not attempted.
- **ShellCore execution, multiprocess support, `PiStationProcessContext` (N/B).** ShellCore only loads and relocates (2074 stub-resolved imports, never executed). `Linker`, `MemoryManager`, `ElfInfo`, handle tables and HLE tables are singletons, so no honest isolation test is possible yet.
- **Visual Boot Experimental UI (N).** No UI was added; the opt-in command-line path (`--game`, `--mount-ro`, `--pistation-boot-report`, `--pistation-render-selftest`) is the interface. Nothing runs firmware automatically.
- **Controller / keyboard (N).** Not investigated beyond noting that `GameControllers` is created by the self-test and reused by the window.
- **Module unload and the `AddressSpace::SplitRegion` assert (B).** Unchanged since Phase 7.
- **Performance (N).** No measurements were taken.

## 5. Tests executed

| Item | Result |
|---|---|
| Debug build of `shadps4.exe` | exit 0 |
| Synthetic render self-test | exit 0; window, renderer and blank-frame present logged |
| Phase 6 synthetic ELF self-test | exit 0, guest returned `0x61747c64` |
| IPC self-test (includes IPMI kernel manager) | 42 of 42 |
| `ctest` of `build-firmware900` | 7 of 7 (offline tools only) |
| `tests/` unit-test project | not run (uncommitted local edits unrelated to this work) |

## 6. Next step

Define the server side of IPMI. Candidates are the `0x2xx` and `0x3xx` operations of `libSceIpmi` (see `IPMI_KERNEL_ABI.md`); identify which wrappers a server calls to register, accept, receive and reply, then decide whether the four services ShellUI needs can be hosted in-process through `PiStation::Ipc::ServiceManager`, or whether ShellCore must run first. In parallel, fix the ImGui shutdown order so the renderer can be created and destroyed repeatedly in a test.
