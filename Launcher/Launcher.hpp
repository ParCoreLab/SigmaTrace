#pragma once

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <string>
#include <vector>

#include "Helpers/RangeOf.hpp"

// TODO: not portable
extern char** environ;

namespace Launcher {
    using Helpers::RangeOf;

    int fork_launch(const std::string& program_bin, const std::vector<const char*>& program_args,
                    const std::vector<const char*>& program_env);

    int lauch_with_ld_preload(const std::string& program_bin, const RangeOf<const char*> auto& c_program_args,
                              const RangeOf<std::string> auto& shim_paths,
                              const RangeOf<std::string> auto& extra_env = {}) {
        const auto pwd = std::filesystem::current_path();

        // This owns the strings
        std::vector<std::string> ld_library_paths;
        ld_library_paths.reserve(std::ranges::size(shim_paths));

        std::ranges::transform(shim_paths, std::back_inserter(ld_library_paths),
                               [&](const std::string& lib_path) -> std::string {
            return "LD_PRELOAD=" + (pwd / lib_path).string();
        });

        std::vector<const char*> c_program_env;
        c_program_env.reserve(ld_library_paths.size());

        std::ranges::transform(ld_library_paths, std::back_inserter(c_program_env),
                               [](const std::string& s) -> const char* {
            return s.c_str();
        });

        // Add profiler-specific environment (if any)
        for (const auto& e: extra_env) {
            c_program_env.push_back(e.c_str());
        }

        // Add existing environment
        for (int i = 0; environ[i] != nullptr; ++i) {
            c_program_env.push_back(environ[i]);
        }

        // Null-terminate for execve
        c_program_env.push_back(nullptr);

        // Convert args range to vector with null terminator
        std::vector<const char*> args_vec(std::ranges::begin(c_program_args), std::ranges::end(c_program_args));
        args_vec.push_back(nullptr);

        return fork_launch(program_bin, args_vec, c_program_env);
    }
} // namespace Launcher
