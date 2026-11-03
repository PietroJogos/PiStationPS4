# IPMI kernel interface (`ipmimgr_call`) — observed contract

Derived from local analysis of the user's own `libSceIpmi.sprx` (disassembly of its call sites; nothing from it is stored here) and from traced runs of ShellUI. Windows only. Items marked *inferred* are not verified.

## 1. Calling convention (confirmed from the library's wrappers)

`libSceIpmi` has one import that matters, `ipmimgr_call` (NID `Hk7iHmGxB18`, libkernel). It is called from 47 small wrapper functions, one per operation, in this form:

```
int ipmimgr_call(u64 op, s32 handle, s32* result, const u64* args, u64 args_size, u64 sentinel)
```

- `op`: operation code (`edi`). `handle`: kernel IPMI object id (`esi`), 0 when creating.
- `result` (`rdx`): points to an `s32` the wrapper pre-sets to `-1`. On success the kernel writes the operation's result there.
- `args` / `args_size` (`rcx`, `r8`): an array of 64-bit values; the wrappers use 0x4 to 0x30 bytes, and 0 when there are no arguments.
- `sentinel` (`r9`): always `0xdeadbadecafebeaf`; unused.
- Return value: `0` means the system call succeeded and `*result` is valid. Non-zero means failure with `errno` set; the wrapper then reads `errno` through `__error()` and converts it to a `0x8002xxxx` code (`0x80020000 | errno`).
- The blocking wrapper for op `0x400` loops while the converted error is `0x80020004` (EINTR) and, under a condition on a caller flag, `0x80020025`.

## 2. Operations present in the library (arg size in bytes)

`0x0` 0x18, `0x1` var, `0x2` 0x18, `0x3` var/0, `0x4` 0x10, `0x5` var, `0x10` var, `0x12d` 0x4, `0x12e` var, `0x201` 0x20, `0x202` 0x18, `0x212` 0x4, `0x222` 0x4, `0x232` 0x18, `0x241` 0x30, `0x242` 0x20, `0x243` 0x28, `0x251` 0x28, `0x252` 0x20, `0x253` 0x20, `0x254` 0x18, `0x255` 0x4, `0x271` 0x8, `0x302` 0x8, `0x303` var, `0x310` 0x8, `0x320` 0x30, `0x372` 0x8, `0x400` 0x20, `0x444` 0x4, `0x456` 0x28, `0x457` 0x20, `0x463`-`0x46b` 0x8 each, `0x473` 0x10, `0x490` 0x28, `0x491` 0x20, `0x492` 0x18, `0x493` 0x10, `0x520` var.

"var" = size taken from a register at the call site. The meaning of most operations has not been established.

## 3. Operations observed in ShellUI and what is known

| Op | Args | Evidence |
|---|---|---|
| `0x2` | 3 qwords: user object pointer, pointer to a config whose first field is the service name string, third pointer | Called from the routine that copies a 0x150-byte client configuration and stores the returned value at `this+8`. `*result` becomes the new object handle; negative means failure (the library logs and returns it). Seen with service names `SceSystemLoggerService`, `SceDbPreparationServer`, `SceAppDbIpc`, `SceMorpheusUpdService` |
| `0x3` | none, handle in `handle` | Called from the object's cleanup routine with the stored handle; a non-zero return is logged as an error |
| `0x400` | 4 qwords | Correlates with the guest message `[SceMorpheusUpdClient] Initialize: connect failed` (*inferred*: connect/wait for the server); retried on EINTR |
| `0x310` | 1 qword (pointer) | Called right after a failed `0x400` (*inferred*: cancel/disconnect) |

## 4. What the emulator implements

`Libraries::Ipmi::IpmiKernelManager` (`src/core/libraries/ipmi/ipmimgr.{h,cpp}`):

- `0x2`: validates `args_size == 0x18`, `args` non-null and the config pointer non-null, allocates the next handle, stores the first argument, writes the handle to `*result`, returns 0.
- `0x3`: validates `args_size == 0`; unknown handle gives `EBADF`; otherwise releases the handle and writes `0` to `*result`.
- anything else: returns `-1` with `errno = ENOSYS` and leaves `*result` untouched, which the guest reports as `0x8002004e`.
- A null `result` pointer gives `EFAULT`. The registered entry point sets `errno` through `__Error()` and returns `-1` on failure, matching the wrappers' convention. (The Phase 9 trace-only version returned the error code itself, which the wrappers would not interpret correctly.)
- The first 256 calls are traced with decoded string arguments into the JSON report.

This is the genuine kernel behaviour for object creation and destruction. It is not an IPMI server: no connection can be made, because nothing answers.
