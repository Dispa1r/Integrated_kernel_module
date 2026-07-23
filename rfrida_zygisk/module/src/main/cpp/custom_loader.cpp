/* Custom ELF loader for ARM64 Android.
   Loads an ELF shared library via anonymous mmap without using dlopen,
   making it invisible to /proc/pid/maps scanners and dl_iterate_phdr.
   Based on the approach from MyInjector's mylinker. */

#include "custom_loader.h"
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#define TAG "rfrida-zygisk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Internal handle structure */
struct loaded_lib {
    char *path;
    uint8_t *base;          /* load base address */
    size_t total_size;      /* total mapped size */
    Elf64_Addr entry;       /* entry point if ET_EXEC, else 0 */
    Elf64_Dyn *dynamic;     /* .dynamic section */
    Elf64_Sym *symtab;
    const char *strtab;
    Elf64_Rela *jmprel;
    size_t pltrelsz;
    Elf64_Rela *rela;
    size_t relasz;
    void (**init_array)();
    size_t init_arraysz;
    void (**fini_array)();
    size_t fini_arraysz;
    std::vector<loaded_lib *> deps;
};

/* ------------------------------------------------------------------ */
/* ELF reading helpers                                                 */
/* ------------------------------------------------------------------ */
static bool read_file(const char *path, uint8_t **data, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return false; }
    *size = st.st_size;
    *data = (uint8_t *)mmap(nullptr, *size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return *data != MAP_FAILED;
}

static void release_file(uint8_t *data, size_t size) {
    munmap(data, size);
}

template<typename T> static T* elf_offset(uint8_t *base, Elf64_Off off) {
    return reinterpret_cast<T*>(base + off);
}

