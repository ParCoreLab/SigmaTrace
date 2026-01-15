#include <iostream>

#include "Launcher.hpp"

int main(int argc, char *argv[]) {
    std::cout << "ok" << std::endl;

    // TODO: boost::program_options in the future
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <program> [args]" << std::endl;
        return 1;
    }

    const std::string program_bin = argv[1];
    std::vector<const char *> program_args(argv + 1, argv + argc);
    program_args.push_back(nullptr);

    // TODO: vector because it may be configurable in the future
    const std::vector<std::string> ld_shims{"libprofiler_shim.so"};

    const auto ret = Launcher::lauch_with_ld_preload(program_bin, program_args, ld_shims);
    return ret;
}
