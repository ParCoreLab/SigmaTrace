/*
 * The insane code below serves to, at runtime, parse ELF to get the
 * real symbols for dlsym and dlvsym. Since we have hooks for both of them, it's not possible
 * to load them any other way without avoiding recursion.
 */

#include <dlfcn.h>
#include <link.h>
#include <string.h>

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
                                    const char* name) {
    const uint32_t namehash = gnu_hash(name);

    const uint32_t nbuckets = gnu_hash_table[0];
    const uint32_t symoffset = gnu_hash_table[1];
    const uint32_t bloom_size = gnu_hash_table[2];
    // const uint32_t bloom_shift = gnu_hash_table[3];

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
            if (strcmp(strtab + sym->st_name, name) == 0) {
                return reinterpret_cast<void*>(sym->st_value);
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

    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB: {
                symtab = reinterpret_cast<const ElfW(Sym)*>(d->d_un.d_ptr);
                break;
            }
            case DT_STRTAB: {
                strtab = reinterpret_cast<const char*>(d->d_un.d_ptr);
                break;
            }
            case DT_GNU_HASH: {
                gnu_hash = reinterpret_cast<const uint32_t*>(d->d_un.d_ptr);
                break;
            }
            default:
                break;
        }
    }

    if (!symtab || !strtab || !gnu_hash) {
        return 0;
    }

    void* sym = lookup_symbol_gnu_hash(symtab, strtab, gnu_hash, ctx->symbol_name);
    if (sym) {
        ctx->result = reinterpret_cast<void*>(info->dlpi_addr + reinterpret_cast<uintptr_t>(sym));
        return 1;
    }
    return 0;
}

extern void* find_symbol_in_lib(const char* lib_pattern, const char* symbol_name) {
    SymbolLookupCtx ctx{lib_pattern, symbol_name, nullptr};
    dl_iterate_phdr(dl_iterate_callback, &ctx);
    return ctx.result;
}
