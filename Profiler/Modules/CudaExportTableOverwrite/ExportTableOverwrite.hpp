#pragma once

#include <string>

// Very similar to smart dlsym
namespace Profiler::CudaExportTable {
    using CallbackType = void*;

    void* get_real_cu_func(const std::string& cu_func_name);

    void register_overwrite(std::string cu_func_name, CallbackType replacement_func);
} // namespace Profiler::CudaExportTable
