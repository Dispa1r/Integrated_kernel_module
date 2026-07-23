# Integrated Kernel Module (lsdriver + wxshadow)

Android ARM64 kernel module with W^X Shadow Hook support.

## Features

- **Memory R/W**: Kernel-level process memory read/write via PTE mapping & AT instruction
- **Memory Scan**: Value/pattern/pointer scanning with saved address locking
- **Hardware Breakpoints**: ARM64 BVR/BCR registers with full register capture
- **PTE Breakpoints**: UXN-based page table breakpoints
- **W^X Shadow Hook** (NEW): Stealth code breakpoints & patches via shadow page tables
  - Process reads original code, executes modified code
  - Completely undetectable by memory integrity checks
  - Up to 128 breakpoints per page
- **System Call Monitor**: do_el0_svc inline hook
- **Virtual Input**: Touch injection, gyroscope, GNSS spoofing

## W^X Shadow Hook

```
set_bp  → create shadow page (--x), write BRK, switch PTE
patch   → write custom code to shadow page
release → restore original PTE, free shadow page
get_state → enumerate active shadow pages
```

## Build

Requires Android DDK or kernel source tree:

```bash
docker run --rm -v $(pwd):/driver ghcr.io/ylarod/ddk:android15-6.6 \
  bash -c 'export PATH=/opt/ddk/clang/clang-r510928/bin:$PATH
  make ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
  -C /opt/ddk/kdir/android15-6.6 M=/driver -j$(nproc) modules'
```

Or via `ddk_build_all.sh` for all Android versions (5.10 - 6.12).

## License

GPL-2.0
