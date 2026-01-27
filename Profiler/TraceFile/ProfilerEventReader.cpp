#include <fstream>
#include <iostream>
#include <map>
#include <unordered_map>

#include <perfetto/trace/perfetto_trace.pb.h>

#include "EventReader.hpp"
#include "EventRecorderConfig.hpp"

#include "../Events/CUDAEvents.hpp"
#include "../Events/IbvEvents.hpp"
#include "../Events/Mlx5DvEvents.hpp"
#include "Helpers/Utilities.hpp"

// Convenience
using Events::EventFileHeader;
using Events::EventHeader;
using Events::EventTrait;

namespace ProfilerEvents = Profiler::Events;
using Profiler::Events::GpuId_T;

using EnabledEvents = Events::TypeList<ProfilerEvents::IbvOpenDevice, ProfilerEvents::IbvCloseDevice,
                                       ProfilerEvents::Mlx5MemoryRegistration, ProfilerEvents::Mlx5MemoryDereg,
                                       ProfilerEvents::DoorbellRing, ProfilerEvents::CUDAKernelLaunch,
                                       ProfilerEvents::CUDAKernelExit>;

// ============================================================================
// PerfettoTraceBuilder - Helper for constructing Perfetto traces
// TODO: Fix types! Too much uint64_t abuse!!!
// ============================================================================
class PerfettoTraceBuilder {
public:
    static constexpr std::string_view FILE_SUFFIX{".pftrace"};

    // Track hierarchy: Root -> Node -> [CPU, GPU, NIC] -> Operation tracks
    enum class TrackType {
        Root = 0,
        CPU = static_cast<int>(Events::EventCategory::CPU),
        GPU = static_cast<int>(Events::EventCategory::GPU),
        NIC = static_cast<int>(Events::EventCategory::NIC),
        Node,
        Operation
    };
    // TODO: dumb. Make tracks dynamic
    const std::map<TrackType, std::string> TrackTypeStr{{TrackType::Root, "Root"}, {TrackType::CPU, "CPU"},
                                                        {TrackType::GPU, "GPU"},   {TrackType::NIC, "NIC"},
                                                        {TrackType::Node, "Node"}, {TrackType::Operation, "Operation"}};

    // Sequence flags from Perfetto
    static constexpr uint32_t SEQ_INCREMENTAL_STATE_CLEARED = 1;
    static constexpr uint32_t SEQ_NEEDS_INCREMENTAL_STATE = 2;

    // Generate deterministic UUID from components
    static uint64_t make_uuid(const TrackType type, const uint64_t node_id = 0, const uint64_t device_id = 0,
                              const uint64_t op_id = 0) {
        // Pack: [type:8][node:16][device:16][op:24]
        return (static_cast<uint64_t>(type) << 56) | (static_cast<uint64_t>(node_id) << 40) |
               (static_cast<uint64_t>(device_id) << 24) | static_cast<uint64_t>(op_id);
    }

    static uint64_t make_uuid(const Events::EventTypeId event_id, const uint64_t node_id, const uint64_t device_id,
                              const uint64_t op_id) {
        // Pack: [event:8][node:16][device:16][op:24]
        return (static_cast<uint64_t>(event_id) << 56) | (static_cast<uint64_t>(node_id) << 40) |
               (static_cast<uint64_t>(device_id) << 24) | (static_cast<uint64_t>(op_id));
    }


    // Register the root track (call once at start)
    void register_root_track(const std::string& name) {
        const uint64_t uuid = make_uuid(TrackType::Root);
        if (m_registered_tracks.contains(uuid)) {
            return;
        }

        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        // First packet clears incremental state
        if (!m_emitted_first_packet) {
            packet->set_sequence_flags(SEQ_INCREMENTAL_STATE_CLEARED);
            m_emitted_first_packet = true;
        }
        auto* desc = packet->mutable_track_descriptor();
        desc->set_uuid(uuid);
        desc->set_name(name);
        m_registered_tracks[uuid] = true;
    }

    // One node is one network host
    uint64_t register_node_track(const uint64_t node_id, const std::string& name = "") {
        const uint64_t uuid = make_uuid(TrackType::Node, node_id);
        if (m_registered_tracks.contains(uuid)) {
            return uuid;
        }

        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        auto* desc = packet->mutable_track_descriptor();
        desc->set_uuid(uuid);
        desc->set_parent_uuid(make_uuid(TrackType::Root));
        desc->set_name(name.empty() ? "Node " + std::to_string(node_id) : name);
        m_registered_tracks[uuid] = true;
        return uuid;
    }

