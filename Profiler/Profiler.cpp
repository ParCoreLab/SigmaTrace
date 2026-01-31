#include <climits>
#include <cstring>
#include <unistd.h>

#include <filesystem>
#include <iostream>

#include "Launcher.hpp"

#include "ProfilerArgs.hpp"
#include "ProfilerProviders.hpp"

int main(int argc, char* argv[]) {
    // TODO: boost::program_options in the future
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <program> [args]" << std::endl;
        return 1;
    }

    // Getting the correct relative path to the shim .so
    std::array<char, PATH_MAX> readlink_buf{};
    const auto len = readlink("/proc/self/exe", readlink_buf.data(), readlink_buf.size() - 1);
    if (len == -1) {
        throw std::runtime_error("readlink failed: " + std::string(strerror(errno)));
    }
    const auto binary_dir = std::filesystem::path(readlink_buf.data()).parent_path();


    // Set up args for the shim and child process
    const Profiler::ProfilerArgs profiler_args{
            .ShimDir = binary_dir,
            .LoadIbtrace = true,
            .LoadMlx5dv = true,
    };

    std::vector<std::filesystem::path> shim_libs{};
    if (profiler_args.LoadIbtrace) {
        shim_libs.emplace_back(binary_dir / Profiler::SIGMA_IBVERBS_PROVIDER_PATH);
    }

    if (profiler_args.LoadMlx5dv) {
        shim_libs.emplace_back(binary_dir / Profiler::SIGMA_MLX5DV_PROVIDER_PATH);
    }

    // Main LD_PRELOAD shim
    // We need to add this at the end
    shim_libs.emplace_back(binary_dir / "libProfilerShim.so");

    const auto profiler_args_env = profiler_args.to_env();

    // Args for the launch
    const std::string program_bin = argv[1];
    std::vector<const char*> program_args(argv + 1, argv + argc);

    const auto ibv_lib = binary_dir / "providers/libibverbs_trace.so";

    const auto ret = Launcher::lauch_with_ld_preload(program_bin, program_args, shim_libs, profiler_args_env);
    return ret;
}
