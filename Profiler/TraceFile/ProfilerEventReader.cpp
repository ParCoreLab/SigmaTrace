#include <fstream>
#include <iostream>
#include <map>
#include <unordered_map>

#include <perfetto/trace/perfetto_trace.pb.h>

#include "EventReader.hpp"
#include "EventRecorderConfig.hpp"

#include "../Events/CUDAEvents.hpp"
#include "../Events/Mlx5DvEvents.hpp"

// Convenience
using Events::EventFileHeader;
using Events::EventHeader;
using Events::EventTrait;

namespace ProfilerEvents = Profiler::Events;
using Profiler::Events::GpuId_T;

using EnabledEvents = Events::TypeList<ProfilerEvents::Mlx5MemoryRegistration, ProfilerEvents::DoorbellRing,
                                       ProfilerEvents::CUDAKernelLaunch, ProfilerEvents::CUDAKernelExit>;

// ============================================================================
// PerfettoTraceBuilder - Helper for constructing Perfetto traces
// ============================================================================
class PerfettoTraceBuilder {
public:
    static constexpr std::string_view FILE_SUFFIX{".pftrace"};

    // Track hierarchy: Root -> Node -> [CPU, GPU, NIC] -> Operation tracks
    enum class TrackType { Root = 0, Node, CPU, GPU, NIC, Operation };
    // TODO: dumb. Make tracks dynamic
    static constexpr std::array<std::string_view, 6> TrackTypeStr{"Root", "Node", "CPU", "GPU", "NIC", "Operation"};

    // Sequence flags from Perfetto
    static constexpr uint32_t SEQ_INCREMENTAL_STATE_CLEARED = 1;
    static constexpr uint32_t SEQ_NEEDS_INCREMENTAL_STATE = 2;

    // Generate deterministic UUID from components
    static uint64_t make_uuid(const TrackType type, const uint64_t node_id = 0, const uint64_t gpu_id = 0,
                              const uint32_t op_id = 0) {
        // Pack: [type:8][node:16][gpu:16][op:24]
        return (static_cast<uint64_t>(type) << 56) | (static_cast<uint64_t>(node_id) << 40) |
               (static_cast<uint64_t>(gpu_id) << 24) | static_cast<uint64_t>(op_id);
    }

    // Register the root track (call once at start)
    void register_root_track(const std::string &name) {
        const uint64_t uuid = make_uuid(TrackType::Root);
        if (m_registered_tracks.contains(uuid)) {
            return;
        }

        auto *packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        // First packet clears incremental state
        if (!m_emitted_first_packet) {
            packet->set_sequence_flags(SEQ_INCREMENTAL_STATE_CLEARED);
            m_emitted_first_packet = true;
        }
        auto *desc = packet->mutable_track_descriptor();
        desc->set_uuid(uuid);
        desc->set_name(name);
        m_registered_tracks[uuid] = true;
    }

    // One node is one network host
    uint64_t register_node_track(uint32_t node_id, const std::string &name = "") {
        const uint64_t uuid = make_uuid(TrackType::Node, node_id);
        if (m_registered_tracks.contains(uuid)) {
            return uuid;
        }

        auto *packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        auto *desc = packet->mutable_track_descriptor();
        desc->set_uuid(uuid);
        desc->set_parent_uuid(make_uuid(TrackType::Root));
        desc->set_name(name.empty() ? "Node " + std::to_string(node_id) : name);
        m_registered_tracks[uuid] = true;
        return uuid;
    }

    // Register a CPU/GPU/NIC track
    uint64_t register_device_track(const TrackType track_type, const GpuId_T node_id, const GpuId_T device_id,
                                   const std::string &name = "") {
        const uint64_t uuid = make_uuid(track_type, node_id, device_id);
        if (m_registered_tracks.contains(uuid)) {
            return uuid;
        }

        // Ensure parent node track exists
        register_node_track(node_id);

        auto *packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        auto *desc = packet->mutable_track_descriptor();
        desc->set_uuid(uuid);
        desc->set_parent_uuid(make_uuid(TrackType::Node, node_id));

        std::stringstream ss;

        //  GPU 0: GPU-1234
        ss << TrackTypeStr[static_cast<size_t>(track_type)] << device_id;
        if (!name.empty()) {
            ss << ": " << name;
        }

        desc->set_name(ss.str());

        m_registered_tracks[uuid] = true;
        return uuid;
    }

