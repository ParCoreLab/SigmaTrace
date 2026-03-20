#include <fstream>
#include <iostream>
#include <map>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/hash/hash.h>

#include <perfetto/trace/perfetto_trace.pb.h>

#include "EventReader.hpp"
#include "EventRecorderConfig.hpp"

#include "../Events/CUDAEvents.hpp"
#include "../Events/IbvEvents.hpp"
#include "../Events/LibfabricEvents.hpp"
#include "../Events/MetaEvents.hpp"
#include "../Events/Mlx5DvEvents.hpp"
#include "../Events/UcxEvents.hpp"
#include "Helpers/Utilities.hpp"

// Convenience
using Events::EventFileHeader;
using Events::EventHeader;
using Events::EventTrait;

namespace ProfilerEvents = Profiler::Events;
using Profiler::Events::GpuId_T;

using NodeId = std::uint64_t;
using ProcessId = std::uint64_t;
using ThreadId = std::uint64_t;
using NicId = std::uint64_t;

using EnabledEvents =
        Events::TypeList<ProfilerEvents::TscCalibration,        //
                         ProfilerEvents::IbvDeviceRegistration, // ProfilerEvents::IbvPortRegistration, //
                         ProfilerEvents::IbvOpenDevice, ProfilerEvents::IbvCloseDevice,                             //
                         ProfilerEvents::IbvCreateQp, ProfilerEvents::IbvDestroyQp, ProfilerEvents::IbvModifyQp,    //
                         ProfilerEvents::IbvMemoryReg, ProfilerEvents::IbvMemoryDereg,                              //
                         ProfilerEvents::IbvPostSend, ProfilerEvents::IbvPostSendWr, ProfilerEvents::IbvPostSendSg, //
                         ProfilerEvents::IbvPostRecv, ProfilerEvents::IbvPostRecvWr,                                //
                         ProfilerEvents::IbvPollCq, ProfilerEvents::IbvCompletion,                                  //
                         ProfilerEvents::QpConnectionInfo, ProfilerEvents::CommunicationSummary,                    //
                         ProfilerEvents::Mlx5dvMemoryRegistration, ProfilerEvents::Mlx5dvMemoryDereg,               //
                         ProfilerEvents::DoorbellRing, ProfilerEvents::CUDAKernelLaunch,
                         ProfilerEvents::CUDAKernelExit, //
                         // libfabric events
                         ProfilerEvents::FiDomainOpen, ProfilerEvents::FiEndpointCreate,             //
                         ProfilerEvents::FiEndpointClose, ProfilerEvents::FiMemoryReg,               //
                         ProfilerEvents::FiAvInsert, ProfilerEvents::FiSend, ProfilerEvents::FiRecv, //
                         ProfilerEvents::FiRdmaRead, ProfilerEvents::FiRdmaWrite,                    //
                         ProfilerEvents::FiCompletion, ProfilerEvents::FiInject,                     //
                         ProfilerEvents::FiEndpointQpMapping,                                        //
                         // UCX events
                         ProfilerEvents::UcpInit, ProfilerEvents::UcpWorkerCreate,                           //
                         ProfilerEvents::UcpWorkerDestroy, ProfilerEvents::UcpEpCreate,                      //
                         ProfilerEvents::UcpEpClose, ProfilerEvents::UcpTagSend, ProfilerEvents::UcpTagRecv, //
                         ProfilerEvents::UcpPut, ProfilerEvents::UcpGet, ProfilerEvents::UcpAmSend,          //
                         ProfilerEvents::UcpMemMap, ProfilerEvents::UcpMemUnmap,                             //
                         ProfilerEvents::UcpWorkerProgress, ProfilerEvents::UcpEpQpMapping                   //
                         >;

