#include <cstdio>

#include <iostream>
#include <thread>

#include <cuda.h>
#include <cupti.h>

#include "CUDAEventSource.hpp"
#include "Helpers/CudaHelpers.hpp"

// Something like 8MB
static constexpr size_t BUF_SIZE = 8 * 1024 * 1024;
static constexpr size_t ALIGN_SIZE = 8;

#define ALIGN_BUFFER(buffer, align)                                                                                    \
    (((uintptr_t) (buffer) & ((align) - 1)) ? ((buffer) + (align) - ((uintptr_t) (buffer) & ((align) - 1))) : (buffer))

// https://github.com/eunomia-bpf/cupti-tutorial/blob/master/activity_trace_async/activity_trace_async.cpp
#define CUPTI_CHECK(call)                                                                                              \
    do {                                                                                                               \
        CUptiResult _status = call;                                                                                    \
        if (_status != CUPTI_SUCCESS) {                                                                                \
            const char* errstr;                                                                                        \
            cuptiGetResultString(_status, &errstr);                                                                    \
            fprintf(stderr, "%s:%d: error: function %s failed with error %s.\n", __FILE__, __LINE__, #call, errstr);   \
            exit(-1);                                                                                                  \
        }                                                                                                              \
    } while (0)


namespace cuda_trace {
    CUPTIEventSource::CUPTIEventSource() = default;
    CUPTIEventSource::~CUPTIEventSource() = default;
} // namespace cuda_trace

namespace cuda_trace {
    namespace detail {
        static CUpti_SubscriberHandle g_cupti_subscriber{};
        // Timestamp at trace initialization time.
        // Will be used  to normalize other timestamps
        static uint64_t g_cupti_start_timestamp;

#if CUPTI_API_VERSION >= 130000
        using CUpti_ActivityKernel = CUpti_ActivityKernel10;
#elif CUPTI_API_VERSION >= 19
        using CUpti_ActivityKernel = CUpti_ActivityKernel9;
#else
        using CUpti_ActivityKernel = CUpti_ActivityKernel8;
#endif
    } // namespace detail

    static void CUPTIAPI on_cupti_callback(void*, CUpti_CallbackDomain domain_id, CUpti_CallbackId,
                                           [[maybe_unused]] const void* cbdata) {
        switch (domain_id) {
            case CUPTI_CB_DOMAIN_SYNCHRONIZE: {
                // Cannot call cuptiActivityFlushAll here (or from another thread while this
                // callback is active) - CUPTI holds a global lock during callback dispatch.
                // Flushing is done from the atexit handler in ProfilerShim instead.

                // We don't join because this call is blocking and will block the cupti thread
                // The whole thing is weird. I tried many permutations of flushing and what
                // have you in other threads, destructor, etc., and this is unfortunately
                // the only way that works consistently
                // what's going on with cupti :(
                std::thread([]() {
                    CUPTI_CHECK(cuptiActivityFlushAll(0));
                }).detach();
                break;
            }
            case CUPTI_CB_DOMAIN_RESOURCE: {
                break;
            }

            default: {
                break;
            }
        }
    }

    static void printActivity(CUpti_Activity* record) {
        auto& profiler = CUPTIEventSource::instance();

        switch (record->kind) {
            case CUPTI_ACTIVITY_KIND_DEVICE: {
                const auto* device = reinterpret_cast<CUpti_ActivityDevice5*>(record);
                profiler.invoke_custom<cuda_device_available_tag>(device->id, device->uuid);

                /*
                const unsigned int* uuid = reinterpret_cast<unsigned int*>(const_cast<char*>(device->uuid.bytes));
                printf("[%u]: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", device->id,
                       uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9],
                       uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
                */

                break;
            }
            case CUPTI_ACTIVITY_KIND_CONTEXT: {
                [[maybe_unused]] const auto* context = reinterpret_cast<CUpti_ActivityContext3*>(record);
                break;
            }
            case CUPTI_ACTIVITY_KIND_KERNEL:
            case CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL: {
                const auto* kernel = reinterpret_cast<detail::CUpti_ActivityKernel*>(record);

                const auto device_id = kernel->deviceId;
                const auto* kernel_name = kernel->name;

                profiler.invoke_custom_ts<cuda_kernel_launch_tag>(kernel->start, device_id, kernel_name);
                profiler.invoke_custom_ts<cuda_kernel_exit_tag>(kernel->end, device_id, kernel_name);

                break;
            }
            default: {
                break;
            }
        }
    }

    static void CUPTIAPI bufferRequested(uint8_t** buffer, size_t* size, size_t* maxNumRecords) {
        *size = BUF_SIZE;
        uint8_t* rawBuffer = static_cast<uint8_t*>(malloc(*size + ALIGN_SIZE));

        *buffer = ALIGN_BUFFER(rawBuffer, ALIGN_SIZE);
        *maxNumRecords = 0;

        if (*buffer == nullptr) {
            std::cerr << "[CUPTI] Failed allocating buffer" << std::endl;
            exit(-1);
        }
    }

    static void CUPTIAPI bufferCompleted(CUcontext ctx, uint32_t streamId, uint8_t* buffer, size_t, size_t validSize) {
        CUptiResult status{};
        CUpti_Activity* record{nullptr};

        do {
            status = cuptiActivityGetNextRecord(buffer, validSize, &record);
            if (status == CUPTI_SUCCESS) {
                printActivity(record);
            } else if (status == CUPTI_ERROR_MAX_LIMIT_REACHED) {
                break;
            } else {
                CUPTI_CHECK(status);
            }
        } while (true);

        // report any records dropped from the queue
        size_t dropped;
        CUPTI_CHECK(cuptiActivityGetNumDroppedRecords(ctx, streamId, &dropped));
        if (dropped != 0) {
            printf("Dropped %u activity records\n", (unsigned int) dropped);
        }

        free(buffer);
    }

    void CUPTIEventSource::subscribe_cupti() {
        // We need to finalize if this module is reloaded, for example in a fork
        static std::atomic<bool> cupti_initialized{false};
        bool expected = false;
        if (!cupti_initialized.compare_exchange_strong(expected, true)) {
            CUPTI_CHECK(cuptiFinalize());
        }

        CUPTI_CHECK(cuptiActivityRegisterTimestampCallback(Helpers::TS::RdtscTimestampSource::Now));

        // Callback
        CUPTI_CHECK(cuptiSubscribe(&detail::g_cupti_subscriber, on_cupti_callback, nullptr));

        CUPTI_CHECK(cuptiEnableCallback(1, detail::g_cupti_subscriber, CUPTI_CB_DOMAIN_SYNCHRONIZE,
                                        CUPTI_CBID_SYNCHRONIZE_CONTEXT_SYNCHRONIZED));

        /*
        CUPTI_CHECK(cuptiEnableCallback(1, detail::g_cupti_subscriber, CUPTI_CB_DOMAIN_RESOURCE,
                                        CUPTI_CBID_RESOURCE_CONTEXT_CREATED));
        */

        // Activity
        CUPTI_CHECK(cuptiActivityRegisterCallbacks(bufferRequested, bufferCompleted));
        CUPTI_CHECK(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONTEXT));
        CUPTI_CHECK(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_DEVICE));
        CUPTI_CHECK(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_KERNEL));

        CUPTI_CHECK(cuptiGetTimestamp(&detail::g_cupti_start_timestamp));
    }
} // namespace cuda_trace

static void __attribute__((constructor)) init() {
    auto& profiler = cuda_trace::CUPTIEventSource::instance();
    profiler.subscribe_cupti();
}

static void __attribute__((destructor)) fini() {
    // Breaks things
    // CUPTI_CHECK(cuptiFinalize());
}

// Accessor for late-loaded libraries to get the singleton instance via dlsym
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
extern "C" cuda_trace::CUPTIEventSource* sigma_provider_instance() {
    return &cuda_trace::CUPTIEventSource::instance();
}
#pragma GCC diagnostic pop
