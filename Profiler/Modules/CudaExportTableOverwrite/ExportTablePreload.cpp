#include <cassert>
#include <sys/mman.h>

#include <string>
#include <tuple>
#include <unordered_map>

#include <cuda.h>

#include "Dlsym.hpp"

#include "ExportTableOverwrite.hpp"

using CuGetExportTable_t = CUresult (*)(const void** ppExportTable, const CUuuid* pExportTableId);
using CuGetProcAddress_v2_t = CUresult (*)(const char* symbol, void** pfn, int cudaVersion, cuuint64_t flags,
                                           CUdriverProcAddressQueryResult* symbolStatus);

CuGetExportTable_t real_cuGetExportTable = nullptr;
CuGetProcAddress_v2_t real_cuGetProcAddress_v2 = nullptr;

namespace Profiler::CudaExportTable {
    struct OverwriteEntry {
        CallbackType overwrite_callback;
        void* real_cu_func{nullptr};
        bool is_replaced{false};
    };

    // If you see an undebuggable floating point exception it's this
    // Use init_priority(101) to ensure it's constructed before other static objects
    // TODO: should probably be std::string_view
    __attribute__((init_priority(101))) std::unordered_map<std::string, OverwriteEntry> RegisteredOverwrites{};

    void* get_real_cu_func(const std::string& cu_func_name) {
        const auto& real_fn_entry = RegisteredOverwrites.find(cu_func_name);
        if (real_fn_entry == RegisteredOverwrites.end()) {
            return nullptr;
        }

        return real_fn_entry->second.real_cu_func;
    }

    void register_overwrite(std::string cu_func_name, CallbackType replacement_func) {
        if (RegisteredOverwrites.contains(cu_func_name)) {
            const auto error = "cu function " + cu_func_name + " already registered";
            throw std::runtime_error(error);
        }

        OverwriteEntry entry{
                .overwrite_callback = replacement_func,
        };
        RegisteredOverwrites.emplace(std::move(cu_func_name), std::move(entry));
    }
} // namespace Profiler::CudaExportTable

using Profiler::CudaExportTable::RegisteredOverwrites;

// TODO: not using pExportTableId
extern "C" CUresult cuGetExportTable(const void** ppExportTable, const CUuuid* pExportTableId) {
    // Never seen it not loaded with dlsym, but just in case
    if (!real_cuGetExportTable) {
        real_cuGetExportTable = reinterpret_cast<CuGetExportTable_t>(
                smart_dlsym::real_dlsym(smart_dlsym::DL_RTLD_NEXT, "cuGetExportTable"));

        if (!real_cuGetExportTable) {
            std::cerr << "Could not find cuGetExportTable" << std::endl;
            std::abort();
        }
    }

    const CUresult res = real_cuGetExportTable(ppExportTable, pExportTableId);
    if (res != CUDA_SUCCESS) {
        std::cerr << "WARNING: real_cuGetExportTable returned error (" << res << ")" << std::endl;
        return res;
    }

    if (RegisteredOverwrites.empty()) {
        std::cerr << "WARNING: RegisteredOverwrites for CudaExportTable is empty!" << std::endl;
        return res;
    }

    void** orig_table = static_cast<void**>(const_cast<void*>(*ppExportTable));

    // This estimates the number of entries
    size_t entry_count = 0;
    for (entry_count = 0; true /* entry_count < 500000 */; entry_count++) {
        const auto val = reinterpret_cast<uintptr_t>(orig_table[entry_count]);

        // Heuristic
        if (val == 0xffffffffffffffff) {
            break;
        }
    }

    // For mapping cu function pointers to RegisteredOverwrites entries
    // TODO: std::reference_wrapper maybe
    std::unordered_map<void*, decltype(RegisteredOverwrites)::iterator> real_cu_to_overwrite;
    real_cu_to_overwrite.reserve(RegisteredOverwrites.size());

    for (auto overwrite_it = RegisteredOverwrites.begin(); overwrite_it != RegisteredOverwrites.end(); ++overwrite_it) {
        const auto& func_name = overwrite_it->first;

        // if (func_name.ends_with("_v2")) {
        //     continue;
        // }

        // TODO: RTLD_NEXT?
        void* real_cu_func = smart_dlsym::real_dlsym(smart_dlsym::DL_RTLD_DEFAULT, func_name.c_str());
        if (real_cu_func == nullptr) {
            std::cerr << "WARNING: " << func_name << " not found in libcuda.so\n";
            continue;
        }

        overwrite_it->second.real_cu_func = real_cu_func;
        real_cu_to_overwrite.emplace(real_cu_func, overwrite_it);
    }

    // In-place patching
    for (size_t i = 0; i < entry_count; i++) {
        const auto& cu_overwrite_entry = real_cu_to_overwrite.find(orig_table[i]);
        if (cu_overwrite_entry == real_cu_to_overwrite.end()) {
            continue;
        }

        auto page = reinterpret_cast<uintptr_t>(&orig_table[i]) & ~0xFFFUL;
        std::ignore = mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE);

        /* We could hack a bit more to pass extra args but it's too much voodoo
         * // Allocate executable memory
         * void* thunk = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
         * code[i++] = 0x4D; code[i++] = 0x89; code[i++] = 0xC1;  // mov r9,
         */
        auto& overwrite_entry = cu_overwrite_entry->second->second;
        orig_table[i] = overwrite_entry.overwrite_callback;
        overwrite_entry.is_replaced = true;
    }

    return res;
}

