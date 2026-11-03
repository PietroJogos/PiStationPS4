# Phase 12 — IPMI kernel ported from RPCSX

Windows only. Logs are local, untracked files under `build-emulator/local-validation/phase13/`. No firmware is committed.

## What was taken from RPCSX, and what was not

RPCSX (https://github.com/RPCSX/rpcsx, revision `e8ae1481`, checked out locally under `RPCSX-FIXED/upstream`) boots further than this project because it has its own multi-process Orbis kernel: every PS4 process becomes a host process, which is how SceSysCore, ShellCore and their children start. That design needs `fork`, `mmap` and Linux process control; it is not a component that fits the single-process shadPS4 runtime on Windows, and nothing of it was brought over.

What was taken is the knowledge of the IPMI kernel (`kernel/orbis/src/ipmi.cpp`, `include/orbis/ipmi.hpp`, the `sys_ipmimgr_call` dispatcher): the structure layouts, the packet types and the connection protocol. The code in `src/core/libraries/ipmi/ipmi_kernel.{h,cpp}` is a rewrite for this emulator (one mutex and condition variable, shared pointers, guest memory behind a validity check), not a copy. Differences from RPCSX that are deliberate:

- the synchronous-call wait checks its condition before sleeping (RPCSX waits first, which can miss a response that is already queued);
- a packet is only removed from the server queue if the caller's buffer is large enough (RPCSX removes it first and then fails);
- destroy operations really remove the object (RPCSX returns success without doing anything), and wake blocked callers;
- a server-less connect fails with `-1` and `errno = ENOENT`, the convention the `libSceIpmi` wrappers use. RPCSX returns ENOENT as a value that is not flagged as an error; which of the two the real kernel does was not verified.

## License

RPCSX is distributed under the GNU GPL version 2 (its `LICENSE` is the plain GPLv2 text; `ipmi.cpp` carries no per-file notice, and no "or later" grant was found). shadPS4 is GPL-2.0-or-later. The new files are therefore marked `GPL-2.0-only`, with a copyright line crediting the RPCSX contributors, and the combined binary can only be redistributed under GPL-2.0, not under GPL-3.0. If the project wants to stay free to use "or later", the files in `ipmi_kernel.*` and `ipmimgr.*` would have to be dropped or rewritten from the library-derived facts alone. This should be confirmed by the project owner before a release.

## Implemented operations

Server create/destroy (`0x0`, `0x1`), client create/destroy (`0x2`, `0x3`), session create/destroy (`0x4`, `0x5`), trace (`0x10`), server receive packet (`0x201`), send connect result (`0x212`), respond to a synchronous call (`0x232`), client connect (`0x400`), disconnect (`0x310`), synchronous invoke (`0x320`), session client pid (`0x302`), session user data (`0x468`), server and client name (`0x46a`, `0x46b`).

Not ported, still answered `ENOSYS` and reported as `unimplemented_operation (host: not ported)`: asynchronous calls (`0x241`, `0x242`, `0x243`), message queues (`0x251`-`0x255`, `0x456`, `0x457`), event flags (`0x490`, `0x491`, `0x493`), session app id (`0x463`) and the remaining ops. The host keeps one process id (`1`) for every caller, so the "same process" flag is always set on synchronous packets.

## Tests (Windows)

IPC self-test: 47 of 47. The IPMI part (15 checks) uses only the kernel ABI: an absent server gives `ENOENT`; duplicate server name, invalid handle, wrong parameter size, unreadable name and a not-ported op are rejected with `EEXIST`, `EINVAL`, `EINVAL`, `EFAULT` and `ENOSYS`; a server thread and four client threads complete 4 connect handshakes and 100 synchronous calls whose replies ("pong:" plus the request) travel through the kernel packet queue; a connect to a server nobody serves blocks until `Shutdown`, which wakes it with `ECONNABORTED`. This proves the kernel logic with synthetic peers; it does not prove compatibility with real PS4 services.

Also run: Debug build (ok), Phase 6 synthetic ELF test (`0x61747c64`), two-cycle render self-test (ok), `ctest` of `build-firmware900` (7 of 7, offline tools only). The `tests/` project was not run.

## ShellUI (ShellUI + Mono + LLE libSceIpmi)

Clients for `SceSystemLoggerService` and `SceDbPreparationServer` are created, and `connect` now fails because no server of that name is registered (`server_not_registered`, errno 2), instead of failing because the operation had no handler. The guest reports `[DBP Error][UI][cexInitialize:60] BeginWaitForInitialization() failed. [0x80020003]` (the library converts the error; the code is ESRCH in the FreeBSD numbering) and the run still ends with the guest abort. No `sceVideoOut*` or GNM call was made.

## Remaining blocker

The services ShellUI needs (`SceSystemLoggerService`, `SceDbPreparationServer`, `SceAppDbIpc`, `SceMorpheusUpdService`) are servers inside other PS4 processes. The kernel side for hosting them now exists, but the processes that create them (ShellCore and its children) cannot run in the single-process runtime. Writing replacement servers would be inventing the behavior of services whose protocol is not known. The next real step is process isolation, or running those components in a separate emulator process that shares this IPMI kernel.
