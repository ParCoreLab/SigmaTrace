#include <climits>
#include <cstring>
#include <unistd.h>

#include <filesystem>
#include <iostream>

#include "Launcher.hpp"

#include "ProfilerArgs.hpp"

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

    // Main LD_PRELOAD shim
    const std::string ShimLib{binary_dir / "libProfilerShim.so"};

    // Set up args for the shim and child process
    const Profiler::ProfilerArgs profiler_args{.ShimDir = binary_dir};
    const auto profiler_args_env = profiler_args.to_env();

    // Args for the launch
    const std::string program_bin = argv[1];
    std::vector<const char*> program_args(argv + 1, argv + argc);
    const auto shimLibs = std::views::single(ShimLib);

    const auto ret = Launcher::lauch_with_ld_preload(program_bin, program_args, shimLibs, profiler_args_env);
    return ret;
}
