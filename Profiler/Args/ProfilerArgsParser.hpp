#pragma once

#include <iostream>

#include <boost/program_options.hpp>

#include "../../Events/EventRecorderConfig.hpp"
#include "../ProfilerFeatures.hpp"

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

        // If this is set, providers are disabled by default
        if (vm["load-none"].as<bool>()) {
            if (vm[flag].defaulted()) {
                return false;
            }
        }

        return vm[flag].as<bool>();
    }

    inline void validate(boost::any& v, const std::vector<std::string>& values, ProfilerTraceMode*, int) {
        po::validators::check_first_occurrence(v);
        const std::string& s = po::validators::get_single_string(values);

        if (s.empty() || s == "simple") {
            v = ProfilerTraceMode::Simple;
        } else if (s == "aggregate") {
            v = ProfilerTraceMode::Aggregate;
        } else if (s == "full") {
            v = ProfilerTraceMode::Full;
        } else {
            throw po::validation_error(po::validation_error::invalid_option_value);
        }
    }

    inline std::pair<ProfilerParseResult, ProfilerParsedArgs> parse_args(int argc, char* argv[]) {
        po::options_description general("General options");
        general.add_options()("help,h", "Print this help message and exit");

        // ================================================================
        // Provider options
        // TODO: Mention if the feature flag is off
        po::options_description providers("Provider options");
        // clang-format off
        providers.add_options()
        ("load-none", po::bool_switch()->default_value(false), "Load no trace providers by default (default: false)")
        ("load-ibtrace", po::value<bool>()->default_value(true)->implicit_value(true), "Load the ibverbs trace provider [RDMA_CORE] (default: true)")
        ("load-mlx5dv", po::value<bool>()->default_value(true)->implicit_value(true), "Load the mlx5dv trace provider [RDMA_CORE] (default: true)")
        ("load-cupti", po::value<bool>()->default_value(true)->implicit_value(true), "Load the CUPTI trace provider [CUDA] (default: true)")
        ("load-libfabric", po::value<bool>()->default_value(true)->implicit_value(true), "Load the libfabric trace provider [LIBFABRIC] (default: true)")
        ("load-ucx", po::value<bool>()->default_value(true)->implicit_value(true), "Load the UCX trace provider [UCX] (default: true)")
        ("enable-doorbell-hijack", po::bool_switch()->default_value(false), "Enable doorbell hijacking [RDMA_CORE] (default: false)");
        // clang-format on
        // ================================================================

        // ================================================================
        po::options_description profiler_trace_control("Profiler trace collection options");

        ProfilerTraceMode profiler_trace_mode;
        // clang-format off
        profiler_trace_control.add_options()
        ("rank-hint", po::value<uint32_t>(), "Communication rank hint for visualization")
        ("trace-mode", po::value<ProfilerTraceMode>(&profiler_trace_mode)->default_value(ProfilerTraceMode::Simple), "Set Profiler trace mode (simple, aggregate, full)");
        // clang-format off
        // ================================================================

        // ================================================================
        // Output options
        po::options_description opt_trace("Profiler trace output options");
        // For default values
        const Events::RuntimeConfig events_config{};
        // clang-format off
        opt_trace.add_options()
        ("output-dir,o", po::value<std::string>()->default_value(events_config.OutputDirectory), "Directory where traces will be recorded")
        ("output-overwrite,f", po::bool_switch()->default_value(false), "Overwrite --output-dir if exists. Useful for multi-process scenarios");
        // clang-format on
        // ================================================================

        // ================================================================
        // Hidden options
        // clang-format off
        po::options_description hidden("Hidden options");
        hidden.add_options()
        ("program", po::value<std::string>(), "Program to run")
        ("program-args", po::value<std::vector<std::string>>(), "Arguments for the program");
        // clang-format on
        // ================================================================

        po::options_description visible("Usage: profiler [options] <program> [program args]\n\nOptions");
        visible.add(general).add(providers);
        visible.add(general).add(profiler_trace_control);
        visible.add(general).add(opt_trace);

        po::options_description all;
        all.add(general);
        all.add(providers);
        all.add(profiler_trace_control);
        all.add(opt_trace);
        all.add(hidden);

        po::positional_options_description positional;
        positional.add("program", 1);
        positional.add("program-args", -1);

        // Parse
        po::variables_map vm;
        int program_argv_index = -1;
        try {
            po::parsed_options parsed =
                    po::command_line_parser(argc, argv)
                            .options(all)
                            .positional(positional)
                            .allow_unregistered()
                            .style(po::command_line_style::unix_style | po::command_line_style::allow_long_disguise)
                            .run();

            auto program_option_it = parsed.options.end();
            int argv_cursor = 1;

            // Remove program options that showed up after the program
            // For example ./Profiler -f ./app --load-ucx
            for (auto it = parsed.options.begin(); it != parsed.options.end(); ++it) {
                if (it->string_key == "program") {
                    program_argv_index = argv_cursor;
                    program_option_it = it;
                    break;
                }

                for (const auto& token: it->original_tokens) {
                    if (argv_cursor < argc && token == argv[argv_cursor]) {
                        ++argv_cursor;
                    }
                }
            }

            if (program_option_it != parsed.options.end()) {
                parsed.options.erase(std::next(program_option_it), parsed.options.end());
            }

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
        if (program_argv_index >= 0) {
            for (int i = program_argv_index + 1; i < argc; ++i) {
                program_args.emplace_back(argv[i]);
            }
        } else if (vm.contains("program-args")) {
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

        profiler_args.TraceMode = vm["trace-mode"].as<ProfilerTraceMode>();

        if (!vm["rank-hint"].empty()) {
            profiler_args.RankHint.emplace(vm["rank-hint"].as<decltype(profiler_args.RankHint)::value_type>());
        }

        // Validation
        if (std::filesystem::exists(profiler_args.OutputDirectory)) {
            if (profiler_args.OutputOverwrite) {
                std::cout << "[Profiler] Overwriting " << profiler_args.OutputDirectory << std::endl;
            } else {
                std::cerr << profiler_args.OutputDirectory << " is not empty " << std::endl;
                std::cerr << "[Profiler] Please either pass --output-dir or --output-overwrite (-f)" << std::endl;
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
