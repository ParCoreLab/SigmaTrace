#pragma once

#include <functional>
#include <iostream>
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
    using CallbackType = std::function<void *(void *, void *, const char *)>;

    // Defined in Dlsym.cpp, shared across all modules linking to libsmart_dlsym.a
    extern void *(*real_dlopen)(const char *, int);
    extern void *(*real_dlsym)(void *, const char *);
    extern void *(*real_dlvsym)(void *, const char *, const char *);
    extern std::unordered_map<std::string, CallbackType> RegisteredSymbols;

    inline void register_symbol(std::string symbol_name, CallbackType callback) {
        if (RegisteredSymbols.contains(symbol_name)) {
            throw std::runtime_error("Symbol already registered");
        }

        RegisteredSymbols.emplace(std::move(symbol_name), std::move(callback));
    }

    template<typename Func>
    void *load_symbol(const Func &real_dlsym_func, void *handle, const char *symbol) {
        auto *real_fn = real_dlsym_func(handle, symbol);

        // TODO: should this return normally?
        if (!real_fn) {
            return real_fn;
        }

        const auto &registered_handler = RegisteredSymbols.find(symbol);
        if (registered_handler == RegisteredSymbols.end()) {
            return real_fn;
        }

        const auto &registered_callback = registered_handler->second;

        // Callback does some work and returns an overridden fun
        auto *new_fn = registered_callback(real_fn, handle, symbol);

        return new_fn;
    }
} // namespace smart_dlsym
