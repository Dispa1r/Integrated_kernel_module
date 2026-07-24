# rfrida-zygisk: Zygisk Module for Stealth Agent Injection

Injects the rustFrida agent.so into target Android processes using Zygisk,
with a **custom ELF linker** for complete stealth.

## How It Works

```
Zygote forks app process
  → ZygiskNext loads rfrida_zygisk.so
  → preAppSpecialize: detects target package, opens agent.so via module dir fd
  → postAppSpecialize: loads agent via custom ELF linker
    → Anonymous mmap (NOT dlopen)
    → Resolves symbols from system libraries
    → Applies RELA relocations
    → Calls init_array / entry point
```

**No ptrace. No dlopen. No /proc/pid/maps trace.**

## Comparison

| | ptrace Injection | Zygisk Injection |
|---|---|---|
| Attachment | ptrace(PTRACE_ATTACH) | Zygote fork hook |
| Loading | shellcode + android_dlopen_ext | Custom ELF linker |
| /proc/maps | Visible | Completely invisible |
| Detection | TracerPid, /proc/maps scan | None |

## Build

```bash
export ANDROID_NDK_HOME=$HOME/Library/Android/sdk/ndk/27.0.12077973
cd rfrida_zygisk && bash build.sh
```

## Deploy

1. Copy `build/librfrida_zygisk.so` to `/data/adb/modules/rfrida_zygisk/zygisk/arm64-v8a.so`
2. Copy `libagent.so` to `/data/adb/modules/rfrida_zygisk/agent.so`
3. Create `/data/adb/modules/rfrida_zygisk/module.prop`
4. Fix SELinux: `chcon -R u:object_r:system_file:s0 /data/adb/modules/rfrida_zygisk/`
5. Reboot

## Target Configuration

Edit `main.cpp` and rebuild to change target packages:

```cpp
static const char *kTargetPackages[] = {
    "com.example.game",
    nullptr
};
```

## References

- [ZygiskNext](https://github.com/5ec1cff/ZygiskNext) - Standalone Zygisk implementation
- [Zygisk-MyInjector](https://github.com/jiqiu2022/Zygisk-MyInjector) - Reference Zygisk injector
- [rustFrida](https://github.com/kkkbbb/mkpms) - The agent.so to inject

## Stealth Hook (W^X Shadow)

When `Java.setStealth(true)` is enabled, all native hooks use the wxshadow
stealth path:

```
Agent hook_engine_mem.c
  → HTTP POST to lsdriver bridge (127.0.0.1:19494)
    → shared memory → lsdriver.ko kernel module
      → wxshadow_handle_patch() → shadow page table → W^X split
```

This makes hooks completely undetectable by memory integrity checks:
- Process reads: original bytes (page mapped r-- on read fault)
- Process executes: modified bytes from shadow page (--x)
