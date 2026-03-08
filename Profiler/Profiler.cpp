#include <climits>
#include <cstring>
#include <unistd.h>

#include <filesystem>
#include <iostream>

#include "Launcher.hpp"

#include "Args/ProfilerArgs.hpp"
#include "Args/ProfilerArgsParser.hpp"
#include "ProfilerProviders.hpp"

int main(int argc, char* argv[]) {
    // Parse the args
    auto [res, parsed_args] = Profiler::parse_args(argc, argv);
    switch (res) {
        case Profiler::ProfilerParseResult::Failed: {
            std::exit(1);
            break;
        }
        case Profiler::ProfilerParseResult::Help: {
            std::exit(0);
        }
        case Profiler::ProfilerParseResult::Parsed: {
            break;
        }
        default: {
            std::cerr << "ERROR: Unchecked ProfilerParseResult" << std::endl;
            std::exit(0);
        }
    }

    // ================================================================
    std::array<char, PATH_MAX> readlink_buf{};
    const auto len = readlink("/proc/self/exe", readlink_buf.data(), readlink_buf.size() - 1);
    if (len == -1) {
        throw std::runtime_error("readlink failed: " + std::string(strerror(errno)));
    }
    const auto binary_dir = std::filesystem::path(readlink_buf.data()).parent_path();
    // ================================================================

    // ================================================================
    // Set LD_PRELOAD shims
    const auto profiler_args = parsed_args.profiler_args;
    std::vector<std::filesystem::path> shim_libs{};

#if SIGMA_HAS_RDMA_CORE
    if (profiler_args.LoadIbtrace) {
        shim_libs.emplace_back(binary_dir / Profiler::SIGMA_IBVERBS_PROVIDER_PATH);
    }

    if (profiler_args.LoadMlx5dv) {
        shim_libs.emplace_back(binary_dir / Profiler::SIGMA_MLX5DV_PROVIDER_PATH);
    }
#endif

    // CUPTI doesn't use LD_PRELOAD symbols
    // if (profiler_args.LoadCupti) {}

#if SIGMA_HAS_LIBFABRIC
    if (profiler_args.LoadLibfabric) {
        shim_libs.emplace_back(binary_dir / Profiler::SIGMA_LIBFABRIC_PROVIDER_PATH);
    }
#endif

#if SIGMA_HAS_UCX
    if (profiler_args.LoadUcx) {
        shim_libs.emplace_back(binary_dir / Profiler::SIGMA_UCX_PROVIDER_PATH);
    }
#endif

    // Master LD_PRELOAD shim — must come last
    shim_libs.emplace_back(binary_dir / "libProfilerShim.so");

    // ================================================================
    const auto profiler_args_env = profiler_args.to_env();

    const std::string program_bin = parsed_args.program_bin;
    const auto& program_args = parsed_args.program_args;

    return Launcher::lauch_with_ld_preload(program_bin, program_args, shim_libs, profiler_args_env);
}
