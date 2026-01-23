#include <cassert>

#include <dlfcn.h>

#include <functional>
#include <string>

#include "SymbolResolver.h"

#include "Dlsym.hpp"

namespace smart_dlsym {
    const int DL_RTLD_NOW = RTLD_NOW;
    const int DL_RTLD_GLOBAL = RTLD_GLOBAL;

    void* (*real_dlopen)(const char*, int) = nullptr;
    void* (*real_dlsym)(void*, const char*) = nullptr;
    void* (*real_dlvsym)(void*, const char*, const char*) = nullptr;
    // If you see an undebuggable floating point exception it's this
    // Use init_priority(101) to ensure it's constructed before other static objects
    __attribute__((init_priority(101))) std::unordered_map<std::string, CallbackType> RegisteredSymbols{};
} // namespace smart_dlsym

static void __attribute__((constructor(101))) init() {
    smart_dlsym::real_dlopen =
            reinterpret_cast<decltype(smart_dlsym::real_dlopen)>(find_symbol_in_lib("libc", "dlopen"));

    smart_dlsym::real_dlsym = reinterpret_cast<decltype(smart_dlsym::real_dlsym)>(find_symbol_in_lib("libc", "dlsym"));
    smart_dlsym::real_dlvsym =
            reinterpret_cast<decltype(smart_dlsym::real_dlvsym)>(find_symbol_in_lib("libc", "dlvsym"));
}

extern "C" void* dlsym(void* handle, const char* symbol) {
    // TODO: don't use assert
    assert(smart_dlsym::real_dlsym);

    return smart_dlsym::load_symbol(smart_dlsym::real_dlsym, handle, symbol);
}

extern "C" void* dlvsym(void* handle, const char* symbol, const char* version) {
    // TODO: don't use assert
    assert(smart_dlsym::real_dlvsym);

    // TODO: inline lambda might be better
    auto dlsym_func = std::bind(smart_dlsym::real_dlvsym, std::placeholders::_1, std::placeholders::_2, version);

    return smart_dlsym::load_symbol(dlsym_func, handle, symbol);
}
