/*
 * The insane code below serves to, at runtime, parse ELF to get the
 * real symbols for dlsym and dlvsym. Since we have hooks for both of them, it's not possible
 * to load them any other way without avoiding recursion.
 */

#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>

#include "SymbolResolver.h"

#ifndef STT_GNU_IFUNC
#define STT_GNU_IFUNC 10
#endif

// Use dl_iterate_phdr to find symbols in libc without relying on internal glibc symbols
struct SymbolLookupCtx {
    const char* lib_pattern;
    const char* symbol_name;
    void* result;
};

static uint32_t gnu_hash(const char* name) {
    uint32_t h = 5381;
    for (auto s = reinterpret_cast<const unsigned char*>(name); *s; s++) {
        h = (h << 5) + h + *s;
    }
    return h;
}

static void* lookup_symbol_gnu_hash(const ElfW(Sym) * symtab, const char* strtab, const uint32_t* gnu_hash_table,
                                    const ElfW(Half) * versym, const char* name, ElfW(Addr) load_bias) {

    const uint32_t namehash = gnu_hash(name);

    const uint32_t nbuckets = gnu_hash_table[0];
    const uint32_t symoffset = gnu_hash_table[1];
    const uint32_t bloom_size = gnu_hash_table[2];

    const uint64_t* bloom = reinterpret_cast<const uint64_t*>(&gnu_hash_table[4]);
    const uint32_t* buckets = reinterpret_cast<const uint32_t*>(&bloom[bloom_size]);
    const uint32_t* chain = &buckets[nbuckets];

    uint32_t idx = buckets[namehash % nbuckets];
    if (idx < symoffset) {
        return nullptr;
    }

    while (true) {
        const uint32_t hash = chain[idx - symoffset];
        if ((hash | 1) == (namehash | 1)) {
            const ElfW(Sym)* sym = &symtab[idx];

            // Skip undefined symbols (imported references, not exports)
            if (sym->st_shndx != SHN_UNDEF) {
                if (strcmp(strtab + sym->st_name, name) == 0) {

                    // Symbol Versioning Check
                    // If DT_VERSYM exists, check if the 15th bit (0x8000) is set.
                    // If it is set, this is a hidden/compatibility symbol (e.g., @GLIBC_2.2.5).
                    // If it is 0, this is the default symbol (e.g., @@GLIBC_2.34).
                    bool is_default_version = true;
                    if (versym != nullptr) {
                        is_default_version = (versym[idx] & 0x8000) == 0;
                    }

                    if (is_default_version) {
                        void* sym_addr = reinterpret_cast<void*>(load_bias + sym->st_value);

                        // Handle GNU Indirect Functions (IFUNC)
                        // Both ELF32 and ELF64 use the lower 4 bits of st_info for the type.
                        if ((sym->st_info & 0xf) == STT_GNU_IFUNC) {
                            // If it's an IFUNC, the address points to a resolver function.
                            // We must execute it to get the actual function pointer.
                            auto resolver = reinterpret_cast<void* (*) ()>(sym_addr);
                            return resolver();
                        }

                        return sym_addr;
                    }
                }
            }
        }
        if (hash & 1) {
            break;
        }
        idx++;
    }
    return nullptr;
}