extern "C" CUresult cuGetProcAddress_v2(const char* symbol, void** pfn, int cudaVersion, cuuint64_t flags,
                                        CUdriverProcAddressQueryResult* symbolStatus) {
    if (!real_cuGetProcAddress_v2) {
        real_cuGetProcAddress_v2 = reinterpret_cast<CuGetProcAddress_v2_t>(
                smart_dlsym::real_dlsym(smart_dlsym::DL_RTLD_NEXT, "cuGetProcAddress_v2"));

        if (!real_cuGetProcAddress_v2) {
            std::cerr << "Could not find cuGetProcAddress_v2" << std::endl;
            std::abort();
        }
    }

    const auto res = real_cuGetProcAddress_v2(symbol, pfn, cudaVersion, flags, symbolStatus);

    const std::string symbol_str{symbol};

    // Note[proc_address_v2]: Special case. Should probably not route non-v2 back here but it's fine for now.
    // God knows why nvidia made two identical versions
    if (symbol_str == "cuGetProcAddress") {
        *pfn = reinterpret_cast<void*>(cuGetProcAddress_v2);
    }

    const auto& overwrite_entry = RegisteredOverwrites.find(symbol_str);
    if (overwrite_entry != RegisteredOverwrites.end()) {
        /*
         * I don't know why, but we have to replace the function here too, even if it was replaced in cuGetExportTable
         * Otherwise we get CUDA_ERROR_INVALID_CONTEXT sometimes
         */
        if (*pfn != overwrite_entry->second.overwrite_callback /* && !overwrite_entry->second.is_replaced */) {
            overwrite_entry->second.real_cu_func = *pfn;
        }

        *pfn = overwrite_entry->second.overwrite_callback;
        overwrite_entry->second.is_replaced = true;
    }

    return res;
}

static void __attribute__((constructor)) init() {
    smart_dlsym::register_symbol("cuGetExportTable", [](void* real_func, auto&&...) -> void* {
        real_cuGetExportTable = reinterpret_cast<CuGetExportTable_t>(real_func);

        return reinterpret_cast<void*>(cuGetExportTable);
    });

    // See Note[proc_address_v2]
    smart_dlsym::register_symbol("cuGetProcAddress", [](void* real_func, auto&&...) -> void* {
        real_cuGetProcAddress_v2 = reinterpret_cast<CuGetProcAddress_v2_t>(real_func);

        return reinterpret_cast<void*>(cuGetProcAddress_v2);
    });

    smart_dlsym::register_symbol("cuGetProcAddress_v2", [](void* real_func, auto&&...) -> void* {
        real_cuGetProcAddress_v2 = reinterpret_cast<CuGetProcAddress_v2_t>(real_func);

        return reinterpret_cast<void*>(cuGetProcAddress_v2);
    });
}
