#include <sanitizer.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

#include "CUDAEventSource.hpp"

namespace cuda_trace {
    SanitizerEventSource::SanitizerEventSource() = default;
} // namespace cuda_trace

struct LaunchData {
    std::string functionName;
};

using LaunchVector = std::vector<LaunchData>;

void LaunchBegin(cuda_trace::SanitizerEventSource* pCallbackTracker, const Sanitizer_LaunchData* pLaunchData) {
    std::cout << "SANITIZER KERNEL LAUNCH" << std::endl;

    pCallbackTracker->invoke_custom<cuda_trace::cuda_kernel_launch_tag>(pLaunchData->device, pLaunchData->functionName);
}

void LaunchEnd(cuda_trace::SanitizerEventSource* pCallbackTracker, const Sanitizer_LaunchData* pLaunchData) {
    std::cout << "SANITIZER KERNEL LAUNCH END" << std::endl;

    // TODO: exit or launch end?
    pCallbackTracker->invoke_custom<cuda_trace::cuda_kernel_exit_tag>(pLaunchData->device, pLaunchData->functionName);
}

void LaunchTrackerCallback(void* userdata, Sanitizer_CallbackDomain domain, Sanitizer_CallbackId cbid,
                           const void* cbdata) {
    auto* callbackTracker = static_cast<cuda_trace::SanitizerEventSource*>(userdata);

    std::cout << "SANITIZER HIT SOMETHING" << std::endl;

    switch (domain) {
        case SANITIZER_CB_DOMAIN_RESOURCE:
            switch (cbid) {
                default:
                    break;
            }
            break;
        case SANITIZER_CB_DOMAIN_LAUNCH:
            switch (cbid) {
                case SANITIZER_CBID_LAUNCH_BEGIN: {
                    const auto* pLaunchData = static_cast<const Sanitizer_LaunchData*>(cbdata);
                    LaunchBegin(callbackTracker, pLaunchData);
                    break;
                }
                case SANITIZER_CBID_LAUNCH_END: {
                    const auto* pLaunchData = static_cast<const Sanitizer_LaunchData*>(cbdata);
                    LaunchEnd(callbackTracker, pLaunchData);
                    break;
                }
                default:
                    break;
            }
            break;
        case SANITIZER_CB_DOMAIN_SYNCHRONIZE:
            switch (cbid) {
                case SANITIZER_CBID_SYNCHRONIZE_STREAM_SYNCHRONIZED: {
                    // auto *pSyncData = (Sanitizer_SynchronizeData *) cbdata;
                    // StreamSynchronized(callbackTracker, pSyncData->context, pSyncData->hStream);
                    break;
                }
                case SANITIZER_CBID_SYNCHRONIZE_CONTEXT_SYNCHRONIZED: {
                    // auto *pSyncData = (Sanitizer_SynchronizeData *) cbdata;
                    // ContextSynchronized(callbackTracker, pSyncData->context);
                    break;
                }
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

int InitializeInjection() {
    auto& profiler = cuda_trace::SanitizerEventSource::SanitizerEventSource::instance();

    std::cout << "SANITIZER INITIALIZED" << std::endl;

    Sanitizer_SubscriberHandle handle;
    sanitizerSubscribe(&handle, LaunchTrackerCallback, &profiler);
    sanitizerEnableDomain(1, handle, SANITIZER_CB_DOMAIN_LAUNCH);

    return 0;
}

int __global_initializer__ = InitializeInjection();

extern "C" cuda_trace::SanitizerEventSource* sigma_provider_instance() {
    return &cuda_trace::SanitizerEventSource::instance();
}
