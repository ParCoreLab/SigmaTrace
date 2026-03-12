#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>

/*
 * TODO: Write explanation.
 * This library can only be linked against ONCE in the entire program of library
 *
 * TODO: Performance test
 */

namespace smart_dlsym {
    // Trying to avoid making downstream users having to #include <dlfcn.h>
    extern void* DL_RTLD_DEFAULT;
    extern void* DL_RTLD_NEXT;
    extern const int DL_RTLD_NOW;
    extern const int DL_RTLD_GLOBAL;
    extern const int DL_RTLD_NOLOAD;

    using CallbackType = std::function<void*(void*, void*, const char*)>;

    // Defined in Dlsym.cpp, shared across all modules linking to libsmart_dlsym.a
    extern void* (*real_dlopen)(const char*, int);
    extern void* (*real_dlsym)(void*, const char*);
    extern void* (*real_dlvsym)(void*, const char*, const char*);
    extern void* (*real_dlclose)(void*);
    extern std::unordered_map<std::string, CallbackType>* RegisteredSymbols;

    inline void register_symbol(std::string symbol_name, CallbackType callback) {
        if (RegisteredSymbols->contains(symbol_name)) {
            throw std::runtime_error("Symbol already registered");
        }

        RegisteredSymbols->emplace(std::move(symbol_name), std::move(callback));
    }
} // namespace smart_dlsym