template<typename T, typename... Ts>
concept OneOf = (std::same_as<T, Ts> || ...);

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
        NIC_QP,
        Node,
        Operation,
        CounterQpBytesSent,
        CounterQpBytesRecv,
        CounterGpuBytesSent,
        CounterGpuBytesRecv,
        CounterQpBandwidthSent,
        CounterQpBandwidthRecv,
        CounterGpuBandwidthSent,
        CounterGpuBandwidthRecv,
        CounterNicBandwidthSent,
        CounterNicBandwidthRecv,
        CounterNodeBandwidthSent,
        CounterNodeBandwidthRecv,
        CounterNicBytesSent,
        CounterNicBytesRecv,
        CounterNodeBytesSent,
        CounterNodeBytesRecv,
        CounterNodeUcxBytesSent,
        CounterNodeUcxBytesRecv,
        CounterNodeUcxBandwidthSent,
        CounterNodeUcxBandwidthRecv,
        CounterNodeFabricBytesSent,
        CounterNodeFabricBytesRecv,
        CounterNodeFabricBandwidthSent,
        CounterNodeFabricBandwidthRecv,
    };
    // TODO: dumb. Make tracks dynamic
    const absl::flat_hash_map<TrackType, std::string> TrackTypeStr{{TrackType::Root, "Root"},
                                                                   {TrackType::CPU, "CPU"},
                                                                   {TrackType::GPU, "GPU"},
                                                                   {TrackType::NIC, "NIC"},
                                                                   {TrackType::NIC_QP, "QP"},
                                                                   {TrackType::Node, "Node"},
                                                                   {TrackType::Operation, "Operation"}};

    // Sequence flags from Perfetto
    static constexpr uint32_t SEQ_INCREMENTAL_STATE_CLEARED = 1;
    static constexpr uint32_t SEQ_NEEDS_INCREMENTAL_STATE = 2;

    template<typename... Args>
    static uint64_t make_uuid(Args&&... args) {
        // Warning: this can produce different hashes with the same values if their types don't match
        return absl::HashOf(std::forward<Args>(args)...);
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
        m_registered_tracks.emplace(uuid);
    }

    // One node is one network host
    uint64_t register_node_track(const uint64_t node_id, const std::string& name = "") {
        const uint64_t uuid = make_uuid(TrackType::Node, node_id);
        if (m_registered_tracks.contains(uuid)) {
            return uuid;
        }

        const auto root_uuid = make_uuid(TrackType::Root);

        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        auto* desc = packet->mutable_track_descriptor();
        desc->set_uuid(uuid);
        desc->set_parent_uuid(root_uuid);

        // Node 0: hostname
        auto node_name = "Node " + std::to_string(node_id) + ": " + name;
        desc->set_name(std::move(node_name));

        m_registered_tracks.emplace(uuid);
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
        const auto node_uuid = register_node_track(node_id);

        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        auto* desc = packet->mutable_track_descriptor();
        desc->set_uuid(uuid);
        desc->set_parent_uuid(node_uuid);

        std::stringstream ss;

        //  GPU/NIC/Unknown
        const auto track_str = TrackTypeStr.find(track_type);
        if (track_str == TrackTypeStr.end()) {
            std::cerr << "WARNING: Unknown Track Type: " << static_cast<size_t>(track_type) << std::endl;
            ss << "Unknown";
        } else {
            ss << track_str->second;
        }

        // Either NIC: mlx5_0 or NIC: 0
        ss << ": ";
        if (!name.empty()) {
            ss << name;
        } else {
            ss << device_id;
        }

        desc->set_name(ss.str());

        m_registered_tracks.emplace(uuid);
        return uuid;
    }

    // Register an operation or subdevice track
    uint64_t register_subdevice_track(const uint64_t parent_uuid, const uint64_t subdevice_uuid,
                                      const uint64_t subdevice_id, const std::string& name = "") {
        if (m_registered_tracks.contains(subdevice_uuid)) {
            return subdevice_uuid;
        }

        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        auto* desc = packet->mutable_track_descriptor();
        desc->set_uuid(subdevice_uuid);
        desc->set_parent_uuid(parent_uuid);
        desc->set_name(name + std::to_string(subdevice_id));
        m_registered_tracks.emplace(subdevice_uuid);

        return subdevice_uuid;
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

    uint64_t new_correlation_id() {
        return m_next_correlation_id++;
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
    // Counter Tracks (for bandwidth-over-time visualization)
    // ========================================================================
    uint64_t register_counter_track(uint64_t uuid, uint64_t parent_uuid, const std::string& name) {
        if (m_registered_tracks.contains(uuid)) {
            return uuid;
        }

        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        auto* desc = packet->mutable_track_descriptor();
        desc->set_uuid(uuid);
        desc->set_parent_uuid(parent_uuid);
        desc->set_name(name);
        auto* counter = desc->mutable_counter();
        counter->set_unit(perfetto::protos::CounterDescriptor::UNIT_SIZE_BYTES);
        counter->set_is_incremental(false);
        m_registered_tracks.emplace(uuid);
        return uuid;
    }

    uint64_t register_counter_track(uint64_t uuid, uint64_t parent_uuid, const std::string& name,
                                    const std::string& unit_name) {
        if (m_registered_tracks.contains(uuid)) {
            return uuid;
        }

        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        auto* desc = packet->mutable_track_descriptor();
        desc->set_uuid(uuid);
        desc->set_parent_uuid(parent_uuid);
        desc->set_name(name);
        auto* counter = desc->mutable_counter();
        counter->set_unit(perfetto::protos::CounterDescriptor::UNIT_UNSPECIFIED);
        counter->set_unit_name(unit_name);
        counter->set_is_incremental(false);
        m_registered_tracks.emplace(uuid);
        return uuid;
    }

    void emit_counter(uint64_t track_uuid, uint64_t timestamp_ns, int64_t value) {
        auto* packet = m_trace.add_packet();
        packet->set_trusted_packet_sequence_id(m_sequence_id);
        packet->set_timestamp(timestamp_ns);
        auto* event = packet->mutable_track_event();
        event->set_track_uuid(track_uuid);
        event->set_type(perfetto::protos::TrackEvent::TYPE_COUNTER);
        event->set_counter_value(value);
    }

    // ========================================================================
    // Debug Annotations (for attaching metadata to events)
    // ========================================================================
    template<typename T>
    void add_annotation(perfetto::protos::TrackEvent* event, const std::string& key, const T& value) {
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

    // Add debug annotation to the most recent packet's track event
    // TODO: change the semantics of this. Getting the last packet is weird
    template<typename T>
    void add_annotation(const std::string& key, const T& value) {
        if (m_trace.packet_size() == 0) {
            return;
        }

        auto* event = m_trace.mutable_packet(m_trace.packet_size() - 1)->mutable_track_event();

        return add_annotation(event, key, value);
    }

    // ========================================================================
    // Per-node sequence IDs to avoid cross-node timestamp ordering issues
    // ========================================================================
    void set_active_node(uint32_t node_id) {
        auto [it, inserted] = m_node_sequence_ids.try_emplace(node_id, m_next_sequence_id);
        if (inserted) {
            ++m_next_sequence_id;
        }
        m_sequence_id = it->second;
    }

    void reset_active_node() {
        m_sequence_id = 1; // Back to default (for track descriptors)
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
    absl::flat_hash_set<uint64_t> m_registered_tracks;
    uint64_t m_next_flow_id{1};
    uint64_t m_next_correlation_id{1};
    uint32_t m_sequence_id{1}; // Default sequence ID (for track descriptors etc.)
    absl::flat_hash_map<uint32_t /*node_id*/, uint32_t /*seq_id*/> m_node_sequence_ids;
    uint32_t m_next_sequence_id{2}; // 1 is reserved for default
    bool m_emitted_first_packet{false};
};

struct NicInfo {
    uint64_t device_guid;    // actual identity, goes in trace
    uint64_t sys_image_guid; // for multi-port HCAs
    char name[16];           // "mlx5_0" for human readability
    uint32_t internal_id;    // your synthetic 0,1,2... for compact storage
};

struct QpInfo {};

// Local to one device (GPU or NIC) in the system
struct DeviceStorage {
    absl::flat_hash_map<uint32_t, QpInfo> ibv_qp_info;
};

// QP connection info for cross-node correlation
struct QpConnectionEntry {
    uint32_t dest_qpn{0};
    uint64_t dest_interface_id{0};
    uint64_t dest_subnet_prefix{0};
    uint16_t dest_lid{0};
    uint8_t port_num{0};
};

// Generation-based flow entry for wr_id reuse disambiguation
struct FlowEntry {
    uint64_t flow_id;
    uint32_t generation;
};

// Local to one process
struct ProcessLocalStorage {
    // TODO:
    // absl::flat_hash_map<ThreadId_T, ThreadStorage> thread_storage

    absl::flat_hash_map<uint64_t, NicId> ibv_context_to_nic_id;

    // TODO: Not great, maybe struct IbvContextStorage?
    // context_addr, qp_num -> flow_id
    absl::flat_hash_map<std::pair<uint64_t, uint32_t>, uint64_t> ibv_qp_create_destroy_flows;

    absl::flat_hash_map<uint64_t, uint64_t> pending_ibv_context_flows;

    // (qp_num, wr_id) -> flow id
    absl::flat_hash_map<std::tuple<uint64_t, uint32_t>, uint64_t> pending_ibv_completions;

    // ============================================================================
    // QP connection map: src_qpn -> connection info
    absl::flat_hash_map<uint32_t, QpConnectionEntry> qp_connection_map;

    // QP -> GPU mapping (from mlx5dv QP creation events)
    absl::flat_hash_map<uint32_t /*qpn*/, GpuId_T> qp_to_gpu;
    // QP -> NIC mapping (from IbvCreateQp events)
    absl::flat_hash_map<uint32_t /*qpn*/, NicId> qp_to_nic;
    // ============================================================================

    // ============================================================================
    // mlx5dv
    // umem addr / page id -> context addr
    absl::flat_hash_map<uint64_t, uint64_t> mlx5dv_umem_addr_to_ctx;
    absl::flat_hash_map<uint64_t, uint64_t> mlx5dv_uar_addr_to_ctx;

    // TODO: Insane. Rewrite this
    // (context_addr, mem addr / page id) -> flow id
    absl::flat_hash_map<std::pair<uint64_t, uint64_t>, uint64_t> mlx5dv_pending_umem_flows;
    absl::flat_hash_map<std::pair<uint64_t, uint64_t>, uint64_t> mlx5dv_pending_uar_flows;
    // ============================================================================

    // ============================================================================
    // libfabric
    absl::flat_hash_map<uint64_t, uint64_t> fi_ep_create_destroy_flows; // ep_addr -> flow_id
    absl::flat_hash_map<uint64_t, uint64_t> fi_pending_context_flows;   // context -> flow_id
    absl::flat_hash_map<uint64_t, uint64_t> fi_ep_to_qp;                // ep_addr -> qp_num
    // ============================================================================

    // ============================================================================
    // UCX
    absl::flat_hash_map<uint64_t, uint64_t> ucp_ep_create_close_flows;       // ep_addr -> flow_id
    absl::flat_hash_map<uint64_t, uint64_t> ucp_worker_create_destroy_flows; // worker_addr -> flow_id
    absl::flat_hash_map<uint64_t, uint64_t> ucp_tag_send_flows;              // tag -> flow_id
    absl::flat_hash_map<uint64_t, uint64_t> ucp_ep_to_qp;                    // ep_addr -> qp_num
    // ============================================================================
};

// Local to one Node
struct NodeLocalStorage {
    absl::flat_hash_map<ProcessId, ProcessLocalStorage> process_storage;

    DeviceStorage device_storage;
};

// Shared among EVERYTHING
struct GlobalStorage {};

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
        // Flush windowed counters before writing
        flush_all_windowed_counters();

        // Write communication graph JSON
        write_comm_graph_json(m_output_path);

        // Write final trace to file
        if (!m_builder.write_to_file(m_output_path)) {
            std::cerr << "Failed to write trace to: " << m_output_path << std::endl;
        }
    }

    void set_gpu_track_mode(bool enabled) {
        m_gpu_track_mode = enabled;
    }

    void set_window_size_ms(uint64_t ms) {
        m_window_size_ns = ms * 1'000'000ULL;
    }

    const std::filesystem::path& get_output_path() {
        return m_output_path;
    }

    // Convert raw TSC to wall-clock ns for correct cross-node merge ordering
    Events::Timestamp transform_merge_timestamp(Events::Timestamp ts, const EventFileHeader& file_header) {
        const auto node_id = register_node(file_header.hostname);
        return tsc_to_ns(ts, node_id, file_header.process_id);
    }

    template<typename EventT>
    void preprocess(EventTrait<EventT>, EventFileHeader& file_header, EventHeader& header, const EventT&) {
        const auto node_id = register_node(file_header.hostname);
        header.timestamp_start = tsc_to_ns(header.timestamp_start, node_id, file_header.process_id);
        header.timestamp_end = tsc_to_ns(header.timestamp_end, node_id, file_header.process_id);

        // Set per-node Perfetto sequence ID to avoid cross-node ordering issues
        m_builder.set_active_node(node_id);
    }

    void add_header_annotations(perfetto::protos::TrackEvent* event, const EventFileHeader& file_header) {
        m_builder.add_annotation(event, "Hostname", file_header.hostname);
        m_builder.add_annotation(event, "CPU ID", file_header.cpu_id);
        m_builder.add_annotation(event, "Process ID", file_header.process_id);
        m_builder.add_annotation(event, "Thread ID", file_header.process_id);
    }

    /*
    auto& thread_local_storage(GpuId_T node_id, GpuId_T process_id, GpuId_T thread_id) {
        return m_thread_local_storage[{node_id, process_id, thread_id}];
    }

    auto& thread_local_storage(const EventFileHeader& file_header) {
        const auto node_id = register_node(file_header.hostname);
        const auto process_id = file_header.process_id;
        const auto thread_id = file_header.thread_id;

        return m_thread_local_storage[{node_id, process_id, thread_id}];
    }
    */

    // ========================================================================
    // Storage accessor helpers
    auto& node_local_storage(const NodeId node_id) {
        return m_node_local_storage.at(node_id);
    }

    ProcessLocalStorage& process_local_storage(const NodeId node_id, const ProcessId process_id) {
        auto& node_storage = node_local_storage(node_id);
        return node_storage.process_storage[process_id];
    }

    ProcessLocalStorage& process_local_storage(const EventFileHeader& file_header) {
        const auto node_id = register_node(file_header.hostname);
        return process_local_storage(node_id, file_header.process_id);
    }
    // ========================================================================

    // Generically handled event
    template<typename EventT>
    void visit(EventTrait<EventT> T, const EventFileHeader& file_header, EventHeader&& header, EventT&& event) {
        using Meta = decltype(T);

        const auto node_id = register_node(file_header.hostname);

        // Try to get the correct ID
        const auto [category, device_id] =
                std::invoke([this, &file_header, &event]() -> std::tuple<std::string_view, std::int32_t> {
            sigma_ignore_unused(event);
            if constexpr (requires { event.context_addr; }) {
                auto& local_storage = process_local_storage(file_header);
                const auto it = local_storage.ibv_context_to_nic_id.find(event.context_addr);
                if (it != local_storage.ibv_context_to_nic_id.end()) {
                    return {"nic", it->second};
                }
                return {"cpu", file_header.cpu_id};
            } else if constexpr (requires { event.gpu_id; }) {
                return {"gpu", event.gpu_id};
            }

            return {"cpu", file_header.cpu_id};
        });

        const uint64_t track_uuid = m_builder.register_device_track(static_cast<TrackType>(file_header.event_category),
                                                                    static_cast<uint64_t>(node_id),
                                                                    static_cast<uint64_t>(device_id), Meta::name);

        if (header.timestamp_end == 0) {
            m_builder.add_instant_event(track_uuid, header.timestamp_start, Meta::name);
        } else {
            m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, category);
        }
    }

    // ========================================================================
    // CPU-side ibv events

    // Generation-based flow ID: handles wr_id reuse by incrementing generation
    uint64_t get_ibv_qp_wr_flow_id(ProcessLocalStorage& local_storage, const auto& event) {
        // Look up dest_qpn for cross-node correlation
        uint32_t dest_qpn = event.qp_num; // default: same QP (local completion)
        const auto conn_it = local_storage.qp_connection_map.find(event.qp_num);
        if (conn_it != local_storage.qp_connection_map.end()) {
            dest_qpn = conn_it->second.dest_qpn;
        }

        auto key = std::make_pair(dest_qpn, event.wr_id);

        // Increment generation for this (dest_qp, wr_id) pair
        auto& gen = m_wr_generation[key];
        ++gen;

        // If there was already a pending flow, warn and overwrite
        auto pending_it = m_global_pending_flows.find(key);
        if (pending_it != m_global_pending_flows.end()) {
            // Previous flow was not completed before wr_id reuse
            // Best-effort: the old flow will remain unterminated
        }

        const auto flow_id = m_builder.new_flow_id();
        m_global_pending_flows[key] = FlowEntry{.flow_id = flow_id, .generation = gen};

        return flow_id;
    }

    template<typename EventT>
    // requires OneOf<EventT, ProfilerEvents::IbvOpenDevice, ProfilerEvents::IbvCloseDevice,
    //                ProfilerEvents::IbvMemoryReg, ProfilerEvents::IbvPostSend, ProfilerEvents::IbvPostSendWr,
    //                ProfilerEvents::IbvPostSendSg, ProfilerEvents::IbvPostRecv, ProfilerEvents::IbvPostRecvWr,
    //                ProfilerEvents::IbvPollCq, ProfilerEvents::IbvCompletion>
    auto* visit_ibv(const EventFileHeader& file_header, EventHeader&& header, EventT&& event, bool is_nic = false) {
        using Meta = EventTrait<EventT>;

        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        // const auto thread_id = file_header.thread_id;

        auto& local_storage = process_local_storage(file_header);

        // std::cout << Meta::name << " " << std::hex << event.context_addr << std::dec << std::endl;

        const auto nic_it = local_storage.ibv_context_to_nic_id.find(event.context_addr);
        if (nic_it == local_storage.ibv_context_to_nic_id.end()) {
            std::cerr << "WARNING: Unknown context_addr " << std::hex << event.context_addr << std::dec << " in "
                      << Meta::name << ", placing on CPU track" << std::endl;
        }
        // Fallback: use 0 as nic_id when context is unknown (events placed on CPU track)
        const auto nic_id = nic_it != local_storage.ibv_context_to_nic_id.end() ? nic_it->second : 0;

        const uint64_t cpu_track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);
        const uint64_t nic_track_uuid = nic_it != local_storage.ibv_context_to_nic_id.end()
                                                ? m_builder.register_device_track(TrackType::NIC, node_id, nic_id)
                                                : cpu_track_uuid;

        const auto ts_start{header.timestamp_start};
        const auto ts_end{header.timestamp_end};

        auto track_uuid = cpu_track_uuid;
        if (is_nic) {
            if constexpr (requires { event.qp_num; }) {
                // GPU-GPU track mode: route to GPU track if QP→GPU mapping exists
                if (m_gpu_track_mode) {
                    auto& lstorage = process_local_storage(file_header);
                    const auto gpu_it = lstorage.qp_to_gpu.find(event.qp_num);
                    if (gpu_it != lstorage.qp_to_gpu.end() && gpu_it->second >= 0) {
                        track_uuid = m_builder.register_device_track(TrackType::GPU, node_id,
                                                                     static_cast<uint64_t>(gpu_it->second));
                    } else {
                        const uint64_t qp_track_uuid =
                                m_builder.make_uuid(TrackType::NIC_QP, node_id, nic_id, event.qp_num);
                        m_builder.register_subdevice_track(nic_track_uuid, qp_track_uuid, event.qp_num, "QP");
                        track_uuid = qp_track_uuid;
                    }
                } else {
                    const uint64_t qp_track_uuid =
                            m_builder.make_uuid(TrackType::NIC_QP, node_id, nic_id, event.qp_num);
                    m_builder.register_subdevice_track(nic_track_uuid, qp_track_uuid, event.qp_num, "QP");
                    track_uuid = qp_track_uuid;
                }
            } else {
                track_uuid = nic_track_uuid;
            }
        }

        auto* slice = m_builder.add_slice(track_uuid, ts_start, ts_end, Meta::name, "ibv");
        add_header_annotations(slice, file_header);

        m_builder.add_annotation(slice, "Context Addr", event.context_addr);

        return slice;
    }

    // Non-trace, for bookkeeping only
    void visit(EventTrait<ProfilerEvents::IbvDeviceRegistration>, const EventFileHeader& file_header, EventHeader&&,
               ProfilerEvents::IbvDeviceRegistration&& event) {
        // This visitor sets up the node hostname
        const auto node_id = register_node(file_header.hostname);
        const auto nic_id = event.node_guid;

        // This visitor also sets up the NIC ID mapping
        const std::string nic_name{event.name};

        // Ensure NIC track with name exists
        // We do it here explicitly because only this visitor gets the name (not really)
        m_builder.register_device_track(TrackType::NIC, node_id, nic_id, nic_name);
    }

    // Non-trace, for bookkeeping only
    void visit(EventTrait<ProfilerEvents::IbvPortRegistration>, const EventFileHeader& file_header, EventHeader&&,
               ProfilerEvents::IbvPortRegistration&& event) {
        sigma_ignore_unused(file_header);
        sigma_ignore_unused(event);
    }

    void visit(EventTrait<ProfilerEvents::IbvOpenDevice>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvOpenDevice&& event) {
        auto& local_storage = process_local_storage(file_header);

        // TODO: dubious claims
        // Ensure context_addr is registered
        // We do it here because multiple subsequent ibv_open_device + ibv_close_device
        // calls can reopen a given context_addr with a different device. Great stuff.
        // IbvCloseDevice intentionally keeps the old entry alive for late events,
        // so a reopened context_addr with the same pointer value is expected.
        const auto [it, inserted] =
                local_storage.ibv_context_to_nic_id.try_emplace(event.context_addr, event.node_guid);

        if (!inserted) {
            if (it->second != event.node_guid) {
                std::cerr << "WARNING: context_addr " << std::hex << event.context_addr << std::dec
                          << " reopened with different NIC (old node_guid=" << it->second
                          << ", new node_guid=" << event.node_guid << "). Updating mapping." << std::endl;
            }
            it->second = event.node_guid;
        }

        const auto flow_id = m_builder.new_flow_id();
        local_storage.pending_ibv_context_flows[event.context_addr] = flow_id;

        auto* slice = visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event));
        slice->add_flow_ids(flow_id);

        m_builder.add_annotation(slice, "Context addr", event.context_addr);
        m_builder.add_annotation(slice, "Node GUID", event.node_guid);
    }

    void visit(EventTrait<ProfilerEvents::IbvCloseDevice>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvCloseDevice&& event) {
        auto* slice = visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event));

        auto& local_storage = process_local_storage(file_header);

        // TODO: may not be safe (logic wise)
        const auto flow_id = local_storage.pending_ibv_context_flows.erase(event.context_addr);
        slice->add_terminating_flow_ids(flow_id);

        // Keep ibv_context_to_nic_id alive. Late events may still reference this context.
        // The map is small (one entry per device) so it's fine to keep.
        // TODO: is this true???
    }

    void visit(EventTrait<ProfilerEvents::IbvCreateQp>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvCreateQp&& event) {
        /*
         * This whole thing doesn't really make sense. QPs are local to the NIC device
         * and the same node can have multiple NICs with different QPs that happen
         * to have the same qp_num. Furthermore, splicing using event.context_addr which
         * is a pointer as an uint64_t is probably also because (I think) multiple processes
         * can be expected to open the same NIC. Maybe even the same process will open multiple
         * times for whatever reason. It *might* be done because even though our maps will
         * have duplicate values from different processes (or even the same process)
         * they *should* point to the same hardware device.
         *
         * Maybe we should just save some device_guid. Where does one get such device_guid? I don't know
         */
        auto& local_storage = process_local_storage(file_header);

        const auto flow_id = m_builder.new_flow_id();
        local_storage.ibv_qp_create_destroy_flows[{event.context_addr, event.qp_num}] = flow_id;

        // Populate qp_to_nic for DoorbellRing NIC/Node bandwidth tracking
        const auto ctx_it = local_storage.ibv_context_to_nic_id.find(event.context_addr);
        if (ctx_it != local_storage.ibv_context_to_nic_id.end()) {
            local_storage.qp_to_nic[event.qp_num] = ctx_it->second;
        }

        auto* slice = visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event));
        slice->add_flow_ids(flow_id);

        m_builder.add_annotation(slice, "QP Num", event.qp_num);
    }

    void visit(EventTrait<ProfilerEvents::IbvDestroyQp>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvDestroyQp&& event) {
        auto& local_storage = process_local_storage(file_header);
        const auto flow_id = local_storage.ibv_qp_create_destroy_flows.erase({event.context_addr, event.qp_num});

        auto* slice = visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event));
        slice->add_terminating_flow_ids(flow_id);

        m_qp_dest_map.erase(event.qp_num);

        m_builder.add_annotation(slice, "QP Num", event.qp_num);
    }

    void visit(EventTrait<ProfilerEvents::IbvModifyQp>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvModifyQp&& event) {
        auto* slice = visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event));

        const auto dest_qp_num = event.dest_qp_num;

        // NO!!! NOT THIS
        m_qp_dest_map.emplace(event.qp_num, dest_qp_num);

        const auto dgid = event.ah_attr_grh_dgid;
        const auto interface_id = be64toh(dgid.interface_id);
        const auto subnet_prefix = be64toh(dgid.subnet_prefix);

        const auto dlid = event.ah_attr_dlid;

        m_builder.add_annotation(slice, "QP Num", event.qp_num);
        m_builder.add_annotation(slice, "Dest QP Num", dest_qp_num);
        m_builder.add_annotation(slice, "DLID", dlid);
        m_builder.add_annotation(slice, "Interface ID", interface_id);
        m_builder.add_annotation(slice, "Subnet Prefix", subnet_prefix);
    }

    void visit(EventTrait<ProfilerEvents::IbvMemoryReg>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvMemoryReg&& event) {
        auto* slice = visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event));

        m_builder.add_annotation(slice, "Addr", event.addr);
        m_builder.add_annotation(slice, "Size", event.size);
        m_builder.add_annotation(slice, "Type", event.type);
        // m_builder.add_annotation(slice, "GPU ID", event.gpu_id);
    }

    void visit(EventTrait<ProfilerEvents::IbvPostSend>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvPostSend&& event) {
        auto* slice = visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event));

        m_builder.add_annotation(slice, "QP Num", event.qp_num);
        m_builder.add_annotation(slice, "Num WRs", event.num_wrs);
    }

    void visit(EventTrait<ProfilerEvents::IbvPostSendWr>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvPostSendWr&& event) {
        auto* slice =
                visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event), true);

        const auto flow_id = get_ibv_qp_wr_flow_id(process_local_storage(file_header), event);
        slice->add_flow_ids(flow_id);

        m_builder.add_annotation("QP Num", event.qp_num);
        m_builder.add_annotation("WR ID", event.wr_id);
        m_builder.add_annotation("Num SGE", event.num_sge);
        m_builder.add_annotation("Opcode", event.opcode);
        m_builder.add_annotation("Send Flags", event.send_flags);
    }

    void visit(EventTrait<ProfilerEvents::IbvPostSendSg>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvPostSendSg&& event) {
        auto* slice =
                visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event), true);

        const auto flow_id = get_ibv_qp_wr_flow_id(process_local_storage(file_header), event);
        slice->add_flow_ids(flow_id);

        m_builder.add_annotation("QP Num", event.qp_num);
        m_builder.add_annotation("WR ID", event.wr_id);
        m_builder.add_annotation("Addr", event.addr);
        m_builder.add_annotation("Length", event.length);
        m_builder.add_annotation("Lkey", event.lkey);

        // Emit bandwidth counters for send
        if (event.length > 0) {
            const auto node_id = register_node(file_header.hostname);
            auto& local_storage = process_local_storage(file_header);
            const auto ctx_nic_it = local_storage.ibv_context_to_nic_id.find(event.context_addr);
            if (ctx_nic_it != local_storage.ibv_context_to_nic_id.end()) {
                const auto nic_id = ctx_nic_it->second;
                const uint64_t nic_track_uuid = m_builder.register_device_track(TrackType::NIC, node_id, nic_id);
                const uint64_t qp_track_uuid = m_builder.make_uuid(TrackType::NIC_QP, node_id, nic_id, event.qp_num);
                m_builder.register_subdevice_track(nic_track_uuid, qp_track_uuid, event.qp_num, "QP");

                emit_qp_send_counter(node_id, event.qp_num, qp_track_uuid, header.timestamp_start, event.length);

                const auto gpu_it = local_storage.qp_to_gpu.find(event.qp_num);
                if (gpu_it != local_storage.qp_to_gpu.end() && gpu_it->second >= 0) {
                    emit_gpu_send_counter(node_id, gpu_it->second, header.timestamp_start, event.length);
                }

                // Windowed bandwidth counters
                emit_qp_bw_sent(node_id, event.qp_num, qp_track_uuid, header.timestamp_start, event.length);
                emit_nic_bw_sent(node_id, nic_id, nic_track_uuid, header.timestamp_start, event.length);
                emit_nic_bytes_sent(node_id, nic_id, nic_track_uuid, header.timestamp_start, event.length);
                emit_node_bw_sent(node_id, header.timestamp_start, event.length);
                emit_node_bytes_sent(node_id, header.timestamp_start, event.length);
                if (gpu_it != local_storage.qp_to_gpu.end() && gpu_it->second >= 0) {
                    emit_gpu_bw_sent(node_id, gpu_it->second, header.timestamp_start, event.length);
                }
            }
        }
    }

    void visit(EventTrait<ProfilerEvents::IbvPostRecv>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvPostRecv&& event) {
        auto* slice = visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event));

        m_builder.add_annotation(slice, "QP Num", event.qp_num);
        m_builder.add_annotation(slice, "Num WRs", event.num_wrs);
    }

    void visit(EventTrait<ProfilerEvents::IbvPostRecvWr>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvPostRecvWr&& event) {
        auto* slice =
                visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event), true);

        const auto flow_id = get_ibv_qp_wr_flow_id(process_local_storage(file_header), event);
        slice->add_flow_ids(flow_id);

        // TODO: cross-node flow with lkey

        m_builder.add_annotation(slice, "QP Num", event.qp_num);
        m_builder.add_annotation(slice, "WR ID", event.wr_id);
        m_builder.add_annotation(slice, "Num SGE", event.num_sge);
    }

    void visit(EventTrait<ProfilerEvents::IbvPollCq>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvPollCq&& event) {
        auto* slice = visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event));

        m_builder.add_annotation(slice, "Num Completed", event.num_completed);
        m_builder.add_annotation("Num Requested", event.num_requested);
    }

    void visit(EventTrait<ProfilerEvents::IbvCompletion>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::IbvCompletion&& event) {
        auto* slice =
                visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event), true);

        // Try to terminate flows using generation-based matching
        // For completions, the qp_num IS the local QP (which is the dest from sender's perspective)
        auto key = std::make_pair(event.qp_num, event.wr_id);
        auto pending_it = m_global_pending_flows.find(key);
        if (pending_it != m_global_pending_flows.end()) {
            const auto flow_id = pending_it->second.flow_id;
            slice->add_terminating_flow_ids(flow_id);
            m_global_pending_flows.erase(pending_it);
        } else {
            // Fall back to local completion flow map
            auto& local_storage = process_local_storage(file_header);
            auto& completion_flow_map = local_storage.pending_ibv_completions;
            const auto& flow_entry = completion_flow_map.find({event.qp_num, event.wr_id});
            if (flow_entry != completion_flow_map.end()) {
                const auto flow_id = flow_entry->second;
                slice->add_terminating_flow_ids(flow_id);
                completion_flow_map.erase(flow_entry);
            }
        }

        m_builder.add_annotation("QP Num", event.qp_num);
        m_builder.add_annotation("WR ID", event.wr_id);
        m_builder.add_annotation("Opcode", static_cast<uint32_t>(event.opcode));
        m_builder.add_annotation("Status", static_cast<uint32_t>(event.status));
        m_builder.add_annotation("Byte Len", event.byte_len);

        // Emit bandwidth counters for recv completions
        if ((event.opcode & Profiler::Events::types::ibv_wc_opcode::IBV_WC_RECV) && event.byte_len > 0) {
            const auto node_id = register_node(file_header.hostname);
            auto& local_storage = process_local_storage(file_header);
            const auto ctx_nic_it = local_storage.ibv_context_to_nic_id.find(event.context_addr);
            if (ctx_nic_it != local_storage.ibv_context_to_nic_id.end()) {
                const auto nic_id = ctx_nic_it->second;
                const uint64_t nic_track_uuid = m_builder.register_device_track(TrackType::NIC, node_id, nic_id);
                const uint64_t qp_track_uuid = m_builder.make_uuid(TrackType::NIC_QP, node_id, nic_id, event.qp_num);
                m_builder.register_subdevice_track(nic_track_uuid, qp_track_uuid, event.qp_num, "QP");

                emit_qp_recv_counter(node_id, event.qp_num, qp_track_uuid, header.timestamp_start, event.byte_len);

                const auto gpu_it = local_storage.qp_to_gpu.find(event.qp_num);
                if (gpu_it != local_storage.qp_to_gpu.end() && gpu_it->second >= 0) {
                    emit_gpu_recv_counter(node_id, gpu_it->second, header.timestamp_start, event.byte_len);
                }

                // Windowed bandwidth counters
                emit_qp_bw_recv(node_id, event.qp_num, qp_track_uuid, header.timestamp_start, event.byte_len);
                emit_nic_bw_recv(node_id, nic_id, nic_track_uuid, header.timestamp_start, event.byte_len);
                emit_nic_bytes_recv(node_id, nic_id, nic_track_uuid, header.timestamp_start, event.byte_len);
                emit_node_bw_recv(node_id, header.timestamp_start, event.byte_len);
                emit_node_bytes_recv(node_id, header.timestamp_start, event.byte_len);
                if (gpu_it != local_storage.qp_to_gpu.end() && gpu_it->second >= 0) {
                    emit_gpu_bw_recv(node_id, gpu_it->second, header.timestamp_start, event.byte_len);
                }
            }
        }
    }

    // ========================================================================
    // QP Connection Info - builds the connection map for cross-node flows
    void visit(EventTrait<ProfilerEvents::QpConnectionInfo>, const EventFileHeader& file_header, EventHeader&&,
               ProfilerEvents::QpConnectionInfo&& event) {
        auto& local_storage = process_local_storage(file_header);

        // Populate connection map
        local_storage.qp_connection_map[event.src_qpn] = QpConnectionEntry{
                .dest_qpn = event.dest_qpn,
                .dest_interface_id = event.dest_interface_id,
                .dest_subnet_prefix = event.dest_subnet_prefix,
                .dest_lid = event.dest_lid,
                .port_num = event.port_num,
        };

        // Emit as instant event on the NIC track
        const auto node_id = register_node(file_header.hostname);
        uint64_t nic_id = 0;
        if (event.context_addr != 0) {
            const auto nic_it = local_storage.ibv_context_to_nic_id.find(event.context_addr);
            if (nic_it != local_storage.ibv_context_to_nic_id.end()) {
                nic_id = nic_it->second;
            }
        }

        const uint64_t qp_track_uuid = m_builder.make_uuid(TrackType::NIC_QP, node_id, nic_id, event.src_qpn);
        const uint64_t nic_track_uuid = m_builder.register_device_track(TrackType::NIC, node_id, nic_id);
        m_builder.register_subdevice_track(nic_track_uuid, qp_track_uuid, event.src_qpn, "QP");

        // auto* instant = m_builder.add_instant_event(qp_track_uuid, header.timestamp_start, "QP Connection");
        // m_builder.add_annotation(instant, "Src QPN", event.src_qpn);
        // m_builder.add_annotation(instant, "Dest QPN", event.dest_qpn);
        // m_builder.add_annotation(instant, "Dest Interface ID", event.dest_interface_id);
        // m_builder.add_annotation(instant, "Dest Subnet Prefix", event.dest_subnet_prefix);
        // m_builder.add_annotation(instant, "Dest LID", event.dest_lid);
    }

    // Communication Summary - aggregate metrics per QP, emitted at profiler shutdown
    // Only used for JSON graph output, NOT emitted into Perfetto trace
    void visit(EventTrait<ProfilerEvents::CommunicationSummary>, const EventFileHeader& file_header, EventHeader&&,
               ProfilerEvents::CommunicationSummary&& event) {
        sigma_ignore_unused(register_node(file_header.hostname));

        // Store for JSON graph output only
        m_comm_summaries.push_back({file_header.hostname, event});

        // Populate QP→GPU mapping for GPU-GPU track mode
        if (event.gpu_id >= 0) {
            auto& local_storage = process_local_storage(file_header);
            local_storage.qp_to_gpu[event.src_qpn] = event.gpu_id;
        }
    }
    // ========================================================================

    void visit(EventTrait<ProfilerEvents::Mlx5dvMemoryRegistration>, const EventFileHeader& file_header,
               EventHeader&& header, ProfilerEvents::Mlx5dvMemoryRegistration&& event) {
        auto* slice = visit_ibv(file_header, std::forward<EventHeader&&>(header), std::forward<decltype(event)>(event));

        // Save ctx for correlation on dereg
        auto& local_storage = process_local_storage(file_header);
        local_storage.mlx5dv_umem_addr_to_ctx.insert_or_assign(event.umem_addr, event.context_addr);

        // TODO: not checking if exists...
        const auto flow_id = m_builder.new_flow_id();
        local_storage.mlx5dv_pending_umem_flows[{event.context_addr, event.mem_id}] = flow_id;

        slice->add_flow_ids(flow_id);
        m_builder.add_annotation("Memory ID", event.mem_id);
        m_builder.add_annotation("Flow ID", flow_id);
        m_builder.add_annotation("Addr", event.addr);
        m_builder.add_annotation("Size", event.size);
    }

    void visit(EventTrait<ProfilerEvents::Mlx5dvMemoryDereg> T, const EventFileHeader& file_header,
               EventHeader&& header, ProfilerEvents::Mlx5dvMemoryDereg&& event) {
        using Meta = decltype(T);

        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;

        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto& local_storage = process_local_storage(file_header);

        // Sometimes we see duplicate mem deregs or out-of-order arrivals after merge reordering
        const auto ctx_it = local_storage.mlx5dv_umem_addr_to_ctx.find(event.umem_addr);
        if (ctx_it == local_storage.mlx5dv_umem_addr_to_ctx.end()) {
            std::cerr << "WARNING: mlx5dv_umem_dereg for unknown umem_addr " << std::hex << event.umem_addr << std::dec
                      << ", skipping" << std::endl;
            return;
        }
        const auto context_addr = ctx_it->second;

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "mlx5dv");
        add_header_annotations(slice, file_header);

        const auto flow_it = local_storage.mlx5dv_pending_umem_flows.find({context_addr, event.mem_id});
        if (flow_it != local_storage.mlx5dv_pending_umem_flows.end()) {
            slice->add_terminating_flow_ids(flow_it->second);
            local_storage.mlx5dv_pending_umem_flows.erase(flow_it);
        }
    }

    void visit(EventTrait<ProfilerEvents::DoorbellRing> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::DoorbellRing&& event) {
        using Meta = decltype(T);

        const auto node_id = register_node(file_header.hostname);
        const auto gpu_id = event.gpu_id;

        // Ensure GPU track exists
        // TODO: static_cast here isn't really correct... GpuId_T can be negative (because of out bad design)
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::GPU, static_cast<uint64_t>(node_id),
                                                                    static_cast<uint64_t>(gpu_id));

        // Emit instant event for doorbell ring
        auto* instant_event = m_builder.add_instant_event(track_uuid, header.timestamp_start, Meta::name);
        add_header_annotations(instant_event, file_header);

        m_builder.add_annotation("Addr", event.addr);
        // m_builder.add_annotation("wqe_index", event.wqe_index);
        m_builder.add_annotation("qp_num", event.qp_num);
        m_builder.add_annotation("Is_CPU_Hijack", event.is_cpu_hijack ? "Yes" : "No");
        m_builder.add_annotation("bytes_sent", event.bytes_sent);

        // Emit bandwidth counters for doorbell send
        if (event.bytes_sent > 0) {
            // For doorbell events, use the GPU track as parent for the QP counter
            emit_qp_send_counter(node_id, event.qp_num, track_uuid, header.timestamp_start, event.bytes_sent);
            emit_gpu_send_counter(node_id, gpu_id, header.timestamp_start, event.bytes_sent);

            // Windowed bandwidth counters
            emit_qp_bw_sent(node_id, event.qp_num, track_uuid, header.timestamp_start, event.bytes_sent);
            emit_gpu_bw_sent(node_id, gpu_id, header.timestamp_start, event.bytes_sent);
            // NIC/Node bandwidth if qp_to_nic lookup succeeds
            auto& db_local_storage = process_local_storage(file_header);
            const auto nic_it = db_local_storage.qp_to_nic.find(event.qp_num);
            if (nic_it != db_local_storage.qp_to_nic.end()) {
                const uint64_t nic_track_uuid =
                        m_builder.register_device_track(TrackType::NIC, node_id, nic_it->second);
                emit_nic_bw_sent(node_id, nic_it->second, nic_track_uuid, header.timestamp_start, event.bytes_sent);
                emit_nic_bytes_sent(node_id, nic_it->second, nic_track_uuid, header.timestamp_start, event.bytes_sent);
                emit_node_bw_sent(node_id, header.timestamp_start, event.bytes_sent);
                emit_node_bytes_sent(node_id, header.timestamp_start, event.bytes_sent);
            }
        }
    }

    // ========================================================================
    // libfabric events
    // ========================================================================
    void visit(EventTrait<ProfilerEvents::FiDomainOpen> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::FiDomainOpen&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "libfabric");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "Fabric Addr", event.fabric_addr);
        m_builder.add_annotation(slice, "Domain Addr", event.domain_addr);
        m_builder.add_annotation(slice, "Provider", std::string(event.provider_name));
    }

    void visit(EventTrait<ProfilerEvents::FiEndpointCreate> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::FiEndpointCreate&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto& local_storage = process_local_storage(file_header);
        const auto flow_id = m_builder.new_flow_id();
        local_storage.fi_ep_create_destroy_flows[event.ep_addr] = flow_id;

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "libfabric");
        add_header_annotations(slice, file_header);
        slice->add_flow_ids(flow_id);
        m_builder.add_annotation(slice, "EP Addr", event.ep_addr);
        m_builder.add_annotation(slice, "Domain Addr", event.domain_addr);
    }

    void visit(EventTrait<ProfilerEvents::FiEndpointClose> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::FiEndpointClose&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "libfabric");
        add_header_annotations(slice, file_header);

        auto& local_storage = process_local_storage(file_header);
        auto it = local_storage.fi_ep_create_destroy_flows.find(event.ep_addr);
        if (it != local_storage.fi_ep_create_destroy_flows.end()) {
            slice->add_terminating_flow_ids(it->second);
            local_storage.fi_ep_create_destroy_flows.erase(it);
        }
        m_builder.add_annotation(slice, "EP Addr", event.ep_addr);
    }

    void visit(EventTrait<ProfilerEvents::FiMemoryReg> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::FiMemoryReg&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "libfabric");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "Addr", event.addr);
        m_builder.add_annotation(slice, "Size", event.size);
        m_builder.add_annotation(slice, "Requested Key", event.requested_key);
    }

    void visit(EventTrait<ProfilerEvents::FiAvInsert> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::FiAvInsert&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "libfabric");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "AV Addr", event.av_addr);
        m_builder.add_annotation(slice, "Count", event.count);
    }

    void visit(EventTrait<ProfilerEvents::FiSend> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::FiSend&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto& local_storage = process_local_storage(file_header);

        // Create send->completion flow using context pointer as key
        if (event.context != 0) {
            const auto flow_id = m_builder.new_flow_id();
            local_storage.fi_pending_context_flows[event.context] = flow_id;
        }

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "libfabric");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "EP Addr", event.ep_addr);
        m_builder.add_annotation(slice, "Total IOV Len", event.total_iov_len);
        m_builder.add_annotation(slice, "Dest Addr", event.dest_addr);
        m_builder.add_annotation(slice, "Flags", event.flags);

        if (event.context != 0) {
            auto it = local_storage.fi_pending_context_flows.find(event.context);
            if (it != local_storage.fi_pending_context_flows.end()) {
                slice->add_flow_ids(it->second);
            }
        }

        // Bandwidth counters for fi_send
        if (event.total_iov_len > 0) {
            emit_fabric_node_bytes_sent(node_id, header.timestamp_start, event.total_iov_len);
            emit_fabric_node_bw_sent(node_id, header.timestamp_start, event.total_iov_len);
            auto info = resolve_ep_nic(local_storage, event.ep_addr, node_id, false);
            if (info.found) {
                const uint64_t qp_track = m_builder.make_uuid(TrackType::NIC_QP, node_id, info.nic_id, info.qp_num);
                m_builder.register_subdevice_track(info.nic_track_uuid, qp_track, info.qp_num, "QP");
                emit_send_counters(node_id, header.timestamp_start, event.total_iov_len, local_storage, info.qp_num,
                                   false, info.nic_id, info.nic_track_uuid, qp_track);
            }
        }
    }

    void visit(EventTrait<ProfilerEvents::FiRecv> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::FiRecv&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "libfabric");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "EP Addr", event.ep_addr);
        m_builder.add_annotation(slice, "Total IOV Len", event.total_iov_len);
        m_builder.add_annotation(slice, "Flags", event.flags);

        // Bandwidth counters for fi_recv
        if (event.total_iov_len > 0) {
            emit_fabric_node_bytes_recv(node_id, header.timestamp_start, event.total_iov_len);
            emit_fabric_node_bw_recv(node_id, header.timestamp_start, event.total_iov_len);
            auto& local_storage = process_local_storage(file_header);
            auto info = resolve_ep_nic(local_storage, event.ep_addr, node_id, false);
            if (info.found) {
                const uint64_t qp_track = m_builder.make_uuid(TrackType::NIC_QP, node_id, info.nic_id, info.qp_num);
                m_builder.register_subdevice_track(info.nic_track_uuid, qp_track, info.qp_num, "QP");
                emit_recv_counters(node_id, header.timestamp_start, event.total_iov_len, local_storage, info.qp_num,
                                   false, info.nic_id, info.nic_track_uuid, qp_track);
            }
        }
    }

    void visit(EventTrait<ProfilerEvents::FiRdmaRead> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::FiRdmaRead&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "libfabric");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "EP Addr", event.ep_addr);
        m_builder.add_annotation(slice, "Total IOV Len", event.total_iov_len);
        m_builder.add_annotation(slice, "Remote Addr", event.remote_addr);
        m_builder.add_annotation(slice, "Remote Key", event.remote_key);

        // Bandwidth counters for fi_rdma_read (recv direction)
        if (event.total_iov_len > 0) {
            emit_fabric_node_bytes_recv(node_id, header.timestamp_start, event.total_iov_len);
            emit_fabric_node_bw_recv(node_id, header.timestamp_start, event.total_iov_len);
            auto& local_storage = process_local_storage(file_header);
            auto info = resolve_ep_nic(local_storage, event.ep_addr, node_id, false);
            if (info.found) {
                const uint64_t qp_track = m_builder.make_uuid(TrackType::NIC_QP, node_id, info.nic_id, info.qp_num);
                m_builder.register_subdevice_track(info.nic_track_uuid, qp_track, info.qp_num, "QP");
                emit_recv_counters(node_id, header.timestamp_start, event.total_iov_len, local_storage, info.qp_num,
                                   false, info.nic_id, info.nic_track_uuid, qp_track);
            }
        }
    }

    void visit(EventTrait<ProfilerEvents::FiRdmaWrite> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::FiRdmaWrite&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "libfabric");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "EP Addr", event.ep_addr);
        m_builder.add_annotation(slice, "Total IOV Len", event.total_iov_len);
        m_builder.add_annotation(slice, "Remote Addr", event.remote_addr);
        m_builder.add_annotation(slice, "Remote Key", event.remote_key);

        // Bandwidth counters for fi_rdma_write (send direction)
        if (event.total_iov_len > 0) {
            emit_fabric_node_bytes_sent(node_id, header.timestamp_start, event.total_iov_len);
            emit_fabric_node_bw_sent(node_id, header.timestamp_start, event.total_iov_len);
            auto& local_storage = process_local_storage(file_header);
            auto info = resolve_ep_nic(local_storage, event.ep_addr, node_id, false);
            if (info.found) {
                const uint64_t qp_track = m_builder.make_uuid(TrackType::NIC_QP, node_id, info.nic_id, info.qp_num);
                m_builder.register_subdevice_track(info.nic_track_uuid, qp_track, info.qp_num, "QP");
                emit_send_counters(node_id, header.timestamp_start, event.total_iov_len, local_storage, info.qp_num,
                                   false, info.nic_id, info.nic_track_uuid, qp_track);
            }
        }
    }

    void visit(EventTrait<ProfilerEvents::FiCompletion> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::FiCompletion&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "libfabric");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "CQ Addr", event.cq_addr);
        m_builder.add_annotation(slice, "Num Completions", event.num_completions);

        // Terminate send->completion flow if context matches
        if (event.context != 0) {
            auto& local_storage = process_local_storage(file_header);
            auto it = local_storage.fi_pending_context_flows.find(event.context);
            if (it != local_storage.fi_pending_context_flows.end()) {
                slice->add_terminating_flow_ids(it->second);
                local_storage.fi_pending_context_flows.erase(it);
            }
        }
    }

    void visit(EventTrait<ProfilerEvents::FiInject> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::FiInject&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "libfabric");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "EP Addr", event.ep_addr);
        m_builder.add_annotation(slice, "Len", event.len);
        m_builder.add_annotation(slice, "Dest Addr", event.dest_addr);

        // Bandwidth counters for fi_inject (send direction)
        if (event.len > 0) {
            emit_fabric_node_bytes_sent(node_id, header.timestamp_start, event.len);
            emit_fabric_node_bw_sent(node_id, header.timestamp_start, event.len);
            auto& local_storage = process_local_storage(file_header);
            auto info = resolve_ep_nic(local_storage, event.ep_addr, node_id, false);
            if (info.found) {
                const uint64_t qp_track = m_builder.make_uuid(TrackType::NIC_QP, node_id, info.nic_id, info.qp_num);
                m_builder.register_subdevice_track(info.nic_track_uuid, qp_track, info.qp_num, "QP");
                emit_send_counters(node_id, header.timestamp_start, event.len, local_storage, info.qp_num, false,
                                   info.nic_id, info.nic_track_uuid, qp_track);
            }
        }
    }

    void visit(EventTrait<ProfilerEvents::FiEndpointQpMapping>, const EventFileHeader& file_header, EventHeader&&,
               ProfilerEvents::FiEndpointQpMapping&& event) {
        auto& local_storage = process_local_storage(file_header);
        local_storage.fi_ep_to_qp[event.ep_addr] = event.qp_num;
    }

    // ========================================================================
    // UCX events
    // ========================================================================
    void visit(EventTrait<ProfilerEvents::UcpInit> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::UcpInit&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice = m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "ucx");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "Context Addr", event.context_addr);
    }

    void visit(EventTrait<ProfilerEvents::UcpWorkerCreate> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::UcpWorkerCreate&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto& local_storage = process_local_storage(file_header);
        const auto flow_id = m_builder.new_flow_id();
        local_storage.ucp_worker_create_destroy_flows[event.worker_addr] = flow_id;

        auto* slice = m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "ucx");
        add_header_annotations(slice, file_header);
        slice->add_flow_ids(flow_id);
        m_builder.add_annotation(slice, "Context Addr", event.context_addr);
        m_builder.add_annotation(slice, "Worker Addr", event.worker_addr);
    }

    void visit(EventTrait<ProfilerEvents::UcpWorkerDestroy> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::UcpWorkerDestroy&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice = m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "ucx");
        add_header_annotations(slice, file_header);

        auto& local_storage = process_local_storage(file_header);
        auto it = local_storage.ucp_worker_create_destroy_flows.find(event.worker_addr);
        if (it != local_storage.ucp_worker_create_destroy_flows.end()) {
            slice->add_terminating_flow_ids(it->second);
            local_storage.ucp_worker_create_destroy_flows.erase(it);
        }
        m_builder.add_annotation(slice, "Worker Addr", event.worker_addr);
    }

    void visit(EventTrait<ProfilerEvents::UcpEpCreate> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::UcpEpCreate&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto& local_storage = process_local_storage(file_header);
        const auto flow_id = m_builder.new_flow_id();
        local_storage.ucp_ep_create_close_flows[event.ep_addr] = flow_id;

        auto* slice = m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "ucx");
        add_header_annotations(slice, file_header);
        slice->add_flow_ids(flow_id);
        m_builder.add_annotation(slice, "Worker Addr", event.worker_addr);
        m_builder.add_annotation(slice, "EP Addr", event.ep_addr);
    }

    void visit(EventTrait<ProfilerEvents::UcpEpClose> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::UcpEpClose&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice = m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "ucx");
        add_header_annotations(slice, file_header);

        auto& local_storage = process_local_storage(file_header);
        auto it = local_storage.ucp_ep_create_close_flows.find(event.ep_addr);
        if (it != local_storage.ucp_ep_create_close_flows.end()) {
            slice->add_terminating_flow_ids(it->second);
            local_storage.ucp_ep_create_close_flows.erase(it);
        }
        m_builder.add_annotation(slice, "EP Addr", event.ep_addr);
    }

    void visit(EventTrait<ProfilerEvents::UcpTagSend> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::UcpTagSend&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto& local_storage = process_local_storage(file_header);

        // Cross-node tag flow: begin flow keyed on tag
        const auto flow_id = m_builder.new_flow_id();
        local_storage.ucp_tag_send_flows[event.tag] = flow_id;

        auto* slice = m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "ucx");
        add_header_annotations(slice, file_header);
        slice->add_flow_ids(flow_id);
        m_builder.add_annotation(slice, "EP Addr", event.ep_addr);
        m_builder.add_annotation(slice, "Tag", event.tag);
        m_builder.add_annotation(slice, "Size", event.size);
        m_builder.add_annotation(slice, "Request Addr", event.request_addr);

        // Bandwidth counters for ucp_tag_send
        if (event.size > 0) {
            emit_ucx_node_bytes_sent(node_id, header.timestamp_start, event.size);
            emit_ucx_node_bw_sent(node_id, header.timestamp_start, event.size);
            auto info = resolve_ep_nic(local_storage, event.ep_addr, node_id, true);
            if (info.found) {
                const uint64_t qp_track = m_builder.make_uuid(TrackType::NIC_QP, node_id, info.nic_id, info.qp_num);
                m_builder.register_subdevice_track(info.nic_track_uuid, qp_track, info.qp_num, "QP");
                emit_send_counters(node_id, header.timestamp_start, event.size, local_storage, info.qp_num, false,
                                   info.nic_id, info.nic_track_uuid, qp_track);
            }
        }
    }

    void visit(EventTrait<ProfilerEvents::UcpTagRecv> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::UcpTagRecv&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice = m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "ucx");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "Worker Addr", event.worker_addr);
        m_builder.add_annotation(slice, "Tag", event.tag);
        m_builder.add_annotation(slice, "Tag Mask", event.tag_mask);
        m_builder.add_annotation(slice, "Size", event.size);
        m_builder.add_annotation(slice, "Request Addr", event.request_addr);

        // Bandwidth counters for ucp_tag_recv — no ep_addr, emit only node-level
        if (event.size > 0) {
            emit_ucx_node_bytes_recv(node_id, header.timestamp_start, event.size);
            emit_ucx_node_bw_recv(node_id, header.timestamp_start, event.size);
        }
    }

    void visit(EventTrait<ProfilerEvents::UcpPut> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::UcpPut&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice = m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "ucx");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "EP Addr", event.ep_addr);
        m_builder.add_annotation(slice, "Size", event.size);
        m_builder.add_annotation(slice, "Remote Addr", event.remote_addr);
        m_builder.add_annotation(slice, "Request Addr", event.request_addr);

        // Bandwidth counters for ucp_put (send direction)
        if (event.size > 0) {
            emit_ucx_node_bytes_sent(node_id, header.timestamp_start, event.size);
            emit_ucx_node_bw_sent(node_id, header.timestamp_start, event.size);
            auto& local_storage = process_local_storage(file_header);
            auto info = resolve_ep_nic(local_storage, event.ep_addr, node_id, true);
            if (info.found) {
                const uint64_t qp_track = m_builder.make_uuid(TrackType::NIC_QP, node_id, info.nic_id, info.qp_num);
                m_builder.register_subdevice_track(info.nic_track_uuid, qp_track, info.qp_num, "QP");
                emit_send_counters(node_id, header.timestamp_start, event.size, local_storage, info.qp_num, false,
                                   info.nic_id, info.nic_track_uuid, qp_track);
            }
        }
    }

    void visit(EventTrait<ProfilerEvents::UcpGet> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::UcpGet&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice = m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "ucx");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "EP Addr", event.ep_addr);
        m_builder.add_annotation(slice, "Size", event.size);
        m_builder.add_annotation(slice, "Remote Addr", event.remote_addr);
        m_builder.add_annotation(slice, "Request Addr", event.request_addr);

        // Bandwidth counters for ucp_get (recv direction)
        if (event.size > 0) {
            emit_ucx_node_bytes_recv(node_id, header.timestamp_start, event.size);
            emit_ucx_node_bw_recv(node_id, header.timestamp_start, event.size);
            auto& local_storage = process_local_storage(file_header);
            auto info = resolve_ep_nic(local_storage, event.ep_addr, node_id, true);
            if (info.found) {
                const uint64_t qp_track = m_builder.make_uuid(TrackType::NIC_QP, node_id, info.nic_id, info.qp_num);
                m_builder.register_subdevice_track(info.nic_track_uuid, qp_track, info.qp_num, "QP");
                emit_recv_counters(node_id, header.timestamp_start, event.size, local_storage, info.qp_num, false,
                                   info.nic_id, info.nic_track_uuid, qp_track);
            }
        }
    }

    void visit(EventTrait<ProfilerEvents::UcpAmSend> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::UcpAmSend&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice = m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "ucx");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "EP Addr", event.ep_addr);
        m_builder.add_annotation(slice, "AM ID", event.am_id);
        m_builder.add_annotation(slice, "Header Size", event.header_size);
        m_builder.add_annotation(slice, "Payload Size", event.payload_size);
        m_builder.add_annotation(slice, "Request Addr", event.request_addr);

        // Bandwidth counters for ucp_am_send (send direction)
        const uint64_t am_total = event.header_size + event.payload_size;
        if (am_total > 0) {
            emit_ucx_node_bytes_sent(node_id, header.timestamp_start, am_total);
            emit_ucx_node_bw_sent(node_id, header.timestamp_start, am_total);
            auto& local_storage = process_local_storage(file_header);
            auto info = resolve_ep_nic(local_storage, event.ep_addr, node_id, true);
            if (info.found) {
                const uint64_t qp_track = m_builder.make_uuid(TrackType::NIC_QP, node_id, info.nic_id, info.qp_num);
                m_builder.register_subdevice_track(info.nic_track_uuid, qp_track, info.qp_num, "QP");
                emit_send_counters(node_id, header.timestamp_start, am_total, local_storage, info.qp_num, false,
                                   info.nic_id, info.nic_track_uuid, qp_track);
            }
        }
    }

    void visit(EventTrait<ProfilerEvents::UcpMemMap> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::UcpMemMap&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice = m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "ucx");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "Context Addr", event.context_addr);
        m_builder.add_annotation(slice, "Addr", event.addr);
        m_builder.add_annotation(slice, "Size", event.size);
    }

    void visit(EventTrait<ProfilerEvents::UcpMemUnmap> T, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::UcpMemUnmap&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* slice = m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "ucx");
        add_header_annotations(slice, file_header);
        m_builder.add_annotation(slice, "Context Addr", event.context_addr);
        m_builder.add_annotation(slice, "Mem Addr", event.mem_addr);
    }

    void visit(EventTrait<ProfilerEvents::UcpWorkerProgress> T, const EventFileHeader& file_header,
               EventHeader&& header, ProfilerEvents::UcpWorkerProgress&& event) {
        using Meta = decltype(T);
        const auto node_id = register_node(file_header.hostname);
        const auto cpu_id = file_header.cpu_id;
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);

        auto* instant = m_builder.add_instant_event(track_uuid, header.timestamp_start, Meta::name, "ucx");
        add_header_annotations(instant, file_header);
        m_builder.add_annotation(instant, "Worker Addr", event.worker_addr);
        m_builder.add_annotation(instant, "Num Progressed", event.num_progressed);
    }

    void visit(EventTrait<ProfilerEvents::UcpEpQpMapping>, const EventFileHeader& file_header, EventHeader&&,
               ProfilerEvents::UcpEpQpMapping&& event) {
        auto& local_storage = process_local_storage(file_header);
        local_storage.ucp_ep_to_qp[event.ep_addr] = event.qp_num;
    }

    // ========================================================================

    void visit(EventTrait<ProfilerEvents::CUDAKernelLaunch>, const EventFileHeader& file_header, EventHeader&& header,
               ProfilerEvents::CUDAKernelLaunch&& event) {
        const auto node_id = register_node(file_header.hostname);
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
        const auto node_id = register_node(file_header.hostname);
        const auto gpu_id = event.gpu_id;

        // Ensure GPU track exists
        const uint64_t track_uuid = m_builder.register_device_track(TrackType::GPU, static_cast<uint64_t>(node_id),
                                                                    static_cast<uint64_t>(gpu_id));

        m_builder.end_slice(track_uuid, header.timestamp_start);
    }

    // This is called very early since it's a Meta event
    void visit(EventTrait<ProfilerEvents::TscCalibration>, const EventFileHeader& file_header, EventHeader&&,
               ProfilerEvents::TscCalibration&& event) {
        const auto node_id = register_node(file_header.hostname);
        const auto cal_key = CalibrationKey{node_id, file_header.process_id};

        // Always keep the earliest calibration (smallest tsc_start) per process.
        // Multiple calibrations exist (startup + shutdown), and with stale PIDs
        // after fork, different processes may share the same key. Using the earliest
        // ensures all events have tsc >= tsc_start, avoiding clamping.
        auto it = m_node_calibrations.find(cal_key);
        if (it != m_node_calibrations.end()) {
            if (event.tsc_value < it->second.tsc_start) {
                it->second.tsc_start = event.tsc_value;
                it->second.wall_start_ns = event.wall_ns;
                it->second.tsc_freq_hz = event.tsc_freq_hz;
            }
            return;
        }

        NodeCalibration calibration{
                .tsc_start = event.tsc_value,
                .wall_start_ns = event.wall_ns,
                .tsc_freq_hz = event.tsc_freq_hz,
                .has_calibration = true,
        };

        m_node_calibrations.emplace(cal_key, std::move(calibration));
    }

