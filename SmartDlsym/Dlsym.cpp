#include <cassert>
#include <cstdio>

#include <dlfcn.h>

#include <functional>
#include <string>

#include "SymbolResolver.h"

#include "Dlsym.hpp"

namespace smart_dlsym {
    static constexpr bool LogDebug{false};

    void* DL_RTLD_DEFAULT = RTLD_DEFAULT;

    void* DL_RTLD_NEXT = RTLD_NEXT;
    const int DL_RTLD_NOW = RTLD_NOW;
    const int DL_RTLD_GLOBAL = RTLD_GLOBAL;
    const int DL_RTLD_NOLOAD = RTLD_NOLOAD;

    void* (*real_dlopen)(const char*, int) = nullptr;
    void* (*real_dlsym)(void*, const char*) = nullptr;
    void* (*real_dlvsym)(void*, const char*, const char*) = nullptr;
    void* (*real_dlclose)(void*) = nullptr;

    // If you see an undebuggable floating point exception it's this
    std::unordered_map<std::string, CallbackType>* RegisteredSymbols{nullptr};

    inline void* on_symbol_load(void* real_symbol, void* handle, const char* symbol_name) {
        if (!real_symbol) {
            return real_symbol;
        }

        if (!RegisteredSymbols) {
            return real_symbol;
        }

        const auto& registered_handler = RegisteredSymbols->find(symbol_name);
        if (registered_handler == RegisteredSymbols->end()) {
            return real_symbol;
        }

        const auto& registered_callback = registered_handler->second;

        // Callback does some work and returns an overridden fun
        auto* new_fn = registered_callback(real_symbol, handle, symbol_name);

        return new_fn;
    }
} // namespace smart_dlsym

// On glibc < 2.34, dl* functions live in libdl.so; on >= 2.34 they're in libc.so
static void* find_dl_symbol(const char* name) {
    void* sym = find_symbol_in_lib("libc", name);
    if (!sym) {
        sym = find_symbol_in_lib("libdl", name);
    }
    return sym;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wprio-ctor-dtor"
// If you see an undebuggable double free, however, it's this. Don't load this module more than once !!!!!
static void __attribute__((constructor(0))) init() {
    smart_dlsym::real_dlopen = reinterpret_cast<decltype(smart_dlsym::real_dlopen)>(find_dl_symbol("dlopen"));

    smart_dlsym::real_dlsym = reinterpret_cast<decltype(smart_dlsym::real_dlsym)>(find_dl_symbol("dlsym"));
    smart_dlsym::real_dlvsym = reinterpret_cast<decltype(smart_dlsym::real_dlvsym)>(find_dl_symbol("dlvsym"));

    smart_dlsym::real_dlclose = reinterpret_cast<decltype(smart_dlsym::real_dlclose)>(find_dl_symbol("dlclose"));

    if (!smart_dlsym::real_dlopen || !smart_dlsym::real_dlsym || !smart_dlsym::real_dlvsym ||
        !smart_dlsym::real_dlclose) {
        fprintf(stderr, "Failed to load real real dl functions\n");
        std::abort();
    }

    smart_dlsym::RegisteredSymbols = new std::unordered_map<std::string, smart_dlsym::CallbackType>();
}
#pragma GCC diagnostic pop

static void log_dlsym(const char* symbol, void* caller, void* result, void* glibc_result) {
    if constexpr (smart_dlsym::LogDebug) {
        Dl_info caller_info = {};
        Dl_info ours_info = {};
        Dl_info glibc_info = {};
        dladdr(caller, &caller_info);
        if (result)
            dladdr(result, &ours_info);
        if (glibc_result)
            dladdr(glibc_result, &glibc_info);
        fprintf(stderr, "[dlsym] RTLD_NEXT '%s' caller=%s ours=%s(%p) glibc=%s(%p)%s\n", symbol,
                caller_info.dli_fname ? caller_info.dli_fname : "?",
                ours_info.dli_fname ? ours_info.dli_fname : "(nil)", result,
                glibc_info.dli_fname ? glibc_info.dli_fname : "(nil)", glibc_result,
                (result && result != glibc_result) ? " MISMATCH" : "");
    }
}

extern "C" void* dlsym(void* handle, const char* symbol) {
    if (!smart_dlsym::real_dlsym) {
        init();
    }

    // Note[RTLD_NEXT]: RTLD_NEXT must resolve relative to the *original caller*, not us.
    // Forwarding real_dlsym(RTLD_NEXT, ...) would search after our library
    // instead of after the caller's, which can cause infinite recursion when
    // a wrapper library tries to find the real symbol behind it.
    // __builtin_return_address(0) must be called here, not in a helper,
    // to capture the actual caller of dlsym.
    if (handle == RTLD_NEXT) {
        void* caller = __builtin_return_address(0);
        void* result = find_symbol_next(caller, symbol);
        // Always call real_dlsym to let glibc populate its internal state
        // (lazy binding, symbol caches, etc.) even though we prefer our result.
        void* glibc_result = smart_dlsym::real_dlsym(handle, symbol);

        // Debug
        log_dlsym(symbol, caller, result, glibc_result);

        auto* real_symbol = result ? result : glibc_result;
        return smart_dlsym::on_symbol_load(real_symbol, handle, symbol);
    }

    auto* real_symbol = smart_dlsym::real_dlsym(handle, symbol);
    return smart_dlsym::on_symbol_load(real_symbol, handle, symbol);
}

extern "C" void* dlvsym(void* handle, const char* symbol, const char* version) {
    if (!smart_dlsym::real_dlvsym) {
        init();
    }

    // See Note[RTLD_NEXT]
    if (handle == RTLD_NEXT) {
        void* caller = __builtin_return_address(0);
        void* result = find_symbol_next(caller, symbol);
        void* glibc_result = smart_dlsym::real_dlvsym(handle, symbol, version);

        // Debug
        log_dlsym(symbol, caller, result, glibc_result);

        auto* real_symbol = smart_dlsym::real_dlvsym(handle, symbol, version);
        return smart_dlsym::on_symbol_load(real_symbol, handle, symbol);
    }

    auto* real_symbol = smart_dlsym::real_dlvsym(handle, symbol, version);
    return smart_dlsym::on_symbol_load(real_symbol, handle, symbol);
}