/* ------------------------------------------------------------------ */
/* Segment mapping                                                     */
/* ------------------------------------------------------------------ */
static bool map_segments(uint8_t *elf, size_t size, loaded_lib *lib) {
    auto *ehdr = reinterpret_cast<Elf64_Ehdr *>(elf);

    /* First pass: calculate total size needed */
    Elf64_Addr min_vaddr = UINTPTR_MAX, max_vaddr = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        auto *ph = elf_offset<Elf64_Phdr>(elf, ehdr->e_phoff + i * sizeof(Elf64_Phdr));
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr < min_vaddr) min_vaddr = ph->p_vaddr;
        Elf64_Addr end = ph->p_vaddr + ph->p_memsz;
        if (end > max_vaddr) max_vaddr = end;
    }

    /* Align */
    min_vaddr &= ~(PAGE_SIZE - 1);
    max_vaddr = (max_vaddr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    size_t total = max_vaddr - min_vaddr;
    lib->total_size = total;

    /* Allocate memory with a fixed base using MAP_FIXED_NOREPLACE */
    /* First try without fixed to see what the system gives us */
    void *probe = mmap(nullptr, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probe == MAP_FAILED) {
        LOGE("initial mmap failed");
        return false;
    }
    munmap(probe, total);

    /* Allocate at the probed address + some offset to avoid conflicts */
    uint8_t *map_base = (uint8_t *)mmap((void *)((uintptr_t)probe + 0x10000000),
                                        total, PROT_NONE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map_base == MAP_FAILED) {
        /* Retry without address hint */
        map_base = (uint8_t *)mmap(nullptr, total, PROT_NONE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (map_base == MAP_FAILED) {
            LOGE("allocation failed");
            return false;
        }
    }
    lib->base = map_base;

    /* Second pass: map each segment */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        auto *ph = elf_offset<Elf64_Phdr>(elf, ehdr->e_phoff + i * sizeof(Elf64_Phdr));
        if (ph->p_type != PT_LOAD) continue;

        Elf64_Addr seg_start = ph->p_vaddr & ~(PAGE_SIZE - 1);
        size_t seg_offset_in_page = ph->p_vaddr - seg_start;
        uint8_t *dest = map_base + seg_start - min_vaddr;
        size_t seg_size = seg_offset_in_page + ph->p_filesz;
        seg_size = (seg_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        int prot = 0;
        if (ph->p_flags & PF_R) prot |= PROT_READ;
        if (ph->p_flags & PF_W) prot |= PROT_WRITE;
        if (ph->p_flags & PF_X) prot |= PROT_EXEC;

        /* Map segment */
        if (ph->p_filesz > 0) {
            void *m = mmap(dest, seg_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (m == MAP_FAILED) { LOGE("segment mmap failed"); return false; }
            memcpy(dest + seg_offset_in_page, elf + ph->p_offset, ph->p_filesz);
        }

        /* If memsz > filesz, zero the remainder (BSS) */
        if (ph->p_memsz > ph->p_filesz) {
            size_t bss_start = seg_offset_in_page + ph->p_filesz;
            size_t bss_end = seg_offset_in_page + ph->p_memsz;
            bss_end = (bss_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            if (bss_end > seg_size) {
                size_t extra = bss_end - seg_size;
                void *ext = mmap(dest + seg_size, extra,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                if (ext == MAP_FAILED) { LOGE("bss mmap failed"); return false; }
            }
            memset(dest + seg_offset_in_page + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
        }

        /* Set final permissions */
        mprotect(dest, seg_size, prot);
    }

    lib->entry = ehdr->e_entry ? (Elf64_Addr)(map_base + ehdr->e_entry - min_vaddr) : 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* Dynamic section parsing                                             */
/* ------------------------------------------------------------------ */
static bool parse_dynamic(loaded_lib *lib) {
    if (!lib->dynamic) return false;

    for (auto *dyn = lib->dynamic; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
        case DT_SYMTAB:  lib->symtab = (Elf64_Sym *)(lib->base + dyn->d_un.d_ptr); break;
        case DT_STRTAB:  lib->strtab = (const char *)(lib->base + dyn->d_un.d_ptr); break;
        case DT_JMPREL:  lib->jmprel = (Elf64_Rela *)(lib->base + dyn->d_un.d_ptr); break;
        case DT_PLTRELSZ: lib->pltrelsz = dyn->d_un.d_val; break;
        case DT_RELA:    lib->rela = (Elf64_Rela *)(lib->base + dyn->d_un.d_ptr); break;
        case DT_RELASZ:  lib->relasz = dyn->d_un.d_val; break;
        case DT_INIT_ARRAY:
            lib->init_array = (void (**)())(lib->base + dyn->d_un.d_ptr); break;
        case DT_INIT_ARRAYSZ: lib->init_arraysz = dyn->d_un.d_val; break;
        case DT_FINI_ARRAY:
            lib->fini_array = (void (**)())(lib->base + dyn->d_un.d_ptr); break;
        case DT_FINI_ARRAYSZ: lib->fini_arraysz = dyn->d_un.d_val; break;
        }
    }
    return lib->symtab && lib->strtab;
}

/* ------------------------------------------------------------------ */
/* GNU hash lookup (for finding symbols by name)                       */
/* ------------------------------------------------------------------ */
static Elf64_Sym *gnu_hash_lookup(loaded_lib *lib, const char *name) {
    /* Parse .gnu.hash or fall back to .hash */
    /* For simplicity, do linear scan through symtab */
    /* A real implementation would parse the hash table */
    /* Symtab linear scan with strtab */
    if (!lib->symtab || !lib->strtab) return nullptr;

    /* Walk symtab entries. symtab size is not directly available from .dynamic.
       Estimate by checking symtab extent from .dynstr size or using nchain from DT_GNU_HASH. */
    for (size_t i = 0; i < 4096; i++) {
        const char *sym_name = lib->strtab + lib->symtab[i].st_name;
        if (sym_name && strcmp(sym_name, name) == 0) {
            return &lib->symtab[i];
        }
    }
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* Symbol resolution - look up in loaded libs + system                 */
/* ------------------------------------------------------------------ */
static void *resolve_symbol(loaded_lib *lib, const char *name, uint32_t sym_type) {
    /* 1. Search in this lib's deps first */
    for (auto *dep : lib->deps) {
        auto *sym = gnu_hash_lookup(dep, name);
        if (sym && sym->st_value) return dep->base + sym->st_value;
    }

    /* 2. Search in this lib itself */
    auto *sym = gnu_hash_lookup(lib, name);
    if (sym && sym->st_value) return lib->base + sym->st_value;

    /* 3. Search in already loaded libraries via RTLD_DEFAULT */
    void *addr = dlsym(RTLD_DEFAULT, name);
    if (addr) return addr;

    /* 4. Try necessary system libraries */
    const char *sys_libs[] = {"libc.so", "libdl.so", "libm.so", "liblog.so",
                              "libstdc++.so", "libandroid.so", nullptr};
    for (int i = 0; sys_libs[i]; i++) {
        void *h = dlopen(sys_libs[i], RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            addr = dlsym(h, name);
            if (addr) return addr;
        }
    }

    return nullptr;
}

/* ------------------------------------------------------------------ */
/* Relocation processing                                               */
/* ------------------------------------------------------------------ */
static bool apply_relocations(loaded_lib *lib) {

    auto do_rela = [&](Elf64_Rela *rela, size_t count) -> bool {
        for (size_t i = 0; i < count; i++) {
            auto *r = &rela[i];
            uint32_t type = ELF64_R_TYPE(r->r_info);
            uint32_t sym_idx = ELF64_R_SYM(r->r_info);
            Elf64_Addr *target = (Elf64_Addr *)(lib->base + r->r_offset);
            Elf64_Sxword addend = r->r_addend;

            Elf64_Addr sym_addr = 0;
            if (sym_idx > 0 && lib->symtab) {
                const char *sym_name = lib->strtab + lib->symtab[sym_idx].st_name;
                void *resolved = resolve_symbol(lib, sym_name, ELF64_ST_TYPE(lib->symtab[sym_idx].st_info));
                if (resolved) {
                    sym_addr = (Elf64_Addr)resolved;
                } else if (ELF64_ST_BIND(lib->symtab[sym_idx].st_info) == STB_WEAK) {
                    sym_addr = 0; /* weak symbol */
                } else {
                    LOGE("unresolved: %s", sym_name);
                    /* Don't fail - some symbols resolve lazily */
                    continue;
                }
            }

            switch (type) {
            case R_AARCH64_NONE: break;
            case R_AARCH64_ABS64:  *target = sym_addr + addend; break;
            case R_AARCH64_GLOB_DAT: *target = sym_addr + addend; break;
            case R_AARCH64_JUMP_SLOT: *target = sym_addr + addend; break;
            case R_AARCH64_RELATIVE: *target = (Elf64_Addr)(lib->base) + addend; break;
            case R_AARCH64_IRELATIVE: {
                auto ifunc = (Elf64_Addr (*)())(lib->base + addend);
                *target = ifunc();
                break;
            }
            default:
                /* Skip unknown relocations */
                break;
            }
        }
        return true;
    };

    /* Process RELA */
    if (lib->rela && lib->relasz) {
        if (!do_rela(lib->rela, lib->relasz / sizeof(Elf64_Rela))) return false;
    }

    /* Process JMPREL (PLT) */
    if (lib->jmprel && lib->pltrelsz) {
        if (!do_rela(lib->jmprel, lib->pltrelsz / sizeof(Elf64_Rela))) return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Load dependencies (DT_NEEDED)                                       */
/* ------------------------------------------------------------------ */
static bool load_deps(loaded_lib *lib, uint8_t *elf, Elf64_Dyn *dynamic) {
    /* Walk .dynamic to find DT_NEEDED entries */
    Elf64_Off strtab_off = 0;
    for (auto *dyn = dynamic; dyn->d_tag != DT_NULL; dyn++) {
        if (dyn->d_tag == DT_STRTAB) strtab_off = dyn->d_un.d_val;
    }
    const char *strtab = (const char *)(lib->base + strtab_off);

    for (auto *dyn = dynamic; dyn->d_tag != DT_NULL; dyn++) {
        if (dyn->d_tag != DT_NEEDED) continue;

        const char *name = strtab + dyn->d_un.d_val;
        /* Load dependency via system dlopen (these ARE system libs, OK to be visible) */
        void *h = dlopen(name, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
        if (h) {
            /* Already loaded, mark as dependency */
        } else {
            /* Try to load it */
            h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
        }
        if (!h) {
            LOGE("dep load failed: %s", name);
            /* Don't fail - some libs may be optional */
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void *custom_dlopen(const char *path) {
    LOGI("custom loading: %s", path);

    uint8_t *elf_data;
    size_t elf_size;
    if (!read_file(path, &elf_data, &elf_size)) {
        LOGE("cannot read: %s", path); return nullptr;
    }

    auto *ehdr = reinterpret_cast<Elf64_Ehdr *>(elf_data);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_machine != EM_AARCH64) {
        LOGE("not arm64 ELF: %s", path);
        release_file(elf_data, elf_size); return nullptr;
    }

    auto *lib = new loaded_lib();
    lib->path = strdup(path);

    /* Map segments */
    if (!map_segments(elf_data, elf_size, lib)) {
        LOGE("map failed"); delete lib; release_file(elf_data, elf_size); return nullptr;
    }

    /* Find .dynamic and parse it */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        auto *ph = elf_offset<Elf64_Phdr>(elf_data, ehdr->e_phoff + i * sizeof(Elf64_Phdr));
        if (ph->p_type == PT_DYNAMIC) {
            lib->dynamic = (Elf64_Dyn *)(lib->base + ph->p_vaddr - (ehdr->e_phnum > 0 ?
                (elf_offset<Elf64_Phdr>(elf_data, ehdr->e_phoff)->p_vaddr & ~(PAGE_SIZE - 1)) : 0));
            break;
        }
    }

    if (!lib->dynamic) {
        LOGE("no .dynamic"); delete lib; release_file(elf_data, elf_size); return nullptr;
    }

    /* Parse .dynamic entries into lib struct */
    if (!parse_dynamic(lib)) {
        LOGE("dynamic parse failed"); delete lib; release_file(elf_data, elf_size); return nullptr;
    }

    /* Load dependencies first */
    load_deps(lib, elf_data, lib->dynamic);

    /* Apply relocations */
    if (!apply_relocations(lib)) {
        LOGE("reloc failed"); delete lib; release_file(elf_data, elf_size); return nullptr;
    }

    /* Run init functions */
    if (lib->init_array) {
        size_t count = lib->init_arraysz / sizeof(void (*)());
        for (size_t i = 0; i < count; i++) {
            if (lib->init_array[i]) lib->init_array[i]();
        }
    }

    release_file(elf_data, elf_size);

    /* Clear instruction cache for the newly loaded code */
    __builtin___clear_cache((char *)lib->base, (char *)(lib->base + lib->total_size));

    LOGI("loaded at %p size 0x%zx", lib->base, lib->total_size);
    return lib;
}

void *custom_dlsym(void *handle, const char *name) {
    if (!handle || !name) return nullptr;
    auto *lib = static_cast<loaded_lib *>(handle);
    auto *sym = gnu_hash_lookup(lib, name);
    if (sym && sym->st_value) return lib->base + sym->st_value;

    /* Fall back to RTLD_DEFAULT for system symbols */
    return dlsym(RTLD_DEFAULT, name);
}

void *custom_dlopen_fd(int fd) {
    if (fd < 0) return nullptr;
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    return custom_dlopen(path);
}

uintptr_t custom_base(void *handle) {
    if (!handle) return 0;
    return (uintptr_t)static_cast<loaded_lib *>(handle)->base;
}