# Integrated Kernel Module

Android ARM64 kernel module + Zygisk injection module for game reverse engineering.

## Project Structure

```
├── lsdriver/                    # Kernel module (lsdriver + wxshadow)
│   ├── lsdriver.c               # Module entry, dispatch, init/exit
│   ├── io_struct.h              # Shared-memory protocol definitions
│   ├── Makefile / Kconfig       # Kernel build system
│   ├── build_all.sh             # Full build (5.10 - 6.12)
│   ├── ddk_build_all.sh         # DDK-based build
│   │
│   ├── wxshadow.h               # W^X Shadow Hook: public API & types
│   ├── wxshadow_core.c          # Shadow page lifecycle
│   ├── wxshadow_pgtable.c       # PTE switch, TLB flush
│   ├── wxshadow_handlers.c      # BRK/step/fault handlers
│   ├── wxshadow_bp.c            # BP/patch/release via shared memory
│   ├── wxshadow_deps.c          # Dependency wrappers
│   ├── wxshadow_internal.h      # Internal helpers
│   │
│   ├── virtual_memory_rw.h      # Memory read/write (PTE map / AT instruction)
│   ├── virtual_memory_enum.h    # VMA walk, module/region enumeration
│   ├── break_point.h            # HW/PTE/step breakpoints
│   ├── arm64_hwdbg.h            # CPU debug register management
│   ├── arm64_ptedbg.h           # PTE UXN breakpoints
│   ├── arm64_stepdbg.h          # Single-step debugging
│   ├── arm64_syscalldbg.h       # System call monitor (do_el0_svc hook)
│   ├── arm64_env.h              # TLS/PACGA environment query
│   ├── arm64_reg.h / arm64_dbi_ghost.h / emulate_insn.h
│   ├── virtual_input.h          # Virtual touch injection
│   ├── virtual_gyro.h           # Virtual gyroscope (sendto hook)
│   ├── virtual_gnss.h           # Virtual GNSS (ioctl/Binder hook)
│   ├── inline_hook_frame.h      # ARM64 inline hook framework
│   ├── export_fun.h             # kallsyms_lookup via kprobe, CFI bypass
│   ├── hide_task.h / hide_kgsl.h # Anti-detection
│   └── arm64_decode/            # ARM64 instruction decoder
│
└── rfrida_zygisk/               # Zygisk injection module
    ├── main.cpp                  # Zygisk entry (API v2)
    ├── custom_loader.cpp         # Custom ELF linker (anonymous mmap)
    ├── zygisk.hpp                # Zygisk API header
    ├── build.sh                  # NDK build script
    └── module/src/main/cpp/      # Native sources
```

## lsdriver — Kernel Module

### Capabilities

| Feature | Mechanism |
|---------|-----------|
| **Memory R/W** | PTE remapping + AT S1E0R instruction |
| **Memory Scanning** | Value/pattern/pointer scan with saved address locking |
| **Hardware Breakpoints** | CPU BVR/BCR registers, up to 4-6 per core |
| **PTE Breakpoints** | UXN bit on page table entries, unlimited count |
| **Step Breakpoints** | MDSCR_EL1.SS single-step |
| **W^X Shadow Hook** | Shadow page table: reads original (r--), executes modified (--x) |
| **System Call Monitor** | do_el0_svc inline hook |
| **Virtual Input** | Touch/gyro/GNSS injection via kernel hooks |

### W^X Shadow Hook

```
set_bp   → allocate shadow page, write BRK, switch PTE to --x
patch    → write custom bytes to shadow page (e.g. NOP = d503201f)
release  → restore original PTE, free shadow page
get_state → enumerate active shadow pages / breakpoints / patches
```

- Up to 128 breakpoints per 4KB page
- Up to 4 register modifications per breakpoint
- Invisible to memory integrity checks and /proc/pid/maps

### Build (lsdriver)

Requires Android DDK Docker image or kernel source tree:

