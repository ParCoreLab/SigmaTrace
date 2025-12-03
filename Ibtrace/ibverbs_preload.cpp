#define _GNU_SOURCE
#include <dlfcn.h>
#include "ibverbs_trace.hpp"

using namespace ibv_hook;

using ActiveManager = DefaultHookManager;

static ibv_context *(*real_open)(ibv_device *) = nullptr;

static int (*real_close)(ibv_context *) = nullptr;

static void __attribute__((constructor)) init() {
    ActiveManager::instance();
    std::cout << "[ibv_hook] Loaded\n";
}

static void __attribute__((destructor)) finalize() {
    std::cout << "[ibv_hook] Unloaded\n";
    ActiveManager::instance().print_stats();
}

extern "C" ibv_context *ibv_open_device(ibv_device *device) {
    if (!real_open) {
        real_open = reinterpret_cast<decltype(real_open)>(dlvsym(RTLD_NEXT, "ibv_open_device", IBV_HOOK_VERSION));
    }

    auto *ctx = real_open(device);
    if (ctx) {
        ActiveManager::instance().install(ctx);
        std::cout << "[ibv_hook] Hooks installed on " << ctx << "\n";
    }

    return ctx;
}

extern "C" int ibv_close_device(ibv_context *context) {
    ActiveManager::instance().print_stats();

    if (!real_close) {
        real_close = reinterpret_cast<decltype(real_close)>(dlvsym(RTLD_NEXT, "ibv_close_device", IBV_HOOK_VERSION));
    }

    ActiveManager::instance().uninstall(context);
    return real_close(context);
}
