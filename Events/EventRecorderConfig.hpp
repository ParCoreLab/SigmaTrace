#pragma once

#include <cstddef>
#include <sched.h> // For CPU ID
#include <unistd.h>

#include <concepts>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include "Helpers/Timestamp.hpp"

#include "EventStorage.hpp"
#include "EventTrait.hpp"
#include "Events.hpp"
#include "FileIO/SerializableTrait.hpp"

namespace Events {
    struct alignas(8) EventFileHeader {
        EventTypeId event_id;
        EventCategory event_category{EventCategory::Unknown};
        char hostname[HOST_NAME_MAX];
        std::uint64_t cpu_id;
        std::uint64_t process_id;
        std::uint64_t thread_id;

        // Always at the end
        uint64_t _check{MAGIC_VALUE};
    };
} // namespace Events

DEFINE_SERIALIZABLE_TRAIT(Events::EventFileHeader);

namespace Events {
    // ================================================================
    // Compile-time config. Can be shared across classes (thanks concepts)
    // ================================================================
    namespace TS = Helpers::TS;

    // Concept requires TimestampSource, other fields have defaults
    template<typename T>
    concept CompileTimeConfig = requires {
        // Must specify a TimestampSource type
        typename T::TimestampSourceType;
        requires TS::TimestampSource<typename T::TimestampSourceType>;
    };

    // Fast (RDTSC-based) configuration - can be overridden by user
    struct DefaultCompileTimeConfig {
        using TimestampSourceType = TS::RdtscTimestampSource;
        static constexpr std::size_t default_buffer_capacity = 128;
    };

    // RdtscTimestamp is x86-64 only
    struct PortableCompileTimeConfig {
        using TimestampSourceType = TS::ChronoTimestampSource;
        static constexpr std::size_t default_buffer_capacity = 128;
    };

    // Defaults for easier debugging of the event library itself
    struct DebugCompileTimeConfig {
        using TimestampSourceType = TS::FakeTimestampSource;
        static constexpr std::size_t default_buffer_capacity = 128;
    };

    // Verify concept compliance
    static_assert(CompileTimeConfig<DefaultCompileTimeConfig>);
    static_assert(CompileTimeConfig<PortableCompileTimeConfig>);
    static_assert(CompileTimeConfig<DebugCompileTimeConfig>);

    // ================================================================
    // Runtime configuration for output and file management
    // ================================================================
    struct RuntimeConfig {
        // Top level directory for event files
        std::filesystem::path OutputDirectory = "EventRecorder";
        std::filesystem::path EventsDirectory = OutputDirectory / "events";

        // Suffix for event files. Useful for globbing
        std::string EventFileSuffix = ".events";

        // Ensure output directories exist
        // TODO: all processes/threads in the same launch should have the same root dir. Maybe boost file lock?
        // TODO: probably not a good idea to do this here. What if we copied the config or want to use const?
        void ensure_output_directory(const int _max_retries = 3) {
            if (std::filesystem::exists(OutputDirectory)) {
                if (_max_retries <= 0) {
                    std::cerr << "Could not find a suitable output directory. Please clean up the dir" << std::endl;
                }

                std::cerr << "Warning: Output directory " << OutputDirectory << " already exists." << std::endl;
                // TODO: maybe handle this better
                const auto timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                OutputDirectory = OutputDirectory.string() + "_" + std::to_string(timestamp);
                EventsDirectory = OutputDirectory / "events";
                std::cerr << "Saving in: " << OutputDirectory << std::endl;
                return ensure_output_directory(_max_retries - 1);
            }

            for (const auto& dir: {OutputDirectory, EventsDirectory}) {
                std::filesystem::create_directories(dir);
            }
        }

        using Path = std::filesystem::path;

        // Helpers for storage duration
        static const std::string& get_hostname() {
            static std::string hostname = std::invoke([]() {
                char hostname_arr[1024];
                const int err = gethostname(hostname_arr, sizeof hostname_arr);
                if (err) {
                    throw std::runtime_error("gethostname() returned " + std::to_string(err));
                }

                return std::string(hostname_arr);
            });

            return hostname;
        }

        static std::uint64_t get_cpu_id() {
            const static std::uint64_t cpu_id = std::invoke([]() {
                const auto cpu_id = sched_getcpu();

                // Apparently can fail if the kernel doesn't support it
                if (cpu_id < 0) {
                    throw std::runtime_error("Can't get CPU ID: errno - " + std::to_string(errno));
                }

                return static_cast<std::uint64_t>(cpu_id);
            });

            return cpu_id;
        }

        // TODO: uint32_t should suffice and even that is too large
        static std::uint64_t get_process_id() noexcept {
            const static auto process_id{getpid()};
            return static_cast<uint64_t>(process_id);
        }

        static std::uint64_t get_thread_id() noexcept {
            const thread_local auto thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
            return thread_id;
        }

        // In case we somehow hit the same node_id, process_id, and thread_id
        // thread_local will reset when the first thread dies
        static auto get_thread_discriminant(const unsigned length = 4) noexcept {
            thread_local std::string value = std::invoke([length] {
                static constexpr char charset[] = "0123456789"
                                                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                  "abcdefghijklmnopqrstuvwxyz";
                constexpr std::size_t N = sizeof(charset) - 1;

                std::mt19937 rng{std::random_device{}()};

                std::string s;
                s.resize(length);

                for (char& c: s) {
                    c = charset[rng() % N];
                }

                return s;
            });

            return value;
        }

        // Get the full path for an event file
        // TODO: may be inefficient
        // TODO: why are we taking std::thread::id here?
        template<HasEventTrait EventT>
        [[nodiscard]] Path get_event_file_path() const noexcept {
            // TODO: this can be thread_local
            std::ostringstream oss;

            // Get a unique filename
            // hostname_pid_tid_rand_event_id.events
            // clang-format off
            oss << get_hostname() << "_"
                << get_process_id() << "_"
                << get_thread_id() << "_"
                << get_thread_discriminant() << "_";
            // clang-format on

            // Add the event ID for good measure
            oss << static_cast<EventId_T>(EventTrait<EventT>::id) << EventFileSuffix;

            const auto path = EventsDirectory / oss.str();
            return path;
        }

        // TODO: this is here for convenience but breaks some conventions
        // I'm unsure if the config here should be static or thread_local first of all
        // Secondly, does it even really make sense to get pid and tid here?
        // Third, why is it part of a
        template<HasEventTrait EventT>
        [[nodiscard]] EventFileHeader get_event_file_header() const noexcept {
            thread_local EventFileHeader header = std::invoke([]() {
                auto header = EventFileHeader{
                        .event_id = EventTrait<EventT>::id,
                        .event_category = EventTrait<EventT>::category,
                        .hostname = {},
                        .cpu_id = get_cpu_id(),
                        .process_id = get_process_id(),
                        .thread_id = get_thread_id(),
                };

                const auto& hostname = get_hostname();
                snprintf(header.hostname, sizeof header.hostname, "%s", hostname.c_str());

                return header;
            });

            return header;
        }
    };

} // namespace Events