    // Register a CPU/GPU/NIC track
    uint64_t register_device_track(const TrackType track_type, const uint64_t node_id, const uint64_t device_id,
                                   std::string_view name = "") {
        // TODO: fix the static_cast
        const uint64_t uuid = make_uuid(track_type, node_id, device_id);
        if (m_registered_tracks.contains(uuid)) {
            return uuid;
        }

        // Ensure parent node track exists
        register_node_track(node_id);

        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        auto* desc = packet->mutable_track_descriptor();
        desc->set_uuid(uuid);
        desc->set_parent_uuid(make_uuid(TrackType::Node, node_id));

        std::stringstream ss;

        //  GPU 0: GPU-1234
        const auto track_str = TrackTypeStr.find(track_type);
        if (track_str == TrackTypeStr.end()) {
            std::cerr << "WARNING: Unknown Track Type: " << static_cast<size_t>(track_type) << std::endl;
            ss << "Unknown " << device_id;
        } else {
            ss << track_str->second << device_id;
        }

        if (!name.empty()) {
            ss << ": " << name;
        }

        desc->set_name(ss.str());

        m_registered_tracks[uuid] = true;
        return uuid;
    }

    // Register an operation track for async operations (parent: GPU)
    uint64_t register_operation_track(const uint64_t node_id, const uint64_t gpu_id, const uint64_t op_id,
                                      const std::string& name) {
        const uint64_t uuid = make_uuid(TrackType::Operation, node_id, gpu_id, op_id);
        if (m_registered_tracks.contains(uuid)) {
            return uuid;
        }

        // Ensure parent GPU track exists
        register_device_track(TrackType::GPU, node_id, gpu_id);

        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        auto* desc = packet->mutable_track_descriptor();
        desc->set_uuid(uuid);
        desc->set_parent_uuid(make_uuid(TrackType::GPU, node_id, gpu_id));
        desc->set_name(name);
        m_registered_tracks[uuid] = true;
        return uuid;
    }

    // ========================================================================
    // Slice Events (for RDMA operations, kernel launches, etc.)
    // ========================================================================
    // TODO: std::string_view
    auto* begin_slice(uint64_t track_uuid, uint64_t timestamp_ns, const std::string& name, std::string_view category) {
        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        packet->set_timestamp(timestamp_ns);

        auto* event = packet->mutable_track_event();
        event->set_track_uuid(track_uuid);
        event->set_type(perfetto::protos::TrackEvent::TYPE_SLICE_BEGIN);
        event->set_name(name);
        event->add_categories(category.data());

        return event;
    }

    auto* end_slice(uint64_t track_uuid, uint64_t timestamp_ns) {
        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        packet->set_timestamp(timestamp_ns);

        auto* event = packet->mutable_track_event();
        event->set_track_uuid(track_uuid);
        event->set_type(perfetto::protos::TrackEvent::TYPE_SLICE_END);

        return event;
    }

    auto* add_slice(const uint64_t track_uuid, const uint64_t ts_start, uint64_t ts_end, const std::string& name,
                    std::string_view category) {
        // Safeguard
        if (ts_end == 0 || ts_start > ts_end) {
            std::cerr << "Received malformed timestamps for add_slice()" << std::endl;
            ts_end = ts_start;
        }

        begin_slice(track_uuid, ts_start, name, category);
        return end_slice(track_uuid, ts_end);
    }

    // Convenience: emit instant event (for doorbell rings, completions, etc.)
    auto* add_instant_event(uint64_t track_uuid, uint64_t timestamp_ns, const std::string& name,
                            const std::string& category = "rdma") {
        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        packet->set_timestamp(timestamp_ns);

        auto* event = packet->mutable_track_event();
        event->set_track_uuid(track_uuid);
        event->set_type(perfetto::protos::TrackEvent::TYPE_INSTANT);
        event->set_name(name);
        event->add_categories(category);

        return event;
    }

    // ========================================================================
    // Flow Events (for visualizing data movement across GPUs/nodes)
    // ========================================================================

    // Generate a unique flow ID for tracking data movement
    uint64_t new_flow_id() {
        return m_next_flow_id++;
    }

