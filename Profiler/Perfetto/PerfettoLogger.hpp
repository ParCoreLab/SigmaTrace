#pragma once

namespace Profiler::Perfetto {
    // TODO: we probably don't even need a class
    class PerfettoLogger {
    public:
        static PerfettoLogger &instance() {
            static PerfettoLogger logger;
            return logger;
        }

        static void UpdateTest();

    private:
        PerfettoLogger();
    };
} // namespace Profiler::Perfetto