    // Register an operation track for async operations (parent: GPU)
    uint64_t register_operation_track(const GpuId_T node_id, const GpuId_T gpu_id, const GpuId_T op_id,
                                      const std::string &name) {
        const uint64_t uuid = make_uuid(TrackType::Operation, node_id, gpu_id, op_id);
        if (m_registered_tracks.contains(uuid)) {
            return uuid;
        }

        // Ensure parent GPU track exists
        register_device_track(TrackType::GPU, node_id, gpu_id);

        auto *packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        auto *desc = packet->mutable_track_descriptor();
        desc->set_uuid(uuid);
        desc->set_parent_uuid(make_uuid(TrackType::GPU, node_id, gpu_id));
        desc->set_name(name);
        m_registered_tracks[uuid] = true;
        return uuid;
    }

    // ========================================================================
    // Slice Events (for RDMA operations, kernel launches, etc.)
    // ========================================================================

    void begin_slice(uint64_t track_uuid, uint64_t timestamp_ns, const std::string &name,
                     const std::string &category = "rdma") {
        auto *packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        packet->set_timestamp(timestamp_ns);

        auto *event = packet->mutable_track_event();
        event->set_track_uuid(track_uuid);
        event->set_type(perfetto::protos::TrackEvent::TYPE_SLICE_BEGIN);
        event->set_name(name);
        event->add_categories(category);
    }

    void end_slice(uint64_t track_uuid, uint64_t timestamp_ns) {
        auto *packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        packet->set_timestamp(timestamp_ns);

        auto *event = packet->mutable_track_event();
        event->set_track_uuid(track_uuid);
        event->set_type(perfetto::protos::TrackEvent::TYPE_SLICE_END);
    }

    // Convenience: emit instant event (for doorbell rings, completions, etc.)
    void instant_event(uint64_t track_uuid, uint64_t timestamp_ns, const std::string &name,
                       const std::string &category = "rdma") {
        auto *packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        packet->set_timestamp(timestamp_ns);

        auto *event = packet->mutable_track_event();
        event->set_track_uuid(track_uuid);
        event->set_type(perfetto::protos::TrackEvent::TYPE_INSTANT);
        event->set_name(name);
        event->add_categories(category);
    }

    // ========================================================================
    // Flow Events (for visualizing data movement across GPUs/nodes)
    // ========================================================================

    // Generate a unique flow ID for tracking data movement
    uint64_t new_flow_id() { return m_next_flow_id++; }

    // Begin a flow at the source (e.g., nvshmem_put initiator)
    void begin_flow(uint64_t track_uuid, uint64_t timestamp_ns, uint64_t flow_id, const std::string &name,
                    const std::string &category = "data_flow") {
        auto *packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        packet->set_timestamp(timestamp_ns);

        auto *event = packet->mutable_track_event();
        event->set_track_uuid(track_uuid);
        event->set_type(perfetto::protos::TrackEvent::TYPE_INSTANT);
        event->set_name(name);
        event->add_categories(category);
        event->add_flow_ids(flow_id);
    }

    // Continue/pass through a flow (intermediate steps)
    void through_flow(uint64_t track_uuid, uint64_t timestamp_ns, uint64_t flow_id, const std::string &name,
                      const std::string &category = "data_flow") {
        auto *packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        packet->set_timestamp(timestamp_ns);

        auto *event = packet->mutable_track_event();
        event->set_track_uuid(track_uuid);
        event->set_type(perfetto::protos::TrackEvent::TYPE_INSTANT);
        event->set_name(name);
        event->add_categories(category);
        event->add_flow_ids(flow_id);
    }

    // End a flow at the destination (e.g., receive completion)
    void end_flow(uint64_t track_uuid, uint64_t timestamp_ns, uint64_t flow_id, const std::string &name,
                  const std::string &category = "data_flow") {
        auto *packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        packet->set_timestamp(timestamp_ns);

        auto *event = packet->mutable_track_event();
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
    void add_annotation(const std::string &key, uint64_t value) {
        if (m_trace.packet_size() == 0)
            return;
        auto *event = m_trace.mutable_packet(m_trace.packet_size() - 1)->mutable_track_event();
        auto *ann = event->add_debug_annotations();
        ann->set_name(key);
        ann->set_uint_value(value);
    }

    void add_annotation(const std::string &key, int64_t value) {
        if (m_trace.packet_size() == 0)
            return;
        auto *event = m_trace.mutable_packet(m_trace.packet_size() - 1)->mutable_track_event();
        auto *ann = event->add_debug_annotations();
        ann->set_name(key);
        ann->set_int_value(value);
    }

    void add_annotation(const std::string &key, const std::string &value) {
        if (m_trace.packet_size() == 0)
            return;
        auto *event = m_trace.mutable_packet(m_trace.packet_size() - 1)->mutable_track_event();
        auto *ann = event->add_debug_annotations();
        ann->set_name(key);
        ann->set_string_value(value);
    }

    void add_annotation(const std::string &key, double value) {
        if (m_trace.packet_size() == 0)
            return;
        auto *event = m_trace.mutable_packet(m_trace.packet_size() - 1)->mutable_track_event();
        auto *ann = event->add_debug_annotations();
        ann->set_name(key);
        ann->set_double_value(value);
    }

    // ========================================================================
    // Serialization
    // ========================================================================

    [[nodiscard]] const perfetto::protos::Trace &trace() const { return m_trace; }

    bool write_to_file(const std::filesystem::path &path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open())
            return false;
        return m_trace.SerializeToOstream(&out);
    }