    // Begin a flow at the source (e.g., nvshmem_put initiator)
    void begin_flow(uint64_t track_uuid, uint64_t timestamp_ns, uint64_t flow_id, const std::string& name,
                    const std::string& category = "data_flow") {
        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        packet->set_timestamp(timestamp_ns);

        auto* event = packet->mutable_track_event();
        event->set_track_uuid(track_uuid);
        event->set_type(perfetto::protos::TrackEvent::TYPE_INSTANT);
        event->set_name(name);
        event->add_categories(category);
        event->add_flow_ids(flow_id);
    }

    // Continue/pass through a flow (intermediate steps)
    void through_flow(uint64_t track_uuid, uint64_t timestamp_ns, uint64_t flow_id, const std::string& name,
                      const std::string& category = "data_flow") {
        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        packet->set_timestamp(timestamp_ns);

        auto* event = packet->mutable_track_event();
        event->set_track_uuid(track_uuid);
        event->set_type(perfetto::protos::TrackEvent::TYPE_INSTANT);
        event->set_name(name);
        event->add_categories(category);
        event->add_flow_ids(flow_id);
    }

    // End a flow at the destination (e.g., receive completion)
    void end_flow(uint64_t track_uuid, uint64_t timestamp_ns, uint64_t flow_id, const std::string& name,
                  const std::string& category = "data_flow") {
        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        packet->set_timestamp(timestamp_ns);

        auto* event = packet->mutable_track_event();
        event->set_track_uuid(track_uuid);
        event->set_type(perfetto::protos::TrackEvent::TYPE_INSTANT);
        event->set_name(name);
        event->add_categories(category);
        event->add_terminating_flow_ids(flow_id);
    }

    // ========================================================================
    // Debug Annotations (for attaching metadata to events)
    // ========================================================================

    // Add debug annotation to the most recent packet's track event
    template<typename T>
    void add_annotation(const std::string& key, const T& value) {
        if (m_trace.packet_size() == 0) {
            return;
        }

        auto* event = m_trace.mutable_packet(m_trace.packet_size() - 1)->mutable_track_event();
        auto* ann = event->add_debug_annotations();
        ann->set_name(key);

        if constexpr (std::is_integral_v<T>) {
            if constexpr (std::is_signed_v<T>) {
                ann->set_int_value(value);
            } else {
                ann->set_uint_value(value);
            }
        } else if constexpr (std::is_floating_point_v<T>) {
            ann->set_double_value(value);
        } else if constexpr (std::is_same_v<T, std::string>) {
            ann->set_string_value(value);
        }
    }

    // ========================================================================
    // Serialization
    // ========================================================================

    [[nodiscard]] const perfetto::protos::Trace& trace() const {
        return m_trace;
    }

    bool write_to_file(const std::filesystem::path& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open())
            return false;
        return m_trace.SerializeToOstream(&out);
    }

    [[nodiscard]] size_t packet_count() const {
        return static_cast<size_t>(m_trace.packet_size());
    }

private:
    perfetto::protos::Trace m_trace;
    std::unordered_map<uint64_t, bool> m_registered_tracks;
    uint64_t m_next_flow_id{1};
    uint32_t m_sequence_id{1}; // Constant sequence ID (single-threaded writer)
    bool m_emitted_first_packet{false};
};

struct LocalStorage {
    std::unordered_map<uint64_t, GpuId_T> ibv_context_to_nic_id;

    // mem id -> flow id
    std::unordered_map<uint64_t, uint64_t> pending_mem_flows;

    std::unordered_map<uint64_t, uint64_t> pending_ibv_context_flows;
};

// ============================================================================
// ProfilerEventReader - Converts binary events to Perfetto traces
// ============================================================================
class ProfilerEventReader : public Events::EventReaderBase<ProfilerEventReader, EnabledEvents> {
    using TrackType = PerfettoTraceBuilder::TrackType;

public:
    explicit ProfilerEventReader(const Events::RuntimeConfig& config, const std::filesystem::path& output_path) :
        EventReaderBase(config), m_output_path(output_path) {
        m_output_path.replace_extension(PerfettoTraceBuilder::FILE_SUFFIX);

        if (std::filesystem::exists(m_output_path)) {
            std::cerr << "WARNING: Overwriting " << m_output_path << std::endl;
        }

        // Initialize trace structure
        m_builder.register_root_track("Profiler Trace");
    }

