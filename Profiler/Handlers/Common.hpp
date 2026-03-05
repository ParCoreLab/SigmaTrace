#pragma once

#include <filesystem>
#include <iostream>
#include <string_view>

#include "Dlsym.hpp"

#include "../ProfilerShim.hpp"

static inline void* internal_load_lib(std::string_view lib_name) {
    const auto mode = smart_dlsym::DL_RTLD_NOW | smart_dlsym::DL_RTLD_GLOBAL;
    void* ibv_shim = smart_dlsym::real_dlopen(lib_name.data(), mode);
    if (!ibv_shim) {
        std::cerr << "Unable to load " << lib_name << std::endl;
        std::abort();
    }

    return ibv_shim;
}

template<typename T>
static inline ProviderHandle<T> load_provider(const std::filesystem::path& lib_name) {
    if (!std::filesystem::exists(lib_name)) {
        std::cerr << "Unable to load " << lib_name << " (file doesn't exist)" << std::endl;
        std::abort();
    }

    auto* handle = internal_load_lib(lib_name.c_str());
    auto* factory = reinterpret_cast<T* (*) ()>(smart_dlsym::real_dlsym(handle, "sigma_provider_instance"));
    if (!factory) {
        std::cerr << "Unable to load " << lib_name << " (no sigma_provider_instance)" << std::endl;
        std::abort();
    }

    return ProviderHandle<T>{factory(), handle};
}
