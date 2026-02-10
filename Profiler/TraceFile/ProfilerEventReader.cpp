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
#include "../Events/Mlx5DvEvents.hpp"
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
        Events::TypeList<ProfilerEvents::IbvDeviceRegistration, // ProfilerEvents::IbvPortRegistration, //
                         ProfilerEvents::IbvOpenDevice, ProfilerEvents::IbvCloseDevice,                             //
                         ProfilerEvents::IbvCreateQp, ProfilerEvents::IbvDestroyQp, ProfilerEvents::IbvModifyQp,    //
                         ProfilerEvents::IbvMemoryReg, ProfilerEvents::IbvMemoryDereg,                              //
                         ProfilerEvents::IbvPostSend, ProfilerEvents::IbvPostSendWr, ProfilerEvents::IbvPostSendSg, //
                         ProfilerEvents::IbvPostRecv, ProfilerEvents::IbvPostRecvWr,                                //
                         ProfilerEvents::IbvPollCq, ProfilerEvents::IbvCompletion,                                  //
                         ProfilerEvents::Mlx5dvMemoryRegistration, ProfilerEvents::Mlx5dvMemoryDereg,               //
                         ProfilerEvents::DoorbellRing, ProfilerEvents::CUDAKernelLaunch,
                         ProfilerEvents::CUDAKernelExit //
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
        Operation
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
    uint32_t m_sequence_id{1}; // Constant sequence ID (single-threaded writer)
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
    // mlx5dv
    // umem addr / page id -> context addr
    absl::flat_hash_map<uint64_t, uint64_t> mlx5dv_umem_addr_to_ctx;
    absl::flat_hash_map<uint64_t, uint64_t> mlx5dv_uar_addr_to_ctx;

    // TODO: Insane. Rewrite this
    // (context_addr, mem addr / page id) -> flow id
    absl::flat_hash_map<std::pair<uint64_t, uint64_t>, uint64_t> mlx5dv_pending_umem_flows;
    absl::flat_hash_map<std::pair<uint64_t, uint64_t>, uint64_t> mlx5dv_pending_uar_flows;
    // ============================================================================};
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
        // Write final trace to file
        if (!m_builder.write_to_file(m_output_path)) {
            std::cerr << "Failed to write trace to: " << m_output_path << std::endl;
        }
    }

    const std::filesystem::path& get_output_path() {
        return m_output_path;
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
                const auto nic_id = local_storage.ibv_context_to_nic_id.at(event.context_addr);
                return {"nic", nic_id};
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
    uint64_t get_ibv_qp_wr_flow_id(ProcessLocalStorage& local_storage, const auto& event) {
        auto& completion_flow_map = local_storage.pending_ibv_completions;

        uint64_t flow_id{0};
        const auto& flow_entry = completion_flow_map.find({event.qp_num, event.wr_id});
        // WRs IDs are reused sometimes
        if (flow_entry != completion_flow_map.end()) {
            flow_id = flow_entry->second;
        } else {
            flow_id = m_builder.new_flow_id();
            completion_flow_map.insert({{event.qp_num, event.wr_id}, flow_id});
        }

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

        const auto nic_id = local_storage.ibv_context_to_nic_id.at(event.context_addr);

        const uint64_t cpu_track_uuid = m_builder.register_device_track(TrackType::CPU, node_id, cpu_id);
        const uint64_t nic_track_uuid = m_builder.register_device_track(TrackType::NIC, node_id, nic_id);

        const auto ts_start{header.timestamp_start};
        const auto ts_end{header.timestamp_end};

        auto track_uuid = cpu_track_uuid;
        if (is_nic) {
            if constexpr (requires { event.qp_num; }) {
                const uint64_t qp_track_uuid = m_builder.make_uuid(TrackType::NIC_QP, node_id, nic_id, event.qp_num);
                m_builder.register_subdevice_track(nic_track_uuid, qp_track_uuid, event.qp_num, "QP");
                track_uuid = qp_track_uuid;
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

        // Ensure context_addr is registered
        // We do it here because multiple subsequent ibv_open_device + ibv_close_device
        // calls can reopen a given context_addr with a different device. Great stuff.
        const auto& [_, inserted] =
                local_storage.ibv_context_to_nic_id.try_emplace(event.context_addr, event.node_guid);

        if (!inserted) {
            std::cerr << "Device registration with duplicate context addr " << std::hex << event.context_addr
                      << std::dec << " at node_guid " << event.node_guid;
            std::cerr << ". Something has gone awry" << std::endl;
            std::abort();
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

        // Erase after we no longer need it
        local_storage.ibv_context_to_nic_id.erase(event.context_addr);
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

        const auto attr = event.attr;
        const auto dest_qp_num = attr.dest_qp_num;

        // NO!!! NOT THIS
        m_qp_dest_map.emplace(event.qp_num, dest_qp_num);

        const auto dgid = attr.ah_attr.grh.dgid;
        const auto interface_id = be64toh(dgid.global.interface_id);
        const auto subnet_prefix = be64toh(dgid.global.subnet_prefix);

        const auto dlid = attr.ah_attr.dlid;

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

        // Try to terminate ibv flows
        auto& local_storage = process_local_storage(file_header);
        auto& completion_flow_map = local_storage.pending_ibv_completions;
        const auto& flow_entry = completion_flow_map.find({event.qp_num, event.wr_id});
        if (flow_entry == completion_flow_map.end()) {
            std::cerr << "WARNING: IbvCompletion found no preceding flows" << std::endl;
        } else {
            const auto flow_id = flow_entry->second;
            slice->add_terminating_flow_ids(flow_id);
            completion_flow_map.erase(flow_entry);
        }

        m_builder.add_annotation("QP Num", event.qp_num);
        m_builder.add_annotation("WR ID", event.wr_id);
        m_builder.add_annotation("Opcode", event.opcode);
        m_builder.add_annotation("Status", event.status);
        m_builder.add_annotation("Byte len", event.pkey_index);
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

        // Sometimes we see duplicate mem deregs
        const auto context_addr = local_storage.mlx5dv_umem_addr_to_ctx.at(event.umem_addr);

        auto* slice =
                m_builder.add_slice(track_uuid, header.timestamp_start, header.timestamp_end, Meta::name, "mlx5dv");
        add_header_annotations(slice, file_header);

        const auto flow_id = local_storage.mlx5dv_pending_umem_flows.at({context_addr, event.mem_id});
        slice->add_terminating_flow_ids(flow_id);
        local_storage.mlx5dv_pending_umem_flows.erase({context_addr, event.mem_id});
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
        m_builder.add_annotation("wqe_index", event.wqe_index);
        m_builder.add_annotation("qp_num", event.qp_num);
        m_builder.add_annotation("Is_CPU_Hijack", event.is_cpu_hijack ? "Yes" : "No");
    }

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

private:
    // TODO: Set to true when we get the first ibv_open_device
    // Not used currently
    [[maybe_unused]] bool m_is_ready{false};

    std::filesystem::path m_output_path;
    PerfettoTraceBuilder m_builder;

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

    // Maps globally unique (hopefully) ibv ids to our local enumeration
    // uint32_t m_current_nic_id{0};
    // absl::flat_hash_map<DeviceId, decltype(m_current_nic_id)> m_global_nic_ids;

    // ================================================================
    // Global storage
    // ================================================================
    // Source qp_num -> Dest qp_num
    // Has duplicate entries because we don't get a bimap
    absl::flat_hash_map<uint32_t, uint32_t> m_qp_dest_map;

    // ================================================================
    // Local Storage
    // Node ID
    std::vector<NodeLocalStorage> m_node_local_storage;

    // Node ID, Process ID, Thread ID
    // absl::flat_hash_map<std::tuple<GpuId_T, GpuId_T, GpuId_T>, LocalStorage> m_thread_local_storage;
    // ================================================================
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
