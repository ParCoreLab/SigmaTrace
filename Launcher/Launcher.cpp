#include <filesystem>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <algorithm>
#include <ranges>

#include "Launcher.hpp"

// TODO: not portable
extern char** environ;

namespace Launcher {
    int fork_launch(const std::string& program_bin, const std::vector<const char*>& program_args,
                    const std::vector<const char*>& program_env) {
        pid_t pid = fork();
        if (pid == -1) {
            std::cerr << "Failed to fork()" << std::endl;
            perror("[Launcher] fork");
            return 1;
        }

        // Child
        if (pid == 0) {
            execve(program_bin.data(), const_cast<char**>(program_args.data()), const_cast<char**>(program_env.data()));
            perror("[Launcher] execvpe");
            _exit(127);
        }

        // Parent
        int status = 0;
        waitpid(pid, &status, 0);

        return WEXITSTATUS(status);
    }

    int lauch_with_ld_preload(const std::string& program_bin, const std::vector<const char*>& c_program_args,
                              const std::vector<std::string>& shim_paths) {
        const auto pwd = std::filesystem::current_path();

        // This owns the strings
        std::vector<std::string> ld_library_paths;
        ld_library_paths.reserve(shim_paths.size());

        std::ranges::transform(shim_paths, std::back_inserter(ld_library_paths),
                               [&](const std::string& lib_path) -> std::string {
            return "LD_PRELOAD=" + (pwd / "lib" / lib_path).string();
        });

        std::vector<const char*> c_program_env;
        c_program_env.reserve(ld_library_paths.size());

        std::ranges::transform(ld_library_paths, std::back_inserter(c_program_env),
                               [](const std::string& s) -> const char* {
            return s.c_str();
        });

        // Add existing environment
        for (int i = 0; environ[i] != nullptr; ++i) {
            c_program_env.push_back(environ[i]);
        }

        auto ret = fork_launch(program_bin, c_program_args, c_program_env);
        return ret;
    }
} // namespace Launcher
