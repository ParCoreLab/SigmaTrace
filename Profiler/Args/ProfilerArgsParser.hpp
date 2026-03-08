#pragma once

#include <iostream>

#include <boost/program_options.hpp>

#include "../ProfilerFeatures.hpp"

#include "ProfilerArgs.hpp"

namespace po = boost::program_options;

namespace Profiler {
    struct ProfilerParsedArgs {
        ProfilerArgs profiler_args;
        std::string program_bin;
        std::vector<const char*> program_args;
    };

    enum class ProfilerParseResult {
        Parsed = 0,
        Help = 1,
        Failed = 2
    };

    // Checks if a disabled flag is supplied or enabled
    template<bool FeatureEnabled>
    static bool get_set_feature_flag(po::variables_map& vm, const std::string& flag, std::string_view feature_name) {
        if constexpr (FeatureEnabled) {
            if (!vm[flag].defaulted()) {
                std::cerr << "error: --" << flag << " was set, but this build was compiled without " << feature_name
                          << " support.\n";

                std::exit(1);
            }

            return false;
        }

        return true;
    }

    inline std::pair<ProfilerParseResult, ProfilerParsedArgs> parse_args(int argc, char* argv[]) {
        po::options_description general("General options");
        general.add_options()("help,h", "Print this help message and exit");

        // clang-format off
        // Mention if the feature flag is off
        po::options_description providers("Provider options");
        providers.add_options()
        ("load-ibtrace", po::value<bool>()->default_value(true)->implicit_value(true), "Load the ibverbs trace provider [RDMA_CORE] (default: true)")
        ("load-mlx5dv", po::value<bool>()->default_value(true)->implicit_value(true), "Load the mlx5dv trace provider [RDMA_CORE] (default: true)")
        ("enable-doorbell-hijack", po::value<bool>()->default_value(false)->implicit_value(true), "Enable doorbell hijacking [RDMA_CORE] (default: false)")
        ("load-cupti", po::value<bool>()->default_value(true)->implicit_value(true), "Load the CUPTI trace provider [CUDA] (default: true)")
        ("load-libfabric", po::value<bool>()->default_value(true)->implicit_value(true), "Load the libfabric trace provider [LIBFABRIC] (default: true)")
        ("load-ucx", po::value<bool>()->default_value(true)->implicit_value(true), "Load the UCX trace provider [UCX] (default: true)");
        // clang-format on

        // clang-format off
        po::options_description hidden("Hidden options");
        hidden.add_options()
        ("program", po::value<std::string>(), "Program to run")
        ("program-args", po::value<std::vector<std::string>>(), "Arguments for the program");
        // clang-format on

        po::options_description visible("Usage: profiler [options] <program> [program args]\n\nOptions");
        visible.add(general).add(providers);

        po::options_description all;
        all.add(general).add(providers).add(hidden);

        po::positional_options_description positional;
        positional.add("program", 1);
        positional.add("program-args", -1);

        // Parse
        po::variables_map vm;
        try {
            auto parsed =
                    po::command_line_parser(argc, argv)
                            .options(all)
                            .positional(positional)
                            .style(po::command_line_style::unix_style | po::command_line_style::allow_long_disguise)
                            .run();
            po::store(parsed, vm);
            po::notify(vm);
        } catch (const po::error& e) {
            std::cerr << "error: " << e.what() << "\n\n" << visible << "\n";
            return {ProfilerParseResult::Failed, {}};
        }

        if (vm.contains("help")) {
            std::cout << visible << "\n";
            return {ProfilerParseResult::Help, {}};
        }

        if (!vm.contains("program")) {
            std::cerr << "error: no program specified\n\n" << visible << "\n";
            return {ProfilerParseResult::Failed, {}};
        }

        // ================================================================

        // ================================================================
        // Args are parsed successfully
        std::string program_bin = vm["program"].as<std::string>();

        std::vector<const char*> program_args{program_bin.c_str()};
        if (vm.contains("program-args")) {
            const auto& parsed_program_args = vm["program-args"].as<std::vector<const char*>>();
            program_args.insert(program_args.end(), std::begin(parsed_program_args), std::end(parsed_program_args));
        }

        // Default initialize because some fields are set in the caller
        ProfilerArgs profiler_args{};

        // ================================================================
        // Check against feature flags
        constexpr auto HasCudaAndRdma{Features::HasRdmaCore && Features::HasCUDA};

        profiler_args.LoadIbtrace = get_set_feature_flag<Features::HasRdmaCore>(vm, "load-ibtrace", "RDMA_CORE");
        profiler_args.LoadMlx5dv = get_set_feature_flag<Features::HasRdmaCore>(vm, "load-mlx5dv", "RDMA_CORE");
        profiler_args.EnableDoorbellHijack =
                get_set_feature_flag<HasCudaAndRdma>(vm, "enable-doorbell-hijack", "RDMA_CORE && HAS_CUDA");
        profiler_args.LoadCupti = get_set_feature_flag<Features::HasCUDA>(vm, "load-cupti", "HAS_CUDA");
        profiler_args.LoadLibfabric = get_set_feature_flag<Features::HasUCX>(vm, "load-ucx", "HAS_UCX");
        profiler_args.LoadUcx = get_set_feature_flag<Features::HasLibfabric>(vm, "load-libfabric", "HAS_LIBFABRIC");

        ProfilerParsedArgs args{.profiler_args = std::move(profiler_args),
                                .program_bin = std::move(program_bin),
                                .program_args = std::move(program_args)};

        return {ProfilerParseResult::Parsed, std::move(args)};
    }
} // namespace Profiler