    ~ProfilerEventReader() {
        // Write final trace to file
        if (!m_builder.write_to_file(m_output_path)) {
            std::cerr << "Failed to write trace to: " << m_output_path << std::endl;
        }
    }

    const std::filesystem::path& get_output_path() {
        return m_output_path;
    }

    void add_header_annotations(const EventFileHeader& file_header) {
        m_builder.add_annotation("Hostname", file_header.hostname);
        m_builder.add_annotation("CPU ID", file_header.cpu_id);
        m_builder.add_annotation("Process ID", file_header.process_id);
        m_builder.add_annotation("Thread ID", file_header.process_id);
    }

    auto& get_local_storage(GpuId_T node_id, GpuId_T process_id, GpuId_T thread_id) {
        return m_local_storage[{node_id, process_id, thread_id}];
    }

    auto& get_local_storage(const EventFileHeader& file_header) {
        const auto node_id = m_node_ids[file_header.hostname];
        const auto process_id = file_header.process_id;
        const auto thread_id = file_header.thread_id;

        return m_local_storage[{node_id, process_id, thread_id}];
    }

    // Generically handled event
    template<typename EventT>
    void visit(EventTrait<EventT> T, const EventFileHeader& file_header, EventHeader&& header, EventT&& event) {
        using Meta = decltype(T);

        const auto node_id = m_node_ids[file_header.hostname];

        // Try to get the correct ID
        const auto [category, id] =
                std::invoke([&file_header, &event]() -> std::tuple<std::string_view, std::uint64_t> {
            sigma_ignore_unused(event);
            if constexpr (requires { event.gpu_id; }) {
                return {"gpu", static_cast<uint64_t>(event.gpu_id)};
            } else if constexpr (requires { event.nic_id; }) {
                return {"nic", static_cast<uint64_t>(event.nic_id)};
            }

            return {"cpu", file_header.cpu_id};
        });

        const uint64_t track_uuid =
                m_builder.register_device_track(static_cast<TrackType>(file_header.event_category),
                                                static_cast<uint64_t>(node_id), static_cast<uint64_t>(id), Meta::name);

        if (header.timestamp_end == 0) {
            m_builder.add_instant_event(track_uuid, header.timestamp_start, Meta::name);
        } else {
            m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, category);
        }
    }

    void visit(EventTrait<ProfilerEvents::IbvOpenDevice> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvOpenDevice&& event) {
        using Meta = decltype(T);

        const std::string nic_name{event.name};

        const auto node_id = m_node_ids[file_header.hostname];
        const auto nic_id = m_nic_ids[nic_name];

        const uint64_t track_uuid = m_builder.register_device_track(TrackType::NIC, static_cast<uint64_t>(node_id),
                                                                    static_cast<uint64_t>(nic_id), nic_name);

        const auto flow_id = m_builder.new_flow_id();

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "control");
        add_header_annotations(file_header);
        slice->add_flow_ids(flow_id);

        m_builder.add_annotation("name", nic_name);
        m_builder.add_annotation("name", event.dev_name);
        m_builder.add_annotation("dev_path", event.dev_path);
        m_builder.add_annotation("ibdev_path", event.ibdev_path);

        // Should be enums
        m_builder.add_annotation("node_type", event.node_type);
        m_builder.add_annotation("transport_type", event.transport_type);

        m_builder.add_annotation("Context addr", event.context_addr);

        auto& local_storage = get_local_storage(file_header);
        local_storage.ibv_context_to_nic_id[event.context_addr] = nic_id;
        local_storage.pending_ibv_context_flows[event.context_addr] = flow_id;
    }

    void visit(EventTrait<ProfilerEvents::IbvCloseDevice> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvCloseDevice&& event) {
        using Meta = decltype(T);

        const auto node_id = m_node_ids[file_header.hostname];

        auto& local_storage = get_local_storage(file_header);
        const auto nic_id = local_storage.ibv_context_to_nic_id[event.context_addr];

        const auto track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::NIC, static_cast<uint64_t>(node_id),
                                                                static_cast<uint64_t>(nic_id));

        const auto flow_id = local_storage.pending_ibv_context_flows[event.context_addr];

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "control");
        add_header_annotations(file_header);

        slice->add_terminating_flow_ids(flow_id);
    }

    void visit(EventTrait<ProfilerEvents::Mlx5MemoryRegistration> T, const EventFileHeader& file_header,
               EventHeader&& header, ProfilerEvents::Mlx5MemoryRegistration&& event) {
        using Meta = decltype(T);

        const auto node_id = m_node_ids[file_header.hostname];
        const auto nic_id = event.nic_id;

        const uint64_t track_uuid = m_builder.register_device_track(TrackType::NIC, static_cast<uint64_t>(node_id),
                                                                    static_cast<uint64_t>(nic_id));

        const auto flow_id = m_builder.new_flow_id();
        auto& local_storage = get_local_storage(file_header);
        local_storage.pending_mem_flows[event.mem_id] = flow_id;

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "memory");

        slice->add_flow_ids(flow_id);
        add_header_annotations(file_header);

        m_builder.add_annotation("Memory ID", event.mem_id);
        m_builder.add_annotation("Flow ID", flow_id);
        m_builder.add_annotation("Addr", event.addr);
        m_builder.add_annotation("Size", event.size);
    }

    void visit(EventTrait<ProfilerEvents::Mlx5MemoryDereg> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::Mlx5MemoryDereg&& event) {
        using Meta = decltype(T);

        const auto node_id = m_node_ids[file_header.hostname];
        const auto nic_id = event.nic_id;


        // Ensure GPU track exists
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::NIC, static_cast<uint64_t>(node_id),
                                                                    static_cast<uint64_t>(nic_id));

        auto& local_storage = get_local_storage(file_header);
        const auto flow_id = local_storage.pending_mem_flows[event.mem_id];
        local_storage.pending_mem_flows.erase(event.mem_id);

        auto* slice = m_builder.add_instant_event(track_uuid, header.timestamp_start, Meta::name, "memory");
        add_header_annotations(file_header);
        m_builder.add_annotation("Memory ID", event.mem_id);

        slice->add_terminating_flow_ids(flow_id);
    }

    void visit(EventTrait<ProfilerEvents::DoorbellRing> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::DoorbellRing&& event) {
        using Meta = decltype(T);

        const auto node_id = m_node_ids[file_header.hostname];
        const auto gpu_id = event.gpu_id;

        // Ensure GPU track exists
        // TODO: static_cast here isn't really correct... GpuId_T can be negative (because of out bad design)
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::GPU, static_cast<uint64_t>(node_id),
                                                                    static_cast<uint64_t>(gpu_id));

        // Emit instant event for doorbell ring
        m_builder.add_instant_event(track_uuid, header.timestamp_start, Meta::name);
        add_header_annotations(file_header);

        m_builder.add_annotation("Addr", event.addr);
        m_builder.add_annotation("wqe_index", event.wqe_index);
        m_builder.add_annotation("qp_num", event.qp_num);
        m_builder.add_annotation("Is_CPU_Hijack", event.is_cpu_hijack ? "Yes" : "No");
    }

    void visit(EventTrait<ProfilerEvents::CUDAKernelLaunch>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::CUDAKernelLaunch&& event) {
        const auto node_id = m_node_ids[file_header.hostname];
        const auto gpu_id = event.gpu_id;

        // Ensure GPU track exists
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::GPU, static_cast<uint64_t>(node_id),
                                                                    static_cast<uint64_t>(gpu_id));

        m_builder.begin_slice(track_uuid, header.timestamp_start, event.kernel_name, "cuda");
        // m_builder.add_annotation("grid_dim", event.grid_dim);
        // m_builder.add_annotation("block_dim", event.block_dim);
    }

    void visit(EventTrait<ProfilerEvents::CUDAKernelExit>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::CUDAKernelExit&& event) {
        const auto node_id = m_node_ids[file_header.hostname];
        const auto gpu_id = event.gpu_id;

        // Ensure GPU track exists
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::GPU, static_cast<uint64_t>(node_id),
                                                                    static_cast<uint64_t>(gpu_id));

        m_builder.end_slice(track_uuid, header.timestamp_start);
    }

    // ========================================================================
    // Example visit handlers for future events (uncomment and adapt as needed)
    // ========================================================================

    /*
    // RDMA Send operation - creates a slice with flow
    void visit(EventTag<RdmaSend>, const EventFileHeader &file_header, EventHeader &&header, RdmaSend &&event) {
        const uint64_t track = m_builder.register_gpu_track(event.node_id, event.gpu_id);

        // Start a flow for this send
        uint64_t flow_id = m_builder.new_flow_id();
        m_pending_flows[event.request_id] = flow_id;

        m_builder.begin_slice(track, header.timestamp, "RDMA Send");
        m_builder.add_annotation("dest_node", event.dest_node);
        m_builder.add_annotation("size", event.size);
        m_builder.begin_flow(track, header.timestamp, flow_id, "Data Transfer");
    }

    // RDMA Receive completion - ends the flow
    void visit(EventTag<RdmaRecv>, const EventFileHeader &file_header, EventHeader &&header, RdmaRecv &&event) {
        const uint64_t track = m_builder.register_gpu_track(event.node_id, event.gpu_id);

        // End the flow if we have the matching request
        if (auto it = m_pending_flows.find(event.request_id); it != m_pending_flows.end()) {
            m_builder.end_flow(track, header.timestamp, it->second, "Data Received");
            m_pending_flows.erase(it);
        }

        m_builder.instant_event(track, header.timestamp, "RDMA Recv Complete");
        m_builder.add_annotation("size", event.size);
    }

    // Kernel launch - hierarchical slice example
    void visit(EventTag<KernelLaunch>, const EventFileHeader &file_header, EventHeader &&header, KernelLaunch &&event) {
        const uint64_t track = m_builder.register_gpu_track(event.node_id, event.gpu_id);

        m_builder.begin_slice(track, header.timestamp, event.kernel_name, "cuda");
        m_builder.add_annotation("grid_dim", event.grid_dim);
        m_builder.add_annotation("block_dim", event.block_dim);

        // Track for hierarchical slice completion
        m_pending_kernels[event.correlation_id] = {track, event.kernel_name};
    }

    // Kernel completion
    void visit(EventTag<KernelComplete>, const EventFileHeader &file_header, EventHeader &&header, KernelComplete
    &&event) { if (auto it = m_pending_kernels.find(event.correlation_id); it != m_pending_kernels.end()) {
            m_builder.end_slice(it->second.track_uuid, header.timestamp);
            m_pending_kernels.erase(it);
        }
    }

    // NVSHMEM Put - demonstrates nested slices and flows
    void visit(EventTag<NvshmemPut>, const EventFileHeader &file_header, EventHeader &&header, NvshmemPut &&event) {
        const uint64_t track = m_builder.register_gpu_track(event.node_id, event.gpu_id);

        // Outer slice for the whole operation
        m_builder.begin_slice(track, header.timestamp, "nvshmem_put", "nvshmem");
        m_builder.add_annotation("dest_pe", event.dest_pe);
        m_builder.add_annotation("size", event.size);

        // Create flow to destination
        uint64_t flow_id = m_builder.new_flow_id();
        m_pending_flows[event.op_id] = flow_id;
        m_builder.begin_flow(track, header.timestamp, flow_id, "NVSHMEM Transfer");
    }
    */