private:
    // TODO: Set to true when we get the first ibv_open_device
    // Not used currently
    [[maybe_unused]] bool m_is_ready{false};

    std::filesystem::path m_output_path;
    PerfettoTraceBuilder m_builder;

    // GPU-GPU track mode: route events to GPU tracks instead of QP tracks
    bool m_gpu_track_mode{false};

    // ================================================================
    // Bandwidth counter state
    // ================================================================
    struct QpCounterState {
        int64_t cumulative_bytes_sent{0};
        int64_t cumulative_bytes_recv{0};
        uint64_t send_uuid{0};
        uint64_t recv_uuid{0};
    };
    // Key: (node_id, qp_num)
    absl::flat_hash_map<std::pair<uint64_t, uint32_t>, QpCounterState> m_qp_byte_counters;

    struct GpuCounterState {
        int64_t cumulative_bytes_sent{0};
        int64_t cumulative_bytes_recv{0};
        uint64_t send_uuid{0};
        uint64_t recv_uuid{0};
    };
    // Key: (node_id, gpu_id)
    absl::flat_hash_map<std::pair<uint64_t, GpuId_T>, GpuCounterState> m_gpu_byte_counters;

    void emit_qp_send_counter(uint64_t node_id, uint32_t qp_num, uint64_t parent_track_uuid, uint64_t timestamp,
                              uint64_t bytes) {
        auto key = std::make_pair(node_id, qp_num);
        auto& state = m_qp_byte_counters[key];
        if (state.send_uuid == 0) {
            state.send_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterQpBytesSent, node_id, qp_num);
            m_builder.register_counter_track(state.send_uuid, parent_track_uuid,
                                             "QP" + std::to_string(qp_num) + " Bytes Sent");
        }
        state.cumulative_bytes_sent += static_cast<int64_t>(bytes);
        m_builder.emit_counter(state.send_uuid, timestamp, state.cumulative_bytes_sent);
    }

    void emit_qp_recv_counter(uint64_t node_id, uint32_t qp_num, uint64_t parent_track_uuid, uint64_t timestamp,
                              uint64_t bytes) {
        auto key = std::make_pair(node_id, qp_num);
        auto& state = m_qp_byte_counters[key];
        if (state.recv_uuid == 0) {
            state.recv_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterQpBytesRecv, node_id, qp_num);
            m_builder.register_counter_track(state.recv_uuid, parent_track_uuid,
                                             "QP" + std::to_string(qp_num) + " Bytes Recv");
        }
        state.cumulative_bytes_recv += static_cast<int64_t>(bytes);
        m_builder.emit_counter(state.recv_uuid, timestamp, state.cumulative_bytes_recv);
    }

    void emit_gpu_send_counter(uint64_t node_id, GpuId_T gpu_id, uint64_t timestamp, uint64_t bytes) {
        auto key = std::make_pair(node_id, gpu_id);
        auto& state = m_gpu_byte_counters[key];
        if (state.send_uuid == 0) {
            auto gpu_track_uuid =
                    m_builder.register_device_track(TrackType::GPU, node_id, static_cast<uint64_t>(gpu_id));
            state.send_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterGpuBytesSent, node_id, gpu_id);
            m_builder.register_counter_track(state.send_uuid, gpu_track_uuid,
                                             "GPU" + std::to_string(gpu_id) + " Bytes Sent");
        }
        state.cumulative_bytes_sent += static_cast<int64_t>(bytes);
        m_builder.emit_counter(state.send_uuid, timestamp, state.cumulative_bytes_sent);
    }

    void emit_gpu_recv_counter(uint64_t node_id, GpuId_T gpu_id, uint64_t timestamp, uint64_t bytes) {
        auto key = std::make_pair(node_id, gpu_id);
        auto& state = m_gpu_byte_counters[key];
        if (state.recv_uuid == 0) {
            auto gpu_track_uuid =
                    m_builder.register_device_track(TrackType::GPU, node_id, static_cast<uint64_t>(gpu_id));
            state.recv_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterGpuBytesRecv, node_id, gpu_id);
            m_builder.register_counter_track(state.recv_uuid, gpu_track_uuid,
                                             "GPU" + std::to_string(gpu_id) + " Bytes Recv");
        }
        state.cumulative_bytes_recv += static_cast<int64_t>(bytes);
        m_builder.emit_counter(state.recv_uuid, timestamp, state.cumulative_bytes_recv);
    }
    // ================================================================

    // ================================================================
    // Windowed bandwidth counter state
    // ================================================================
    struct WindowedCounterState {
        uint64_t window_size_ns{1'000'000};
        uint64_t window_start_ts{0};
        int64_t bytes_in_window{0};
        uint64_t track_uuid{0};
        bool initialized{false};
    };

    // Key: (node_id, qp_num)
    absl::flat_hash_map<std::pair<uint64_t, uint32_t>, WindowedCounterState> m_qp_bw_sent;
    absl::flat_hash_map<std::pair<uint64_t, uint32_t>, WindowedCounterState> m_qp_bw_recv;
    // Key: (node_id, gpu_id)
    absl::flat_hash_map<std::pair<uint64_t, GpuId_T>, WindowedCounterState> m_gpu_bw_sent;
    absl::flat_hash_map<std::pair<uint64_t, GpuId_T>, WindowedCounterState> m_gpu_bw_recv;
    // Key: (node_id, nic_id)
    absl::flat_hash_map<std::pair<uint64_t, NicId>, WindowedCounterState> m_nic_bw_sent;
    absl::flat_hash_map<std::pair<uint64_t, NicId>, WindowedCounterState> m_nic_bw_recv;
    // Key: node_id
    absl::flat_hash_map<uint64_t, WindowedCounterState> m_node_bw_sent;
    absl::flat_hash_map<uint64_t, WindowedCounterState> m_node_bw_recv;

    uint64_t m_window_size_ns{1'000'000};
    uint64_t m_max_timestamp{0};

    void flush_windows(WindowedCounterState& state, uint64_t ts, int64_t bytes) {
        if (!state.initialized) {
            state.window_start_ts = (ts / state.window_size_ns) * state.window_size_ns;
            state.initialized = true;
        }
        if (ts >= state.window_start_ts + state.window_size_ns) {
            // Emit the current window's bandwidth
            int64_t bw = state.bytes_in_window * 1'000'000'000LL / static_cast<int64_t>(state.window_size_ns);
            m_builder.emit_counter(state.track_uuid, state.window_start_ts + state.window_size_ns, bw);

            // Skip directly to the window containing ts, emitting a single zero
            // point instead of iterating through potentially millions of empty windows
            uint64_t target_window = (ts / state.window_size_ns) * state.window_size_ns;
            if (target_window > state.window_start_ts + state.window_size_ns) {
                m_builder.emit_counter(state.track_uuid, target_window, 0);
            }
            state.window_start_ts = target_window;
            state.bytes_in_window = 0;
        }
        state.bytes_in_window += bytes;
        m_max_timestamp = std::max(m_max_timestamp, ts);
    }

    void emit_qp_bw_sent(uint64_t node_id, uint32_t qp_num, uint64_t parent_uuid, uint64_t ts, uint64_t bytes) {
        auto key = std::make_pair(node_id, qp_num);
        auto& state = m_qp_bw_sent[key];
        state.window_size_ns = m_window_size_ns;
        if (state.track_uuid == 0) {
            state.track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterQpBandwidthSent, node_id, qp_num);
            m_builder.register_counter_track(state.track_uuid, parent_uuid,
                                             "QP" + std::to_string(qp_num) + " Bandwidth Sent", "bytes/s");
        }
        flush_windows(state, ts, static_cast<int64_t>(bytes));
    }

    void emit_qp_bw_recv(uint64_t node_id, uint32_t qp_num, uint64_t parent_uuid, uint64_t ts, uint64_t bytes) {
        auto key = std::make_pair(node_id, qp_num);
        auto& state = m_qp_bw_recv[key];
        state.window_size_ns = m_window_size_ns;
        if (state.track_uuid == 0) {
            state.track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterQpBandwidthRecv, node_id, qp_num);
            m_builder.register_counter_track(state.track_uuid, parent_uuid,
                                             "QP" + std::to_string(qp_num) + " Bandwidth Recv", "bytes/s");
        }
        flush_windows(state, ts, static_cast<int64_t>(bytes));
    }

    void emit_gpu_bw_sent(uint64_t node_id, GpuId_T gpu_id, uint64_t ts, uint64_t bytes) {
        auto key = std::make_pair(node_id, gpu_id);
        auto& state = m_gpu_bw_sent[key];
        state.window_size_ns = m_window_size_ns;
        if (state.track_uuid == 0) {
            const uint64_t gpu_track_uuid =
                    m_builder.register_device_track(TrackType::GPU, node_id, static_cast<uint64_t>(gpu_id));
            state.track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterGpuBandwidthSent, node_id, gpu_id);
            m_builder.register_counter_track(state.track_uuid, gpu_track_uuid,
                                             "GPU" + std::to_string(gpu_id) + " Bandwidth Sent", "bytes/s");
        }
        flush_windows(state, ts, static_cast<int64_t>(bytes));
    }

    void emit_gpu_bw_recv(uint64_t node_id, GpuId_T gpu_id, uint64_t ts, uint64_t bytes) {
        auto key = std::make_pair(node_id, gpu_id);
        auto& state = m_gpu_bw_recv[key];
        state.window_size_ns = m_window_size_ns;
        if (state.track_uuid == 0) {
            const uint64_t gpu_track_uuid =
                    m_builder.register_device_track(TrackType::GPU, node_id, static_cast<uint64_t>(gpu_id));
            state.track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterGpuBandwidthRecv, node_id, gpu_id);
            m_builder.register_counter_track(state.track_uuid, gpu_track_uuid,
                                             "GPU" + std::to_string(gpu_id) + " Bandwidth Recv", "bytes/s");
        }
        flush_windows(state, ts, static_cast<int64_t>(bytes));
    }

    void emit_nic_bw_sent(uint64_t node_id, NicId nic_id, uint64_t nic_track_uuid, uint64_t ts, uint64_t bytes) {
        auto key = std::make_pair(node_id, nic_id);
        auto& state = m_nic_bw_sent[key];
        state.window_size_ns = m_window_size_ns;
        if (state.track_uuid == 0) {
            state.track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNicBandwidthSent, node_id, nic_id);
            m_builder.register_counter_track(state.track_uuid, nic_track_uuid, "NIC Bandwidth Sent", "bytes/s");
        }
        flush_windows(state, ts, static_cast<int64_t>(bytes));
    }

    void emit_nic_bw_recv(uint64_t node_id, NicId nic_id, uint64_t nic_track_uuid, uint64_t ts, uint64_t bytes) {
        auto key = std::make_pair(node_id, nic_id);
        auto& state = m_nic_bw_recv[key];
        state.window_size_ns = m_window_size_ns;
        if (state.track_uuid == 0) {
            state.track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNicBandwidthRecv, node_id, nic_id);
            m_builder.register_counter_track(state.track_uuid, nic_track_uuid, "NIC Bandwidth Recv", "bytes/s");
        }
        flush_windows(state, ts, static_cast<int64_t>(bytes));
    }

    void emit_node_bw_sent(uint64_t node_id, uint64_t ts, uint64_t bytes) {
        auto& state = m_node_bw_sent[node_id];
        state.window_size_ns = m_window_size_ns;
        if (state.track_uuid == 0) {
            const uint64_t node_track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::Node, node_id);
            state.track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNodeBandwidthSent, node_id);
            m_builder.register_counter_track(state.track_uuid, node_track_uuid, "Node Bandwidth Sent", "bytes/s");
        }
        flush_windows(state, ts, static_cast<int64_t>(bytes));
    }

    void emit_node_bw_recv(uint64_t node_id, uint64_t ts, uint64_t bytes) {
        auto& state = m_node_bw_recv[node_id];
        state.window_size_ns = m_window_size_ns;
        if (state.track_uuid == 0) {
            const uint64_t node_track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::Node, node_id);
            state.track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNodeBandwidthRecv, node_id);
            m_builder.register_counter_track(state.track_uuid, node_track_uuid, "Node Bandwidth Recv", "bytes/s");
        }
        flush_windows(state, ts, static_cast<int64_t>(bytes));
    }

    // ================================================================
    // NIC/Node cumulative byte counters
    // ================================================================
    struct NicByteCounterState {
        int64_t cumulative_bytes_sent{0};
        int64_t cumulative_bytes_recv{0};
        uint64_t send_uuid{0};
        uint64_t recv_uuid{0};
    };
    // Key: (node_id, nic_id)
    absl::flat_hash_map<std::pair<uint64_t, NicId>, NicByteCounterState> m_nic_byte_counters;

    struct NodeByteCounterState {
        int64_t cumulative_bytes_sent{0};
        int64_t cumulative_bytes_recv{0};
        uint64_t send_uuid{0};
        uint64_t recv_uuid{0};
    };
    // Key: node_id
    absl::flat_hash_map<uint64_t, NodeByteCounterState> m_node_byte_counters;

    void emit_nic_bytes_sent(uint64_t node_id, NicId nic_id, uint64_t nic_track_uuid, uint64_t timestamp,
                             uint64_t bytes) {
        auto key = std::make_pair(node_id, nic_id);
        auto& state = m_nic_byte_counters[key];
        if (state.send_uuid == 0) {
            state.send_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNicBytesSent, node_id, nic_id);
            m_builder.register_counter_track(state.send_uuid, nic_track_uuid, "NIC Bytes Sent");
        }
        state.cumulative_bytes_sent += static_cast<int64_t>(bytes);
        m_builder.emit_counter(state.send_uuid, timestamp, state.cumulative_bytes_sent);
    }

    void emit_nic_bytes_recv(uint64_t node_id, NicId nic_id, uint64_t nic_track_uuid, uint64_t timestamp,
                             uint64_t bytes) {
        auto key = std::make_pair(node_id, nic_id);
        auto& state = m_nic_byte_counters[key];
        if (state.recv_uuid == 0) {
            state.recv_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNicBytesRecv, node_id, nic_id);
            m_builder.register_counter_track(state.recv_uuid, nic_track_uuid, "NIC Bytes Recv");
        }
        state.cumulative_bytes_recv += static_cast<int64_t>(bytes);
        m_builder.emit_counter(state.recv_uuid, timestamp, state.cumulative_bytes_recv);
    }

    void emit_node_bytes_sent(uint64_t node_id, uint64_t timestamp, uint64_t bytes) {
        auto& state = m_node_byte_counters[node_id];
        if (state.send_uuid == 0) {
            const uint64_t node_track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::Node, node_id);
            state.send_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNodeBytesSent, node_id);
            m_builder.register_counter_track(state.send_uuid, node_track_uuid, "Node Bytes Sent");
        }
        state.cumulative_bytes_sent += static_cast<int64_t>(bytes);
        m_builder.emit_counter(state.send_uuid, timestamp, state.cumulative_bytes_sent);
    }

    void emit_node_bytes_recv(uint64_t node_id, uint64_t timestamp, uint64_t bytes) {
        auto& state = m_node_byte_counters[node_id];
        if (state.recv_uuid == 0) {
            const uint64_t node_track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::Node, node_id);
            state.recv_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNodeBytesRecv, node_id);
            m_builder.register_counter_track(state.recv_uuid, node_track_uuid, "Node Bytes Recv");
        }
        state.cumulative_bytes_recv += static_cast<int64_t>(bytes);
        m_builder.emit_counter(state.recv_uuid, timestamp, state.cumulative_bytes_recv);
    }
    // ================================================================

    // ================================================================
    // UCX/Libfabric-specific node-level counters
    // ================================================================
    absl::flat_hash_map<uint64_t, NodeByteCounterState> m_ucx_node_byte_counters;
    absl::flat_hash_map<uint64_t, WindowedCounterState> m_ucx_node_bw_sent;
    absl::flat_hash_map<uint64_t, WindowedCounterState> m_ucx_node_bw_recv;

    absl::flat_hash_map<uint64_t, NodeByteCounterState> m_fabric_node_byte_counters;
    absl::flat_hash_map<uint64_t, WindowedCounterState> m_fabric_node_bw_sent;
    absl::flat_hash_map<uint64_t, WindowedCounterState> m_fabric_node_bw_recv;

    void emit_ucx_node_bytes_sent(uint64_t node_id, uint64_t timestamp, uint64_t bytes) {
        emit_node_bytes_sent(node_id, timestamp, bytes);
        auto& state = m_ucx_node_byte_counters[node_id];
        if (state.send_uuid == 0) {
            const uint64_t node_track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::Node, node_id);
            state.send_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNodeUcxBytesSent, node_id);
            m_builder.register_counter_track(state.send_uuid, node_track_uuid, "UCX Bytes Sent");
        }
        state.cumulative_bytes_sent += static_cast<int64_t>(bytes);
        m_builder.emit_counter(state.send_uuid, timestamp, state.cumulative_bytes_sent);
    }

    void emit_ucx_node_bytes_recv(uint64_t node_id, uint64_t timestamp, uint64_t bytes) {
        emit_node_bytes_recv(node_id, timestamp, bytes);
        auto& state = m_ucx_node_byte_counters[node_id];
        if (state.recv_uuid == 0) {
            const uint64_t node_track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::Node, node_id);
            state.recv_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNodeUcxBytesRecv, node_id);
            m_builder.register_counter_track(state.recv_uuid, node_track_uuid, "UCX Bytes Recv");
        }
        state.cumulative_bytes_recv += static_cast<int64_t>(bytes);
        m_builder.emit_counter(state.recv_uuid, timestamp, state.cumulative_bytes_recv);
    }

    void emit_ucx_node_bw_sent(uint64_t node_id, uint64_t ts, uint64_t bytes) {
        emit_node_bw_sent(node_id, ts, bytes);
        auto& state = m_ucx_node_bw_sent[node_id];
        state.window_size_ns = m_window_size_ns;
        if (state.track_uuid == 0) {
            const uint64_t node_track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::Node, node_id);
            state.track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNodeUcxBandwidthSent, node_id);
            m_builder.register_counter_track(state.track_uuid, node_track_uuid, "UCX Bandwidth Sent", "bytes/s");
        }
        flush_windows(state, ts, static_cast<int64_t>(bytes));
    }

    void emit_ucx_node_bw_recv(uint64_t node_id, uint64_t ts, uint64_t bytes) {
        emit_node_bw_recv(node_id, ts, bytes);
        auto& state = m_ucx_node_bw_recv[node_id];
        state.window_size_ns = m_window_size_ns;
        if (state.track_uuid == 0) {
            const uint64_t node_track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::Node, node_id);
            state.track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNodeUcxBandwidthRecv, node_id);
            m_builder.register_counter_track(state.track_uuid, node_track_uuid, "UCX Bandwidth Recv", "bytes/s");
        }
        flush_windows(state, ts, static_cast<int64_t>(bytes));
    }

    void emit_fabric_node_bytes_sent(uint64_t node_id, uint64_t timestamp, uint64_t bytes) {
        emit_node_bytes_sent(node_id, timestamp, bytes);
        auto& state = m_fabric_node_byte_counters[node_id];
        if (state.send_uuid == 0) {
            const uint64_t node_track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::Node, node_id);
            state.send_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNodeFabricBytesSent, node_id);
            m_builder.register_counter_track(state.send_uuid, node_track_uuid, "Libfabric Bytes Sent");
        }
        state.cumulative_bytes_sent += static_cast<int64_t>(bytes);
        m_builder.emit_counter(state.send_uuid, timestamp, state.cumulative_bytes_sent);
    }

    void emit_fabric_node_bytes_recv(uint64_t node_id, uint64_t timestamp, uint64_t bytes) {
        emit_node_bytes_recv(node_id, timestamp, bytes);
        auto& state = m_fabric_node_byte_counters[node_id];
        if (state.recv_uuid == 0) {
            const uint64_t node_track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::Node, node_id);
            state.recv_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNodeFabricBytesRecv, node_id);
            m_builder.register_counter_track(state.recv_uuid, node_track_uuid, "Libfabric Bytes Recv");
        }
        state.cumulative_bytes_recv += static_cast<int64_t>(bytes);
        m_builder.emit_counter(state.recv_uuid, timestamp, state.cumulative_bytes_recv);
    }

    void emit_fabric_node_bw_sent(uint64_t node_id, uint64_t ts, uint64_t bytes) {
        emit_node_bw_sent(node_id, ts, bytes);
        auto& state = m_fabric_node_bw_sent[node_id];
        state.window_size_ns = m_window_size_ns;
        if (state.track_uuid == 0) {
            const uint64_t node_track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::Node, node_id);
            state.track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNodeFabricBandwidthSent, node_id);
            m_builder.register_counter_track(state.track_uuid, node_track_uuid, "Libfabric Bandwidth Sent", "bytes/s");
        }
        flush_windows(state, ts, static_cast<int64_t>(bytes));
    }

    void emit_fabric_node_bw_recv(uint64_t node_id, uint64_t ts, uint64_t bytes) {
        emit_node_bw_recv(node_id, ts, bytes);
        auto& state = m_fabric_node_bw_recv[node_id];
        state.window_size_ns = m_window_size_ns;
        if (state.track_uuid == 0) {
            const uint64_t node_track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::Node, node_id);
            state.track_uuid = PerfettoTraceBuilder::make_uuid(TrackType::CounterNodeFabricBandwidthRecv, node_id);
            m_builder.register_counter_track(state.track_uuid, node_track_uuid, "Libfabric Bandwidth Recv", "bytes/s");
        }
        flush_windows(state, ts, static_cast<int64_t>(bytes));
    }
    // ================================================================

    // ================================================================
    // EP→NIC resolution for UCX/libfabric
    // ================================================================
    struct EpNicInfo {
        uint32_t qp_num;
        NicId nic_id;
        uint64_t nic_track_uuid;
        bool found;
    };

    EpNicInfo resolve_ep_nic(ProcessLocalStorage& ls, uint64_t ep_addr, uint64_t node_id, bool is_ucx) {
        auto& ep_map = is_ucx ? ls.ucp_ep_to_qp : ls.fi_ep_to_qp;
        auto qp_it = ep_map.find(ep_addr);
        if (qp_it == ep_map.end())
            return {0, 0, 0, false};
        auto nic_it = ls.qp_to_nic.find(static_cast<uint32_t>(qp_it->second));
        if (nic_it == ls.qp_to_nic.end())
            return {static_cast<uint32_t>(qp_it->second), 0, 0, false};
        auto nic_track = m_builder.register_device_track(TrackType::NIC, node_id, nic_it->second);
        return {static_cast<uint32_t>(qp_it->second), nic_it->second, nic_track, true};
    }

    // Emit all bandwidth/byte counters for a send operation (used by IBV, libfabric, UCX)
    // Note: node-level counters should be emitted by the caller for UCX/libfabric
    // (unconditionally, before EP resolution). For IBV, has_nic=true includes them here.
    void emit_send_counters(uint64_t node_id, uint64_t timestamp, uint64_t bytes, ProcessLocalStorage& ls,
                            uint32_t qp_num, bool has_nic, NicId nic_id, uint64_t nic_track_uuid,
                            uint64_t qp_track_uuid) {
        emit_qp_send_counter(node_id, qp_num, qp_track_uuid, timestamp, bytes);
        emit_qp_bw_sent(node_id, qp_num, qp_track_uuid, timestamp, bytes);

        const auto gpu_it = ls.qp_to_gpu.find(qp_num);
        if (gpu_it != ls.qp_to_gpu.end() && gpu_it->second >= 0) {
            emit_gpu_send_counter(node_id, gpu_it->second, timestamp, bytes);
            emit_gpu_bw_sent(node_id, gpu_it->second, timestamp, bytes);
        }

        if (has_nic) {
            emit_nic_bytes_sent(node_id, nic_id, nic_track_uuid, timestamp, bytes);
            emit_nic_bw_sent(node_id, nic_id, nic_track_uuid, timestamp, bytes);
            emit_node_bytes_sent(node_id, timestamp, bytes);
            emit_node_bw_sent(node_id, timestamp, bytes);
        }
    }

    void emit_recv_counters(uint64_t node_id, uint64_t timestamp, uint64_t bytes, ProcessLocalStorage& ls,
                            uint32_t qp_num, bool has_nic, NicId nic_id, uint64_t nic_track_uuid,
                            uint64_t qp_track_uuid) {
        emit_qp_recv_counter(node_id, qp_num, qp_track_uuid, timestamp, bytes);
        emit_qp_bw_recv(node_id, qp_num, qp_track_uuid, timestamp, bytes);

        const auto gpu_it = ls.qp_to_gpu.find(qp_num);
        if (gpu_it != ls.qp_to_gpu.end() && gpu_it->second >= 0) {
            emit_gpu_recv_counter(node_id, gpu_it->second, timestamp, bytes);
            emit_gpu_bw_recv(node_id, gpu_it->second, timestamp, bytes);
        }

        if (has_nic) {
            emit_nic_bytes_recv(node_id, nic_id, nic_track_uuid, timestamp, bytes);
            emit_nic_bw_recv(node_id, nic_id, nic_track_uuid, timestamp, bytes);
            emit_node_bytes_recv(node_id, timestamp, bytes);
            emit_node_bw_recv(node_id, timestamp, bytes);
        }
    }
    // ================================================================

    void flush_all_windowed_counters() {
        const uint64_t final_ts = m_max_timestamp + m_window_size_ns;
        for (auto& [key, state]: m_qp_bw_sent)
            flush_windows(state, final_ts, 0);
        for (auto& [key, state]: m_qp_bw_recv)
            flush_windows(state, final_ts, 0);
        for (auto& [key, state]: m_gpu_bw_sent)
            flush_windows(state, final_ts, 0);
        for (auto& [key, state]: m_gpu_bw_recv)
            flush_windows(state, final_ts, 0);
        for (auto& [key, state]: m_nic_bw_sent)
            flush_windows(state, final_ts, 0);
        for (auto& [key, state]: m_nic_bw_recv)
            flush_windows(state, final_ts, 0);
        for (auto& [key, state]: m_node_bw_sent)
            flush_windows(state, final_ts, 0);
        for (auto& [key, state]: m_node_bw_recv)
            flush_windows(state, final_ts, 0);
        for (auto& [key, state]: m_ucx_node_bw_sent)
            flush_windows(state, final_ts, 0);
        for (auto& [key, state]: m_ucx_node_bw_recv)
            flush_windows(state, final_ts, 0);
        for (auto& [key, state]: m_fabric_node_bw_sent)
            flush_windows(state, final_ts, 0);
        for (auto& [key, state]: m_fabric_node_bw_recv)
            flush_windows(state, final_ts, 0);
    }
    // ================================================================

    template<typename T>
    uint32_t register_node(T&& key) {
        const auto& [entry, inserted] = m_global_node_ids.try_emplace(std::forward<T>(key), m_current_node_id);
        if (!inserted) {
            return entry->second;
        }

        // Make sure we have local node storage
        m_node_local_storage.push_back({});

        m_builder.register_node_track(entry->second, entry->first);

        ++m_current_node_id;
        return entry->second;
    }

    // TODO: This gives First-come, first-serve IDs
    uint32_t m_current_node_id{0};
    absl::flat_hash_map<std::string, decltype(m_current_node_id)> m_global_node_ids{};

    // ================================================================
    // Global storage
    // ================================================================
    // Source qp_num -> Dest qp_num
    // Has duplicate entries because we don't get a bimap
    absl::flat_hash_map<uint32_t, uint32_t> m_qp_dest_map;

    // Generation-based flow tracking for wr_id reuse disambiguation
    // Key: (dest_qpn, wr_id) -> FlowEntry
    absl::flat_hash_map<std::pair<uint32_t, uint64_t>, FlowEntry> m_global_pending_flows;
    absl::flat_hash_map<std::pair<uint32_t, uint64_t>, uint32_t> m_wr_generation;

    // Communication summaries collected for JSON graph output
    struct CommSummaryEntry {
        std::string hostname;
        ProfilerEvents::CommunicationSummary summary;
    };
    std::vector<CommSummaryEntry> m_comm_summaries;

    // ================================================================
    // Local Storage
    // Node ID
    std::vector<NodeLocalStorage> m_node_local_storage;
    // ================================================================

    // ================================================================
    // TSC Calibration
    // ================================================================
    struct NodeCalibration {
        uint64_t tsc_start{0};
        uint64_t wall_start_ns{0};
        uint64_t tsc_freq_hz{0};
        bool has_calibration{false};
    };
    using CalibrationKey = std::pair<uint32_t /*node_id*/, uint64_t /*process_id*/>;
    absl::flat_hash_map<CalibrationKey, NodeCalibration> m_node_calibrations;

    uint64_t tsc_to_ns(uint64_t tsc, uint32_t node_id, uint64_t process_id) const {
        // return tsc;
        if (tsc == 0) {
            return 0;
        }

        auto it = m_node_calibrations.find({node_id, process_id});
        if (it == m_node_calibrations.end() || !it->second.has_calibration) {
            return tsc; // No calibration available, return raw
        }

        const auto& cal = it->second;

        // Guard against unsigned underflow if tsc < calibration start
        if (tsc < cal.tsc_start) {
            return cal.wall_start_ns;
        }

        const uint64_t tsc_delta = tsc - cal.tsc_start;

        // Split into seconds and remainder to avoid overflow
        const uint64_t seconds = tsc_delta / cal.tsc_freq_hz;
        const uint64_t remainder = tsc_delta % cal.tsc_freq_hz;

        return cal.wall_start_ns + seconds * 1'000'000'000ULL + remainder * 1'000'000'000ULL / cal.tsc_freq_hz;
    }
    // ================================================================

    // Write communication graph JSON
    void write_comm_graph_json(const std::filesystem::path& output_path) const {
        if (m_comm_summaries.empty())
            return;

        auto json_path = output_path;
        json_path.replace_extension(".commgraph.json");

        std::ofstream out(json_path);
        if (!out.is_open()) {
            std::cerr << "Failed to write comm graph to: " << json_path << std::endl;
            return;
        }

        // Build nodes and edges
        out << "{\n  \"nodes\": [\n";
        absl::flat_hash_set<std::string> emitted_nodes;
        bool first_node = true;

        for (const auto& entry: m_comm_summaries) {
            const auto& s = entry.summary;
            std::string node_id = entry.hostname + "_qp" + std::to_string(s.src_qpn);
            if (emitted_nodes.contains(node_id))
                continue;
            emitted_nodes.insert(node_id);

            if (!first_node)
                out << ",\n";
            first_node = false;

            out << "    {\"id\": \"" << node_id << "\", \"node\": \"" << entry.hostname << "\", \"qpn\": " << s.src_qpn
                << ", \"gpu_id\": " << s.gpu_id << ", \"type\": \"" << (s.gpu_id >= 0 ? "gpu" : "cpu") << "\"}";
        }

        out << "\n  ],\n  \"edges\": [\n";
        bool first_edge = true;

        for (const auto& entry: m_comm_summaries) {
            const auto& s = entry.summary;
            if (s.bytes_sent == 0 && s.bytes_recv == 0)
                continue;

            if (!first_edge)
                out << ",\n";
            first_edge = false;

            out << "    {"
                << "\"src\": \"" << entry.hostname << "_qp" << s.src_qpn << "\""
                << ", \"dst_qpn\": " << s.dest_qpn << ", \"bytes_sent\": " << s.bytes_sent
                << ", \"bytes_recv\": " << s.bytes_recv << ", \"send_count\": " << s.send_count
                << ", \"recv_count\": " << s.recv_count << "}";
        }

        out << "\n  ]\n}\n";
        std::cout << "Wrote communication graph to: " << json_path << std::endl;
    }
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <events dir> <output file> [--gpu-tracks]" << std::endl;
        std::exit(1);
    }

    const auto input_path = std::filesystem::current_path() / std::filesystem::path{argv[1]} / "events";
    const auto output_path = std::filesystem::path{argv[2]};

    // bool gpu_track_mode = false;
    bool gpu_track_mode = true;
    uint64_t window_ms = 1;

    for (int i = 3; i < argc; i++) {
        if (std::string_view{argv[i]} == "--gpu-tracks") {
            gpu_track_mode = true;
        } else if (std::string_view{argv[i]} == "--window-ms" && i + 1 < argc) {
            window_ms = std::stoull(argv[i + 1]);
            ++i;
        }
    }

    if (!std::filesystem::exists(input_path)) {
        std::cerr << input_path << " does not exist" << std::endl;
        std::exit(1);
    }

    Events::RuntimeConfig config{};
    config.EventsDirectory = input_path;

    ProfilerEventReader reader{config, output_path};
    reader.set_gpu_track_mode(gpu_track_mode);
    reader.set_window_size_ms(window_ms);

    std::cout << "Reading events from: " << config.EventsDirectory << std::endl;
    std::cout << "Writing output to: " << reader.get_output_path() << std::endl;

    const size_t count = reader.process();
    std::cout << "\nProcessed " << count << " events." << std::endl;

    // Print stats
    // TODO: not clean
    const auto& stats_map = reader.get_stats();
    std::vector<std::pair<std::string_view, std::size_t>> sorted_stats;
    sorted_stats.reserve(stats_map.size());
    for (const auto& entry: stats_map) {
        sorted_stats.emplace_back(entry.first, entry.second);
    }

    sorted_stats.reserve(stats_map.size());
    std::ranges::make_heap(sorted_stats, [](const auto& a, const auto& b) {
        return a.second < b.second;
    });

    std::cout << "=== Stats ===" << std::endl;
    for (const auto& [event_name, event_count]: sorted_stats) {
        std::cout << event_name << ": " << event_count << std::endl;
    }

    return 0;
}
