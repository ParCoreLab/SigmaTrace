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

        // Build LD_PRELOAD=path1:path2:... string
        // Maybe in a better future we'll use C++23 views::join_with
        std::string ld_preload_str = "LD_PRELOAD=";
        bool is_first_ld_path{true};
        for (const auto& lib_path: shim_paths) {
            // Probably not needed
            if (!is_first_ld_path) {
                ld_preload_str += ':';
            }
            ld_preload_str += (pwd / lib_path).string();
            is_first_ld_path = false;
        }

        std::vector<const char*> c_program_env;
        c_program_env.push_back(ld_preload_str.c_str());

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