static int dl_iterate_callback(dl_phdr_info* info, size_t /*size*/, void* data) {
    auto* ctx = static_cast<SymbolLookupCtx*>(data);

    if (!info->dlpi_name || strstr(info->dlpi_name, ctx->lib_pattern) == nullptr) {
        return 0;
    }

    const ElfW(Dyn)* dyn = nullptr;
    for (size_t i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) {
        return 0;
    }

    const ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    const uint32_t* gnu_hash = nullptr;
    const ElfW(Half)* versym = nullptr;

    // Helper to resolve potential load bias issues in the DYNAMIC array
    auto fix_addr = [&](ElfW(Addr) ptr) -> uintptr_t {
        if (ptr < info->dlpi_addr) {
            return info->dlpi_addr + ptr;
        }
        return ptr;
    };

    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:
                symtab = reinterpret_cast<const ElfW(Sym)*>(fix_addr(d->d_un.d_ptr));
                break;
            case DT_STRTAB:
                strtab = reinterpret_cast<const char*>(fix_addr(d->d_un.d_ptr));
                break;
            case DT_GNU_HASH:
                gnu_hash = reinterpret_cast<const uint32_t*>(fix_addr(d->d_un.d_ptr));
                break;
            case DT_VERSYM:
                versym = reinterpret_cast<const ElfW(Half)*>(fix_addr(d->d_un.d_ptr));
                break;
            default:
                break;
        }
    }

    if (!symtab || !strtab || !gnu_hash) {
        return 0;
    }

    // Notice we pass info->dlpi_addr down to handle the load bias inside the lookup
    void* sym = lookup_symbol_gnu_hash(symtab, strtab, gnu_hash, versym, ctx->symbol_name, info->dlpi_addr);
    if (sym) {
        ctx->result = sym;
        return 1; // Stop iterating, we found it
    }
    return 0;
}

extern void* find_symbol_in_lib(const char* lib_pattern, const char* symbol_name) {
    SymbolLookupCtx ctx{lib_pattern, symbol_name, nullptr};
    dl_iterate_phdr(dl_iterate_callback, &ctx);
    return ctx.result;
}

// ================================================================
// RTLD_NEXT emulation
// ================================================================
struct NextSymbolCtx {
    void* caller_addr;
    const char* symbol_name;
    void* result;
    bool passed_caller; // true once we've walked past the caller's library
};

// Check if an address falls within any PT_LOAD segment of this library
static bool addr_in_library(const dl_phdr_info* info, void* addr) {
    auto a = reinterpret_cast<ElfW(Addr)>(addr);
    for (size_t i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_LOAD) {
            ElfW(Addr) lo = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
            ElfW(Addr) hi = lo + info->dlpi_phdr[i].p_memsz;
            if (a >= lo && a < hi) {
                return true;
            }
        }
    }
    return false;
}

static int dl_iterate_next_callback(dl_phdr_info* info, size_t /*size*/, void* data) {
    auto* ctx = static_cast<NextSymbolCtx*>(data);

    // Find the caller's library and skip it
    if (!ctx->passed_caller) {
        if (addr_in_library(info, ctx->caller_addr)) {
            ctx->passed_caller = true;
        }
        return 0; // keep iterating
    }

    // Search subsequent libraries for the symbol
    const ElfW(Dyn)* dyn = nullptr;
    for (size_t i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) {
        return 0;
    }

    const ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    const uint32_t* gnu_hash_table = nullptr;
    const ElfW(Half)* versym = nullptr;

    auto fix_addr = [&](ElfW(Addr) ptr) -> uintptr_t {
        if (ptr < info->dlpi_addr) {
            return info->dlpi_addr + ptr;
        }
        return ptr;
    };

    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:
                symtab = reinterpret_cast<const ElfW(Sym)*>(fix_addr(d->d_un.d_ptr));
                break;
            case DT_STRTAB:
                strtab = reinterpret_cast<const char*>(fix_addr(d->d_un.d_ptr));
                break;
            case DT_GNU_HASH:
                gnu_hash_table = reinterpret_cast<const uint32_t*>(fix_addr(d->d_un.d_ptr));
                break;
            case DT_VERSYM:
                versym = reinterpret_cast<const ElfW(Half)*>(fix_addr(d->d_un.d_ptr));
                break;
            default:
                break;
        }
    }

    if (!symtab || !strtab || !gnu_hash_table) {
        return 0;
    }

    void* sym = lookup_symbol_gnu_hash(symtab, strtab, gnu_hash_table, versym, ctx->symbol_name, info->dlpi_addr);
    if (sym) {
        ctx->result = sym;
        return 1;
    }
    return 0;
}

extern void* find_symbol_next(void* caller_addr, const char* symbol_name) {
    NextSymbolCtx ctx{caller_addr, symbol_name, nullptr, false};
    dl_iterate_phdr(dl_iterate_next_callback, &ctx);
    return ctx.result;
}
