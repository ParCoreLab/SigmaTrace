#include <fstream>
#include <iostream>

#include <perfetto.h>

#include "PerfettoLogger.hpp"

PERFETTO_DEFINE_CATEGORIES(perfetto::Category("test_trace").SetDescription("Events from the graphics subsystem"));

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

namespace Profiler::Perfetto {
    PerfettoLogger::PerfettoLogger() {
        // https://perfetto.dev/docs/instrumentation/tracing-sdk
        // Also see https://github.com/pytorch/pytorch/tree/main/torch/csrc/profiler
        // and https://github.com/pytorch/kineto/tree/main/libkineto/src
        perfetto::TracingInitArgs args;

        // The backends determine where trace events are recorded. You may select one
        // or more of:

        // 1) The in-process backend only records within the app itself.
        args.backends |= perfetto::kInProcessBackend;

        // 2) The system backend writes events into a system Perfetto daemon,
        //    allowing merging app and system events (e.g., ftrace) on the same
        //    timeline. Requires the Perfetto `traced` daemon to be running (e.g.,
        //    on Android Pie and newer).
        args.backends |= perfetto::kSystemBackend;

        perfetto::Tracing::Initialize(args);

        perfetto::TrackEvent::Register();

        // ---- Create tracing session ----
        perfetto::TraceConfig cfg;
        cfg.add_buffers()->set_size_kb(1024);

        auto *ds = cfg.add_data_sources()->mutable_config();
        ds->set_name("test_trace");

        auto tracing_session = perfetto::Tracing::NewTrace();
        tracing_session->Setup(cfg);
        tracing_session->StartBlocking();

        // ---- Emit events ----
        {
            TRACE_EVENT("test_trace", "CPU_Work");
        }

        // ---- Stop + collect ----
        tracing_session->StopBlocking();

        std::vector trace_data(tracing_session->ReadTraceBlocking());
        std::ofstream out("trace.pftrace", std::ios::binary);
        out.write(trace_data.data(), trace_data.size());
    }

    void PerfettoLogger::UpdateTest() {
        std::cout << "[PerfettoLogger] Tracing..." << std::endl;
        TRACE_EVENT("test_trace", "LayerTreeHost::DoUpdateLayers");
    }
} // namespace Profiler::Perfetto
