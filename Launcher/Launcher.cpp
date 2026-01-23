#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

#include "Launcher.hpp"

namespace Launcher {
    int fork_launch(const std::string& program_bin, const std::vector<const char*>& program_args,
                    const std::vector<const char*>& program_env) {
        const pid_t pid = fork();
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
} // namespace Launcher
