#pragma once

#include <string>
#include <vector>

namespace Launcher {
    int fork_launch(const std::string &program_bin, const std::vector<const char *> &program_args,
                    const std::vector<const char *> &program_env);

    int lauch_with_ld_preload(const std::string &program_bin, const std::vector<const char *> &c_program_args,
                              const std::vector<std::string> &shim_paths);
} // namespace Launcher
