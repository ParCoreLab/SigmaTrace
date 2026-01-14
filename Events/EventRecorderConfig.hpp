#pragma once

#include <unistd.h>

#include <concepts>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "EventStorage.hpp"
#include "EventTrait.hpp"
#include "Events.hpp"
#include "FileIO/SerializableTrait.hpp"

namespace Events {
    struct alignas(8) EventFileHeader {
        EventTypeId event_id;
        int32_t process_id;
        int32_t thread_id;
    };
} // namespace Events

DEFINE_SERIALIZABLE_TRAIT(Events::EventFileHeader);

namespace Events {
    // ================================================================
    // Compile-time config. Can be shared across classes (thanks concepts)
    // ================================================================

    // Concept requires TimestampSource, other fields have defaults
    template<typename T>
    concept CompileTimeConfig = requires {
        // Must specify a TimestampSource type
        typename T::TimestampSourceType;
        requires TimestampSource<typename T::TimestampSourceType>;
    };

    // Fast (RDTSC-based) configuration - can be overridden by user
    struct DefaultCompileTimeConfig {
        using TimestampSourceType = RdtscTimestampSource;
        static constexpr size_t default_buffer_capacity = 128;
    };

    // RdtscTimestamp is x86-64 only
    struct PortableCompileTimeConfig {
        using TimestampSourceType = ChronoTimestampSource;
        static constexpr size_t default_buffer_capacity = 128;
    };

    // Defaults for easier debugging of the event library itself
    struct DebugCompileTimeConfig {
        using TimestampSourceType = FakeTimestampSource;
        static constexpr size_t default_buffer_capacity = 128;
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
        std::filesystem::path MetadataDirectory = OutputDirectory / "metadata";
        std::filesystem::path EventsDirectory = OutputDirectory / "events";

        // TODO
        // Prefix for event files. Probably not really necessary
        // std::string EventFilePrefix = "event";

        // Suffix for event files. Useful for globbing
        std::string EventFileSuffix = ".event";

        // Ensure output directories exist
        // TODO: probably not a good idea to do this here. What if we copied the config or want to use const?
        void ensure_output_directory() {
            if (std::filesystem::exists(OutputDirectory)) {
                // TODO: maybe handle this better
                const auto timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                OutputDirectory = OutputDirectory.string() + "_" + std::to_string(timestamp);
                MetadataDirectory = OutputDirectory / "metadata";
                EventsDirectory = OutputDirectory / "events";
                std::cerr << "Warning: Output directory " << OutputDirectory << " already exists.";
                std::cerr << "Saving in: " << OutputDirectory << std::endl;
            }

            for (const auto &dir: {OutputDirectory, MetadataDirectory, EventsDirectory}) {
                std::filesystem::create_directories(dir);
            }
        }

        using Path = std::filesystem::path;

        // Get the full path for an event file
        // TODO: may be inefficient
        // TODO: why are we taking std::thread::id here?
        Path get_event_file_path(const EventTypeId event_id, const std::thread::id thread_id,
                                 const char *event_type_name) const noexcept {
            std::ostringstream oss;
            oss << /* EventFilePrefix << "_" << */ static_cast<EventId_T>(event_id) << "_thread" << thread_id << "_"
                << event_type_name << EventFileSuffix;
            const auto path = EventsDirectory / oss.str();
            return path;
        }

        // TODO: this is here for convenience but breaks some conventions
        // I'm unsure if the config here should be static or thread_local first of all
        // Secondly, does it even really make sense to get pid and tid here?
        // Third, why is it part of a
        [[nodiscard]] EventFileHeader get_event_file_header(const EventTypeId event_id) const noexcept {
            const auto process_id = getpid();
            const auto thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());

            const EventFileHeader header{
                    .event_id = event_id,
                    .process_id = process_id,
                    .thread_id = static_cast<decltype(EventFileHeader::thread_id)>(thread_id),
            };

            return header;
        }

        // TODO
        // EventTypeId get_event_id_from_path(Path path) const noexcept {
        //
        // }

        Path get_metadata_path(std::thread::id thread_id, const char *event_type_name) const noexcept {
            std::ostringstream oss;
            oss << "metadata_" << thread_id << "_" << event_type_name;
            return MetadataDirectory / oss.str();
        }
    };

} // namespace Events