private:
    std::filesystem::path m_output_path;
    PerfettoTraceBuilder m_builder;

    // TODO: This gives First-come, first-serve IDs
    std::map<std::string, GpuId_T> m_node_ids;
    std::map<std::string, GpuId_T> m_nic_ids;

    std::map<std::tuple<GpuId_T, GpuId_T, GpuId_T>, LocalStorage> m_local_storage;

    // Track pending operations for flow/slice matching
    // std::unordered_map<uint64_t, uint64_t> m_pending_flows;  // request_id -> flow_id
    // std::unordered_map<uint64_t, PendingKernel> m_pending_kernels;
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <events dir> <output file>" << std::endl;
        std::exit(1);
    }

    const auto input_path = std::filesystem::current_path() / std::filesystem::path{argv[1]} / "events";
    const auto output_path = std::filesystem::path{argv[2]};

    if (!std::filesystem::exists(input_path)) {
        std::cerr << input_path << " does not exist" << std::endl;
        std::exit(1);
    }

    Events::RuntimeConfig config{};
    config.EventsDirectory = input_path;

    ProfilerEventReader reader{config, output_path};

    std::cout << "Reading events from: " << config.EventsDirectory << std::endl;
    std::cout << "Writing output to: " << reader.get_output_path() << std::endl;

    const size_t count = reader.process();
    std::cout << "\nProcessed " << count << " events." << std::endl;

    return 0;
}
