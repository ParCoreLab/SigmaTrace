#include <climits>
#include <cstring>
#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <boost/program_options.hpp>

#include "Launcher.hpp"

#include "ProfilerArgs.hpp"
#include "ProfilerProviders.hpp"

namespace po = boost::program_options;

int main(int argc, char* argv[]) {
    try {
        // Getting the correct relative path to the shim .so
        std::array<char, PATH_MAX> readlink_buf{};
        const auto len = readlink("/proc/self/exe", readlink_buf.data(), readlink_buf.size() - 1);
        if (len == -1) {
            throw std::runtime_error("readlink failed: " + std::string(strerror(errno)));
        }
        const auto binary_dir = std::filesystem::path(readlink_buf.data()).parent_path();

        // Define command-line options
        po::options_description profiler_options("Profiler Options");
        profiler_options.add_options()
            ("help,h", "Display this help message")
            ("shim-dir", po::value<std::string>()->default_value(binary_dir.string()),
                "Directory containing the profiler shim libraries")
            ("load-nvbit", po::value<bool>()->default_value(false),
                "Enable NVBIT tracing")
            ("load-ibtrace", po::value<bool>()->default_value(true),
                "Enable InfiniBand tracing")
            ("load-mlx5dv", po::value<bool>()->default_value(true),
                "Enable MLX5 direct verbs tracing");

        // Hidden options for the program to launch
        po::options_description hidden_options("Hidden Options");
        hidden_options.add_options()
            ("program", po::value<std::string>(), "Program to launch")
            ("program-args", po::value<std::vector<std::string>>(), "Program arguments");

        // Combine all options
        po::options_description all_options;
        all_options.add(profiler_options).add(hidden_options);

        // Define positional arguments
        po::positional_options_description positional;
        positional.add("program", 1);
        positional.add("program-args", -1);

        // Parse command line
        po::variables_map vm;
        po::parsed_options parsed = po::command_line_parser(argc, argv)
            .options(all_options)
            .positional(positional)
            .allow_unregistered()  // Allow unrecognized options to pass through to child
            .run();

        po::store(parsed, vm);
        po::notify(vm);

        // Handle help
        if (vm.count("help")) {
            std::cout << "Usage: " << argv[0] << " [options] <program> [program-args...]" << std::endl;
            std::cout << std::endl;
            std::cout << profiler_options << std::endl;
            std::cout << "Example:" << std::endl;
            std::cout << "  " << argv[0] << " --load-nvbit=true ./my_program arg1 arg2" << std::endl;
            return 0;
        }

        // Validate that a program was specified
        if (!vm.count("program")) {
            std::cerr << "Error: No program specified to launch" << std::endl;
            std::cerr << "Usage: " << argv[0] << " [options] <program> [program-args...]" << std::endl;
            std::cerr << "Use --help for more information" << std::endl;
            return 1;
        }

        // Set up args for the shim and child process
        Profiler::ProfilerArgs profiler_args{
            .ShimDir = std::filesystem::path(vm["shim-dir"].as<std::string>()),
            .LoadNvbit = vm["load-nvbit"].as<bool>(),
            .LoadIbtrace = vm["load-ibtrace"].as<bool>(),
            .LoadMlx5dv = vm["load-mlx5dv"].as<bool>(),
        };

        std::vector<std::filesystem::path> shim_libs{};
        if (profiler_args.LoadIbtrace) {
            shim_libs.emplace_back(profiler_args.ShimDir / Profiler::SIGMA_IBVERBS_PROVIDER_PATH);
        }

        if (profiler_args.LoadMlx5dv) {
            shim_libs.emplace_back(profiler_args.ShimDir / Profiler::SIGMA_MLX5DV_PROVIDER_PATH);
        }

        // Main LD_PRELOAD shim
        // We need to add this at the end
        shim_libs.emplace_back(profiler_args.ShimDir / "libProfilerShim.so");

        const auto profiler_args_env = profiler_args.to_env();

        // Get program and its arguments
        const std::string program_bin = vm["program"].as<std::string>();
        
        // Build program_args from parsed options
        std::vector<const char*> program_args;
        program_args.push_back(program_bin.c_str());
        
        if (vm.count("program-args")) {
            const auto& prog_args = vm["program-args"].as<std::vector<std::string>>();
            for (const auto& arg : prog_args) {
                program_args.push_back(arg.c_str());
            }
        }

        const auto ret = Launcher::lauch_with_ld_preload(program_bin, program_args, shim_libs, profiler_args_env);
        return ret;
        
    } catch (const po::error& e) {
        std::cerr << "Error parsing arguments: " << e.what() << std::endl;
        std::cerr << "Use --help for usage information" << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