```bash
docker run --rm -v $(pwd)/lsdriver:/driver ghcr.io/ylarod/ddk:android15-6.6 \
  bash -c 'export PATH=/opt/ddk/clang/clang-r510928/bin:$PATH
  make ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
  -C /opt/ddk/kdir/android15-6.6 M=/driver -j$(nproc) modules'
```

Or: `cd lsdriver && bash ddk_build_all.sh` for all Android versions.

## rfrida_zygisk — Zygisk Injection Module

### How It Works

```
Zygote forks app → ZygiskNext loads rfrida_zygisk.so
  → preAppSpecialize: detects target package → opens agent.so via module dir fd
  → postAppSpecialize: custom ELF linker loads agent
    → Anonymous mmap (NOT dlopen)
    → Symbol resolution from system libraries
    → RELA relocation processing
    → init_array / entry point
```

**No ptrace. No dlopen. No /proc/pid/maps trace.**

| | ptrace Injection | Zygisk Injection |
|---|---|---|
| Attachment | ptrace(PTRACE_ATTACH) | Zygote fork hook |
| Loading | shellcode + memfd + android_dlopen_ext | Custom ELF linker (anonymous mmap) |
| /proc/maps | Visible | Completely invisible |
| TracerPid | Set | Not set |
| Detection vectors | ptrace, memfd, proc/maps | None |

### Build (rfrida_zygisk)

```bash
export ANDROID_NDK_HOME=$HOME/Library/Android/sdk/ndk/27.0.12077973
cd rfrida_zygisk && bash build.sh
```

### Deploy

```bash
# 1. Push files
adb push build/librfrida_zygisk.so /data/adb/modules/rfrida_zygisk/zygisk/arm64-v8a.so
adb push libagent.so /data/adb/modules/rfrida_zygisk/agent.so

# 2. Fix SELinux (required, resets after reboot)
adb shell su -c 'chcon -R u:object_r:system_file:s0 /data/adb/modules/rfrida_zygisk/'
adb shell su -c 'chcon u:object_r:system_lib_file:s0 /data/adb/modules/rfrida_zygisk/zygisk/arm64-v8a.so'

# 3. Reboot
adb reboot
```

### Target Configuration

Edit `rfrida_zygisk/module/src/main/cpp/main.cpp` and rebuild:

```cpp
static const char *kTargetPackages[] = {
    "com.example.game",
    nullptr
};
```

## MCP Server

A unified MCP server (`rfrida_mcp/server.py`, 43 tools) wraps both modules for Claude integration:

- `kernel_*` tools — kernel-level memory R/W, W^X Shadow Hook, breakpoints, syscall monitor
- `inject_*` tools — process injection, JS execution, function hooking, module enumeration
- `memory_read/memory_write/module_find_base` — smart routing: kernel first, fallback to injection

## References

This project builds upon:

- **[lsdriver](https://github.com/lsnbm/Linux-android-arm64)** — Android ARM64 kernel driver with memory R/W, breakpoints, system call monitoring, and virtual input injection. Uses a shared-memory protocol for userspace communication.

- **[wxshadow](https://github.com/kkkbbb/mkpms)** — W^X Shadow Memory KPM. Stealth code breakpoints using shadow page tables: target reads original (`r--`), executes modified shadow page (`--x`). The core algorithm was ported from KPM framework to lsdriver's inline hook framework.

- **[Zygisk-MyInjector](https://github.com/jiqiu2022/Zygisk-MyInjector)** — Reference Zygisk injection module. The `rfrida_zygisk` module follows the same Zygisk API v2 pattern with a custom ELF linker for stealth loading.

- **[ZygiskNext](https://github.com/5ec1cff/ZygiskNext)** — Standalone Zygisk implementation used as the runtime.

- **[rustFrida](https://github.com/kkkbbb/mkpms)** — The agent injected into target processes for JS scripting and Frida-compatible API.

## License

GPL-2.0
