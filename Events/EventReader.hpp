#pragma once

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <queue>
#include <type_traits>
#include <utility>
#include <vector>

#include "EventRecorderConfig.hpp"
#include "EventStorage.hpp"
#include "EventTrait.hpp"

namespace Events {

    // ============================================================================
    // Type List for enabled events
    // ============================================================================
    template<typename... Events>
    struct TypeList {
        static constexpr size_t size = sizeof...(Events);
    };

    // Derived class implements
    // visit(EventTrait<EventT>, const EventFileHeader &, EventHeader&& header, EventT&& event) for EventT
    template<typename Derived, typename EventT>
    concept Visitable = requires(Derived d) {
        // TODO: move the first arg out
        d.visit(std::declval<EventTrait<EventT>>(), std::declval<const EventFileHeader&>(),
                std::declval<EventHeader &&>(), std::declval<EventT &&>());
    };

    // ============================================================================
    template<typename Derived, typename EventList>
    class EventReaderBase;

    template<typename Derived, typename... EnabledEvents>
    class EventReaderBase<Derived, TypeList<EnabledEvents...>> {
    public:
        using EnabledEventList = TypeList<EnabledEvents...>;

        explicit EventReaderBase(RuntimeConfig config) : m_config(std::move(config)) {}

        // Process all events from files, merged by timestamp
        size_t process() {
            // Deferred check - Derived type is complete at this point
            static_assert((Visitable<Derived, EnabledEvents> && ...),
                          "Make sure to define a visitor for all EnabledEvents!");

            size_t num_processed{0};

            // Process meta events first
            if (m_meta_event_paths.empty()) {
                m_meta_event_paths = find_meta_event_files();
            }
            num_processed += process_merged(m_meta_event_paths);

            if (m_event_paths.empty()) {
                m_event_paths = find_event_files();
            }
            num_processed += process_merged(m_event_paths);

            return num_processed;
        }

        const auto& get_stats() const noexcept {
            return m_event_stats;
        }

    protected:
        Derived& derived() {
            return static_cast<Derived&>(*this);
        }

        // Default: no transformation, use raw timestamp for merge ordering
        // Derived classes override to convert TSC -> wall-clock ns
        Timestamp transform_merge_timestamp(Timestamp ts, const EventFileHeader&) {
            return ts;
        }

        std::map<std::string_view, size_t> m_event_stats{};

    public:
        // Single stream state (mmap actually, not a stream)
        struct StreamState {
            int fd{-1};
            const char* m_base{nullptr};
            const char* m_cursor{nullptr};
            size_t m_size{0};

            std::filesystem::path path;
            EventTypeId event_id{EventTypeId::Unknown};
            Timestamp next_timestamp{0};
            bool has_event{false};

            EventFileHeader file_header{};

            explicit StreamState(const std::filesystem::path& p) : path(p) {
                fd = open(p.c_str(), O_RDONLY);
                if (fd < 0) {
                    return;
                }

                struct stat st{};
                if (fstat(fd, &st) < 0 || st.st_size == 0) {
                    close(fd);
                    fd = -1;
                    return;
                }
                m_size = static_cast<size_t>(st.st_size);

                m_base = static_cast<const char*>(mmap(nullptr, m_size, PROT_READ, MAP_PRIVATE, fd, 0));
                if (m_base == MAP_FAILED) {
                    m_base = nullptr;
                    close(fd);
                    fd = -1;
                    return;
                }

                madvise(const_cast<char*>(m_base), m_size, MADV_SEQUENTIAL);

                m_cursor = m_base;

                // Read file header
                if (remaining() < sizeof(file_header)) {
                    cleanup();
                    return;
                }
                std::memcpy(&file_header, m_cursor, sizeof(file_header));
                m_cursor += sizeof(file_header);

                if (file_header._check != MAGIC_VALUE) {
                    std::cerr << "Magic value check fail in File header: " << path << std::endl;
                    std::cerr << "File is corrupted or recorded with an older version. Closing stream... " << std::endl;
                    cleanup();
                    return;
                }

                event_id = file_header.event_id;

                peek_next();
            }

            ~StreamState() {
                cleanup();
            }

            StreamState(StreamState&& o) noexcept :
                fd(o.fd), m_base(o.m_base), m_cursor(o.m_cursor), m_size(o.m_size), path(std::move(o.path)),
                event_id(o.event_id), next_timestamp(o.next_timestamp), has_event(o.has_event),
                file_header(o.file_header) {
                o.m_base = nullptr;
                o.fd = -1;
                o.has_event = false;
            }

            StreamState& operator=(StreamState&& o) noexcept {
                if (this != &o) {
                    cleanup();
                    fd = o.fd;
                    m_base = o.m_base;
                    m_cursor = o.m_cursor;
                    m_size = o.m_size;
                    path = std::move(o.path);
                    event_id = o.event_id;
                    next_timestamp = o.next_timestamp;
                    has_event = o.has_event;
                    file_header = o.file_header;
                    o.m_base = nullptr;
                    o.fd = -1;
                    o.has_event = false;
                }
                return *this;
            }

            StreamState(const StreamState&) = delete;
            StreamState& operator=(const StreamState&) = delete;

            [[nodiscard]] size_t remaining() const {
                return m_size - static_cast<size_t>(m_cursor - m_base);
            }

