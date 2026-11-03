# Phase 10 — IPMI mapping and handle-backed kernel manager

Windows only. Logs are local files under `build-emulator/local-validation/phase10/`; nothing proprietary is committed. Contract details are in `IPMI_KERNEL_ABI.md`.

## Result

| Item | Status |
|---|---|
| `ipmimgr_call` calling convention and the 47 operations of `libSceIpmi` mapped from the library | Confirmed (disassembly) |
| Kernel-side create (`0x2`) and destroy (`0x3`) with a real handle table | Implemented and unit-checked |
| Correct failure convention (`-1` + `errno`) | Implemented; the Phase 9 version returned the code directly |
| IPMI service names ShellUI asks for | Confirmed by trace |
| Connect (`0x400`) and the rest | Not implemented, returns `ENOSYS` |
| IPMI servers (ShellCore and others) | Not present |

## Boot comparison (ShellUI + real Mono modules + LLE `libSceIpmi`)

- Phase 9 configuration: first IPMI client creation failed; later a null client was dereferenced and Mono aborted (`0xc000001d`).
- Now: seven client objects are created and destroyed without error. ShellUI logs `[DBP Error][UI][cexInitialize] BeginWaitForInitialization() failed [0x8002004e]`, `[SceMorpheusUpdClient] Initialize: connect failed 0x8002004e`, then `init_system.cpp(665) initializeSystem: LX_ISOK2::0x8002004e`, and the guest aborts (`0x80000003`). The log grew from about 6000 to about 9000 lines. The null-client crash is gone because creation now succeeds.
- Services requested by ShellUI before it gives up: `SceSystemLoggerService`, `SceDbPreparationServer`, `SceAppDbIpc`, `SceMorpheusUpdService`.

The first blocker is now: no IPMI server answers a connect. Those servers live in other system processes, so this is also the multiprocess problem; it is not solvable with a client-side change.

## Tests (Windows)

Debug build ok. IPC self-test 42 of 42 (33 earlier checks plus 9 for the kernel manager: null result, wrong size, null config, create, distinct handles, unknown op, destroy with args, destroy, double destroy). Phase 6 synthetic ELF test passes (`0x61747c64`). ShellCore load probe unchanged (2074 stub-resolved imports, not executed). `ctest` of `build-firmware900`: 7 of 7 (offline tools only). The `tests/` project was not run (uncommitted local edits unrelated to this work).

## Not done

No server-side operations, no ShellCore execution, no process isolation, no module unload, no graphics progress (the guest still stops before any VideoOut/GNM call).

## Next

Find which operations form the server side (the ops in `0x2xx` and `0x3xx` ranges are candidates, unverified), then decide whether to host the four required services inside the emulator process through `PiStation::Ipc::ServiceManager`, or to run ShellCore. Hosting them requires knowing each service's message formats, which come from the ShellUI client calls and are not yet captured.