    [[nodiscard]] size_t packet_count() const { return m_trace.packet_size(); }

private:
    perfetto::protos::Trace m_trace;
    std::unordered_map<uint64_t, bool> m_registered_tracks;
    uint64_t m_next_flow_id{1};
    uint32_t m_sequence_id{1}; // Constant sequence ID (single-threaded writer)
    bool m_emitted_first_packet{false};
};

// ============================================================================
// ProfilerEventReader - Converts binary events to Perfetto traces
// ============================================================================
class ProfilerEventReader : public Events::EventReaderBase<ProfilerEventReader, EnabledEvents> {
    using TrackType = PerfettoTraceBuilder::TrackType;

public:
    explicit ProfilerEventReader(const Events::RuntimeConfig &config, const std::filesystem::path &output_path) :
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

    const std::filesystem::path &get_output_path() { return m_output_path; }

    void visit(EventTrait<ProfilerEvents::Mlx5MemoryRegistration> T, const EventFileHeader &file_header,
               EventHeader &&header, ProfilerEvents::Mlx5MemoryRegistration &&event) {
        using Meta = decltype(T);

        const auto node_id = m_node_ids[file_header.hostname];
        // TODO: This is wrong. There's just no GPU sometimes
        const auto nic_id = event.nic_id;

        // Ensure GPU track exists
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::NIC, node_id, nic_id);

        m_builder.instant_event(track_uuid, header.timestamp, Meta::name);
        m_builder.add_annotation("addr", event.addr);
        m_builder.add_annotation("size", event.size);

        std::cout << "[" << header.timestamp << "] Memory Registration: addr=0x";
        std::cout << std::hex << event.addr << std::dec << std::endl;
    }

    void visit(EventTrait<ProfilerEvents::DoorbellRing> T, const EventFileHeader &file_header, EventHeader &&header,
               ProfilerEvents::DoorbellRing &&event) {
        using Meta = decltype(T);

        const auto node_id = m_node_ids[file_header.hostname];
        const auto gpu_id = event.gpu_id;

        // Ensure GPU track exists
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::GPU, node_id, gpu_id);

        // Emit instant event for doorbell ring
        m_builder.instant_event(track_uuid, header.timestamp, Meta::name);
        m_builder.add_annotation("addr", event.addr);

        std::cout << "[" << header.timestamp << "] DoorbellRing: addr=0x";
        std::cout << std::hex << event.addr << std::dec << std::endl;
    }

    void visit(EventTrait<ProfilerEvents::CUDAKernelLaunch>, const EventFileHeader &file_header, EventHeader &&header,
               ProfilerEvents::CUDAKernelLaunch &&event) {
        const auto node_id = m_node_ids[file_header.hostname];
        const auto gpu_id = event.gpu_id;

        // Ensure GPU track exists
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::GPU, node_id, gpu_id);

        m_builder.begin_slice(track_uuid, header.timestamp, event.kernel_name, "cuda");
        // m_builder.add_annotation("grid_dim", event.grid_dim);
        // m_builder.add_annotation("block_dim", event.block_dim);
    }

    void visit(EventTrait<ProfilerEvents::CUDAKernelExit>, const EventFileHeader &file_header, EventHeader &&header,
               ProfilerEvents::CUDAKernelExit &&event) {
        const auto node_id = m_node_ids[file_header.hostname];
        const auto gpu_id = event.gpu_id;

        // Ensure GPU track exists
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::GPU, node_id, gpu_id);

        m_builder.end_slice(track_uuid, header.timestamp);
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

    // Track pending operations for flow/slice matching
    // std::unordered_map<uint64_t, uint64_t> m_pending_flows;  // request_id -> flow_id
    // std::unordered_map<uint64_t, PendingKernel> m_pending_kernels;
};

int main(int argc, char *argv[]) {
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