            void cleanup() {
                if (m_base) {
                    ::munmap(const_cast<char*>(m_base), m_size);
                    m_base = nullptr;
                }
                if (fd >= 0) {
                    ::close(fd);
                    fd = -1;
                }
                has_event = false;
            }

            void peek_next() {
                has_event = false;
                if (!m_base || remaining() < sizeof(EventHeader)) {
                    return;
                }

                // Read timestamp directly from mmap without copying
                const auto* header = reinterpret_cast<const EventHeader*>(m_cursor);
                next_timestamp = header->timestamp_start;
                has_event = true;
            }

            // Transform the raw timestamp for merge ordering (CRTP hook)
            // Derived class can override to convert TSC -> wall-clock ns
            void apply_merge_transform(Derived& d) {
                if (has_event) {
                    next_timestamp = d.transform_merge_timestamp(next_timestamp, file_header);
                }
            }

            static EventTypeId match_event_id(const EventId_T id) {
                auto result = EventTypeId::Unknown;
                std::ignore = ((id == static_cast<EventId_T>(EventTrait<EnabledEvents>::id)
                                        ? (result = EventTrait<EnabledEvents>::id, true)
                                        : false) ||
                               ...);
                return result;
            }
        };

    private:
        // Heap entry for merge
        struct HeapEntry {
            Timestamp timestamp;
            size_t stream_idx;

            bool operator<(const HeapEntry& o) const {
                return timestamp > o.timestamp;
            }
        };

        [[nodiscard]] std::vector<std::filesystem::path>
        internal_find_event_files(const std::filesystem::path dir) const {
            std::vector<std::filesystem::path> paths;

            if (!std::filesystem::exists(m_config.EventsDirectory)) {
                return paths;
            }

            for (const auto& entry: std::filesystem::directory_iterator(dir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }

                const auto filename = entry.path().filename().string();
                // if (filename.starts_with(m_config.EventFilePrefix) && filename.ends_with(m_config.EventFileSuffix)) {
                paths.push_back(entry.path());
                // }
            }

            // TODO
            // std::sort(paths.begin(), paths.end());
            return paths;
        }

    public:
        [[nodiscard]] std::vector<std::filesystem::path> find_event_files() const {
            return internal_find_event_files(m_config.EventsDirectory);
        }

        [[nodiscard]] std::vector<std::filesystem::path> find_meta_event_files() const {
            return internal_find_event_files(m_config.MetaEventsDirectory);
        }

    private:
        size_t process_merged(const std::vector<std::filesystem::path>& paths) {
            // Open all streams
            std::vector<StreamState> streams;
            streams.reserve(paths.size());

            for (const auto& path: paths) {
                streams.emplace_back(path);
            }

            // Initialize min-heap - Read one entry from each stream
            std::priority_queue<HeapEntry> heap;
            for (size_t i = 0; i < streams.size(); ++i) {
                streams[i].apply_merge_transform(derived());
                if (streams[i].has_event) {
                    heap.push({streams[i].next_timestamp, i});
                }
            }

            size_t count = 0;

            while (!heap.empty()) {
                auto [ts, idx] = heap.top();
                heap.pop();

                auto& state = streams[idx];

                // Dispatch and read event
                if (dispatch_read(state)) {
                    ++count;
                } else {
                    // No reader for this, we need to skip the stream...
                    std::cerr << "WARNING: No reader for ID " << static_cast<EventId_T>(state.event_id) << ". "
                              << "Add its tag to the type list" << std::endl;
                    state.cleanup();
                    continue;
                }

                // Peek next from this stream
                state.peek_next();
                state.apply_merge_transform(derived());
                if (state.has_event) {
                    heap.push({state.next_timestamp, idx});
                }
            }

            return count;
        }

        bool dispatch_read(StreamState& state) {
            // Try each enabled event type
            return (try_dispatch<EnabledEvents>(state) || ...);
        }

        template<typename EventT>
        bool try_dispatch(StreamState& state) {
            if (EventTrait<EventT>::id != state.event_id) {
                return false;
            }

            constexpr size_t total = sizeof(StoredEvent<EventT>);
            if (state.remaining() < total) {
                return false;
            }

            const auto* stored = reinterpret_cast<const StoredEvent<EventT>*>(state.m_cursor);
            state.m_cursor += total;

            // Copy the header in case we need to preprocess it
            EventHeader header = stored->header;

            // TODO: make Visitable const T& everything
            EventT data = stored->data;

            // Allow derived class to transform the header (e.g., TSC-to-ns conversion)
            // if constexpr (HasPreprocess<Derived, EventT>) {
            // Meta evnts don't really need preprocessing
            if constexpr (EventTrait<EventT>::category != EventCategory::Meta) {
                derived().preprocess(EventTrait<EventT>{}, state.file_header, header, data);
            }
            // }

            derived().visit(EventTrait<EventT>{}, state.file_header, std::move(header), std::move(data));

            ++m_event_stats[EventTrait<EventT>::name];

            return true;
        }

    protected:
        RuntimeConfig m_config{};
        std::vector<std::filesystem::path> m_event_paths;
        std::vector<std::filesystem::path> m_meta_event_paths;
    };

} // namespace Events
