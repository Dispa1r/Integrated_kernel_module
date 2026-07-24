# Agent Changes (rustFrida)

These changes are in the agent Rust code (separate repo: mkpms/rustFrida).
They must be applied + agent rebuilt for full stealth hook support.

## 1. Crash handler removal (stability)
**File**: `agent/src/lib.rs:113`
```rust
// install_crash_handlers();  // 和游戏 CrashSight SDK 冲突，移除
```
SIGSEGV/SIGBUS/SIGABRT handlers conflict with game's CrashSight SDK,
causing random game crashes. Removing them has NO impact on agent
functionality - they only collected crash dumps for debugging.

## 2. callNative dladdr check removal (function calling)
**File**: `quickjs-hook/src/jsapi/hook_api/functions.rs:158-168`
```rust
// dladdr check REMOVED: custom linker and some system libraries
// may not pass dladdr validation even when the page is valid executable code.
```
The dladdr() check was too restrictive - it rejected valid code addresses
in libraries loaded by the custom linker or with non-standard mappings.

## 3. wxshadow HTTP bridge adapter (stealth hook)
**File**: `quickjs-hook/src/hook_engine_mem.c`
```c
// NEW: wxshadow_patch() and wxshadow_release() try lsdriver HTTP bridge
// (127.0.0.1:19494) first, with KPM prctl as fallback.
// Uses raw TCP socket + HTTP POST to /api/rpc with JSON body.
```
The original code used prctl(PR_WXSHADOW_PATCH/RELEASE) which requires
the KPM wxshadow module. Now it first tries the lsdriver HTTP bridge,
enabling stealth hooks without KernelPatch.

**Required includes added**: `<sys/socket.h>`, `<netinet/in.h>`, etc.

## 4. wxshadow HTTP bridge bug fixes (2026-07-24)
**File**: `quickjs-hook/src/hook_engine_mem.c`

Two critical bugs in the HTTP bridge code that caused wxshadow to always fail:

### Bug 1: Wrong Content-Length for target.select
```c
// BEFORE (broken):
"{\"operation\":\"target.select\",\"params\":{\"pid\":%d}}",
(int)strlen(body) + 50, getpid());  // body = wxshadow.patch body, NOT select body!
// Content-Length was ~350 bytes for a ~56-byte body → server waits for more data → timeout

// AFTER (fixed):
char tgt_body[128];
int tgt_body_len = snprintf(tgt_body, sizeof(tgt_body), ...);
"...Content-Length: %d\r\n...%s", tgt_body_len, tgt_body);
```

### Bug 2: Reusing socket closed by "Connection: close"
```c
// BEFORE (broken): both target.select and wxshadow.patch sent on same socket
// target.select has "Connection: close" → server closes socket after first response
// wxshadow.patch write() goes to already-closed socket → empty response
// Fallback KPM prctl fails → falls back to mprotect → wxshadow never used

// AFTER (fixed): two separate sockets
// Socket 1: target.select → close
// Socket 2: wxshadow.patch → read response → close
```

## 5. Rebuild command
```bash
cd mkpms/rustFrida
cargo build --release --target aarch64-linux-android
adb push target/aarch64-linux-android/release/libagent.so /data/local/tmp/
adb shell su -c 'cp /data/local/tmp/libagent.so /data/adb/modules/rfrida_zygisk/agent.so'
```
