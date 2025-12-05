#include <filesystem>
#include <iostream>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

// TODO: not portable
extern char **environ;

#include "ibverbs_trace.hpp"

using namespace ibv_hook;

struct Profiler : HookManager<Profiler, post_send_tag, post_recv_tag, poll_cq_tag> {};

int main(int argc, char *argv[]) {
    std::cout << "=== Profiler ok ===" << std::endl;

    // TODO: boost::program_options in the future
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <program> [args]" << std::endl;
        return 1;
    }

    // TODO: boost exec in the future (for portability (maybe))
    const std::string program_bin = argv[1];
    std::vector<const char *> program_args(argv + 1, argv + argc);
    program_args.push_back(nullptr);

    const auto pwd = std::filesystem::current_path();
    const auto ld_preload_path = pwd / "lib" / "libprofiler_trace.so";
    const auto ld_preload_env = "LD_PRELOAD=" + ld_preload_path.string();

    std::vector<const char *> program_env{ld_preload_env.c_str()};

    for (int i = 0; environ[i] != nullptr; ++i) {
        program_env.push_back(environ[i]);
    }
    program_env.push_back(ld_preload_env.c_str());
    program_env.push_back(nullptr);

    pid_t pid = fork();
    if (pid == -1) {
        std::cerr << "Failed to fork()" << std::endl;
        perror("fork");
        return 1;
    }

    // Child
    if (pid == 0) {
        // Initialize the singleton
        Profiler::instance();

        // This will probably not run?
        Profiler::instance().register_pre<post_send_tag>([](auto &&...) {
            std::cout << "========= Pre post_send hook called ========= " << std::endl;
        });

        execvpe(program_bin.c_str(), const_cast<char **>(program_args.data()), const_cast<char **>(program_env.data()));
        perror("execvpe");
        _exit(127);
    }

    // Parent
    int status = 0;
    waitpid(pid, &status, 0);

    std::cout << "=== Profiler finished ===" << std::endl;

    return WEXITSTATUS(status);
}
