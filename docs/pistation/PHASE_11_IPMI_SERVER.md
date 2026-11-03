# Phase 11 — IPMI operation map and renderer shutdown fix

Windows only. Logs are local, untracked files under `build-emulator/local-validation/phase12/`. Nothing proprietary is committed.

## Renderer shutdown (confirmed fixed)

- Cause: `Vulkan::Instance::~Instance` calls `ImGui::Core::Shutdown`, which ends in `ImGui::DestroyContext`. The project's SDL3 ImGui backend (`imgui_impl_sdl3.cpp`, `Init`) fills `PlatformHandle` and `PlatformHandleRaw` of the main viewport, but its `Shutdown` never cleared them, so `ImGui::Shutdown` rejected the main viewport ("Backend or app forgot to call DestroyPlatformWindows()"). Found with a debugger backtrace at the assert.
- Fix: `Shutdown` now clears those two fields before the backend data is deleted. The assert is untouched.
- `--pistation-render-selftest` now runs two full cycles (window, Vulkan device, swapchain, one synthetic blank frame, destroy presenter, destroy Liverpool, destroy window) and returns normally. It no longer uses `quick_exit`. Result: exit 0 in about 2 seconds, both cycles logged.
- With the Vulkan validation layer enabled (SDK 1.4.363.0, core and sync validation through `user/config.json`), the same run finished with zero validation errors; the only message was a deprecation warning about a validation setting.
- Not covered: no image readback, so the content of the presented frame is not verified. The frame is blank and is not guest output.

## IPMI operation map (confirmed from the library's own symbols)

`IPMI_KERNEL_ABI.md` section 5 maps operations to the methods of `libSceIpmi` that issue them, using the exported C++ symbols that reach each wrapper. Result: `0x0`/`0x1` server initialize/destroy, `0x2`/`0x3` client initialize/destroy, `0x4`/`0x5` session create/destroy, `0x310` client disconnect, `0x320` sync invoke, `0x241` async invoke, `0x232`/`0x242` respond sync/async, message ops `0x251`-`0x255`/`0x456`/`0x457`, event flags `0x490`/`0x491`/`0x493`. Op `0x400` is reached only from an unnamed export; the guest's `connect failed` message suggests connect (unverified here). The layout of the server configuration was not established from the library alone.

## Code

- `ipmimgr.cpp`: operation names, the service name of each client handle, and a trace that separates `unimplemented_operation` (the host has no handler, errno 78) from `rejected_arguments_or_handle`. Tags `IPMI_CLIENT`, `IPMI_CONNECT`, `IPMI` appear in the log and the JSON report.
- IPC self-test: 45 of 45 checks (3 new: service name recorded, unreadable name leaves none, operation names).

## ShellUI retry (ShellUI + Mono + LLE libSceIpmi)

Same as Phase 10, now with names: `SceSystemLoggerService` and `SceDbPreparationServer` clients are created (`ok`), then op `0x400` for each is reported `unimplemented_operation`, so the guest's `0x8002004e` comes from the missing host handler, not from an absent server. No `sceVideoOut*` or GNM call was made. The first error is unchanged: `initializeSystem` aborts after `connect failed`.

## Not done

Server operations and a server backend: not implemented, because the server config layout was not established from the library (see above). ShellCore: still only loads and relocates. Multiprocess support: none. Module unload and the `SplitRegion` assert: unchanged. Linux: not tested.

## Tests

Debug build ok; synthetic ELF test `0x61747c64`; IPC self-test 45/45; two-cycle render self-test ok (also with validation layers); ShellCore load probe 2074 stub-resolved imports; `ctest` of the firmware tools 7/7 (offline tools only). The `tests/` project was not run.

## Next

A reference implementation of the IPMI kernel exists in the open-source RPCSX project (GPL-2.0); its struct layouts and the connect handshake can settle the points left open here. See Phase 12.
