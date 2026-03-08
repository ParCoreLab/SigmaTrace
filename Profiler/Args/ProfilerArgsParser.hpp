#pragma once

#include <iostream>

#include <boost/program_options.hpp>

#include "../ProfilerFeatures.hpp"

#include "../../Events/EventRecorderConfig.hpp"
#include "ProfilerArgs.hpp"

namespace po = boost::program_options;

namespace Profiler {
    struct ProfilerParsedArgs {
        ProfilerArgs profiler_args;
        std::string program_bin;
        std::vector<std::string> program_args;
    };

    enum class ProfilerParseResult {
        Parsed = 0,
        Help = 1,
        Failed = 2
    };

    // Checks if a disabled flag is supplied or enabled
    template<bool FeatureEnabled>
    static bool get_set_feature_flag(po::variables_map& vm, const std::string& flag, std::string_view feature_name) {
        if constexpr (!FeatureEnabled) {
            // Explicitly set
            if (!vm[flag].defaulted()) {
                std::cerr << "error: --" << flag << " was set, but this build was compiled without " << feature_name
                          << " support.\n";

                std::exit(1);
            }

            return false;
        }

        // Feature is enabled, return whatever was parsed
        return vm[flag].as<bool>();
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

        // For default values
        const Events::RuntimeConfig events_config{};
        // clang-format off
        po::options_description opt_trace("Profiler trace options");
        opt_trace.add_options()
        ("output-dir,o", po::value<std::string>()->default_value(events_config.OutputDirectory), "Directory where traces will be recorded")
        ("output-overwrite,f", po::bool_switch()->default_value(false), "Overwrite --output-dir if exists. Useful for multi-process schenarioasdaa");
        // clang-format on

        // clang-format off
        po::options_description hidden("Hidden options");
        hidden.add_options()
        ("program", po::value<std::string>(), "Program to run")
        ("program-args", po::value<std::vector<std::string>>(), "Arguments for the program");
        // clang-format on

        po::options_description visible("Usage: profiler [options] <program> [program args]\n\nOptions");
        visible.add(general).add(providers);
        visible.add(general).add(opt_trace);

        po::options_description all;
        all.add(general);
        all.add(providers);
        all.add(opt_trace);
        all.add(hidden);

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

        std::vector<std::string> program_args{};
        if (vm.contains("program-args")) {
            program_args = vm["program-args"].as<std::vector<std::string>>();
        }

        // Default initialize because some fields are set in the caller
        ProfilerArgs profiler_args{};

        // ================================================================
        // Check against feature flags
        constexpr auto HasCudaAndRdma{Features::HasRdmaCore && Features::HasCUDA};

        // Provider args
        profiler_args.LoadIbtrace = get_set_feature_flag<Features::HasRdmaCore>(vm, "load-ibtrace", "RDMA_CORE");
        profiler_args.LoadMlx5dv = get_set_feature_flag<Features::HasRdmaCore>(vm, "load-mlx5dv", "RDMA_CORE");
        profiler_args.EnableDoorbellHijack =
                get_set_feature_flag<HasCudaAndRdma>(vm, "enable-doorbell-hijack", "RDMA_CORE && HAS_CUDA");
        profiler_args.LoadCupti = get_set_feature_flag<Features::HasCUDA>(vm, "load-cupti", "HAS_CUDA");
        profiler_args.LoadLibfabric =
                get_set_feature_flag<Features::HasLibfabric>(vm, "load-libfabric", "HAS_LIBFABRIC");
        profiler_args.LoadUcx = get_set_feature_flag<Features::HasUCX>(vm, "load-ucx", "HAS_UCX");

        profiler_args.OutputDirectory = vm["output-dir"].as<std::string>();
        profiler_args.OutputOverwrite = vm["output-overwrite"].as<bool>();

        // Validation
        if (std::filesystem::exists(profiler_args.OutputDirectory)) {
            if (profiler_args.OutputOverwrite) {
                std::cout << "[Profiler] Overwriting " << profiler_args.OutputDirectory << std::endl;
            } else {
                std::cerr << profiler_args.OutputDirectory << " is not empty " << std::endl;
                std::cerr << "[Profiler] Please either pass --output-dir or --output-overwrite" << std::endl;
                return {ProfilerParseResult::Failed, {}};
            }
        }

        ProfilerParsedArgs args{
                .profiler_args = std::move(profiler_args),
                .program_bin = std::move(program_bin),
                .program_args = std::move(program_args),
        };

        return {ProfilerParseResult::Parsed, std::move(args)};
    }
} // namespace Profiler
