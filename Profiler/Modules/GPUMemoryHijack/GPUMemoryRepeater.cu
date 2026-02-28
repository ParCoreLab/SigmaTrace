#include <cassert>
#include <cstdint>

#include <cuda.h>
#include <cuda_runtime.h>

#include "GPUMemoryRepeater.cuh"

#include <infiniband/mlx5dv.h>

namespace Profiler::GpuRepeater::detail {
    // ============================================================
    // Device code
    // ============================================================
    __device__ __forceinline__ uint32_t bswap32(const uint32_t v) {
        return __byte_perm(v, 0, 0x0123);
    }

    __device__ __forceinline__ uint64_t bswap64(const uint64_t v) {
        const auto lo = static_cast<uint32_t>(v);
        const auto hi = static_cast<uint32_t>(v >> 32);
        return (static_cast<uint64_t>(__byte_perm(lo, 0, 0x0123)) << 32) | __byte_perm(hi, 0, 0x0123);
    }

    // Minimal WQE segment layouts for device-side parsing.
    // Matches mlx5 PRM; fields are big-endian on wire.
    struct DevCtrlSeg {
        uint32_t opmod_idx_opcode;
        uint32_t qpn_ds;
        uint32_t fm_ce_se;
        uint32_t imm;
    } __attribute__((__packed__)) __attribute__((__aligned__(4)));

    struct DevRaddrSeg {
        uint64_t raddr;
        uint32_t rkey;
        uint32_t reserved;
    };

    struct DevDataSeg {
        uint32_t byte_count;
        uint32_t lkey;
        uint64_t addr;
    };

    __device__ void parse_and_record(const uint8_t* wqe, uint32_t qpn, int qp_type, uint16_t doorbell_prod_idx,
                                     uint64_t timestamp, GpuWqeRecord* result_ring, uint32_t& result_head,
                                     volatile uint32_t* result_tail_ptr) {
        auto* ctrl = reinterpret_cast<const DevCtrlSeg*>(wqe);
        uint32_t qpn_ds = bswap32(ctrl->qpn_ds);
        uint32_t opmod = bswap32(ctrl->opmod_idx_opcode);

        uint8_t opcode = opmod & 0xFF;
        uint8_t ds = qpn_ds & 0xFF;
        uint16_t wqe_idx = (opmod >> 8) & 0xFFFF;

        const uint8_t* ptr = wqe + sizeof(DevCtrlSeg);

        // DCI QPs have an address vector segment after ctrl
        if (qp_type == QP_TYPE_DCI) {
            ptr += (ds <= 4) ? 16 : 48;
        }

        GpuWqeRecord rec{};
        rec.timestamp = timestamp;
        rec.qpn = qpn;
        rec.wqe_idx = wqe_idx;
        rec.doorbell_prod_idx = doorbell_prod_idx;
        rec.opcode = opcode;
        rec.ds = ds;
        rec.gpu_id = 0;

        switch (opcode) {
            case mlx5_opcodes::RDMA_WRITE:
            case mlx5_opcodes::RDMA_READ: {
                auto* rs = reinterpret_cast<const DevRaddrSeg*>(ptr);
                auto* dseg = reinterpret_cast<const DevDataSeg*>(ptr + sizeof(DevRaddrSeg));
                rec.raddr = bswap64(rs->raddr);
                rec.rkey = bswap32(rs->rkey);
                rec.laddr = bswap64(dseg->addr);
                rec.lkey = bswap32(dseg->lkey);
                rec.byte_count = bswap32(dseg->byte_count);
                break;
            }
            case mlx5_opcodes::SEND:
            case mlx5_opcodes::SEND_IMM: {
                auto* dseg = reinterpret_cast<const DevDataSeg*>(ptr);
                rec.laddr = bswap64(dseg->addr);
                rec.lkey = bswap32(dseg->lkey);
                rec.byte_count = bswap32(dseg->byte_count);
                break;
            }
            case mlx5_opcodes::ATOMIC_CS:
            case mlx5_opcodes::ATOMIC_FA: {
                auto* rs = reinterpret_cast<const DevRaddrSeg*>(ptr);
                // Skip atomic seg (16 bytes) between raddr and data
                auto* dseg = reinterpret_cast<const DevDataSeg*>(ptr + sizeof(DevRaddrSeg) + 16);
                rec.raddr = bswap64(rs->raddr);
                rec.rkey = bswap32(rs->rkey);
                rec.laddr = bswap64(dseg->addr);
                rec.byte_count = 8; // atomics always 8 bytes
                break;
            }
            default:
                break;
        }

        // Drop rather than stall if ring is full
        if (result_head - *result_tail_ptr >= GpuRepeaterChannel::RESULT_RING_SIZE) {
            return;
        }

        uint32_t slot = result_head & GpuRepeaterChannel::RESULT_RING_MASK;
        result_ring[slot] = rec;
        result_head++;
    }

    __global__ void parse_and_record_oneshot(volatile void* sq_base, uint32_t qpn, uint32_t log_wq_size, int qp_type,
                                             uint32_t prod_id_start, uint32_t prod_id_end,
                                             GpuRepeaterChannel* channel) {

        const uint32_t wq_mask = (1 << log_wq_size) - 1;
        const auto* sq = static_cast<const volatile uint8_t*>(sq_base);
        uint32_t local_result_head = 0;

        for (uint16_t i = prod_id_start; i < prod_id_end;) {
            const auto* wqe = sq + (i & wq_mask) * WQEBB_SIZE;
            auto* ctrl = reinterpret_cast<const volatile mlx5_wqe_ctrl_seg*>(wqe);

            const uint8_t ds = bswap32(ctrl->qpn_ds) & 0xFF;
            const uint8_t bbs = (ds + 3) / 4;

            uint32_t opmod = bswap32(ctrl->opmod_idx_opcode);

            uint8_t opcode = opmod & 0xFF;
            uint16_t wqe_idx = (opmod >> 8);

            // parse_and_record(wqe, qpn, qp_type, prod_id_end, 0 /* ts */, channel->result_ring, local_result_head,
            //                  &channel->result_tail);
            i += max(1u, bbs);

            // printf("okokoka %d %llu %d %d %d %d %d %d\n", wqe_idx, *(uint64_t *)sq_base, ds, i, prod_id_start,
            // prod_id_end, bbs, ds);
        }
    }

    __global__ void gpu_doorbell_intercept_kernel(GpuRepeaterChannel* channel) {
        using namespace detail;

        // Kernel-local per-QP / per-UAR state
        uint16_t last_prod[GpuRepeaterChannel::MAX_QPS] = {};
        uint64_t last_db_val[GpuRepeaterChannel::MAX_UARS] = {};

        int known_num_uars = 0;
        int known_num_qps = 0;
        uint32_t local_result_head = 0;

        while (!channel->shutdown) {
            // Pick up newly registered UARs / QPs
            const int cur_uars = channel->num_uars;
            const int cur_qps = channel->num_qps;
            if (cur_uars != known_num_uars || cur_qps != known_num_qps) {
                __threadfence_system(); // see table entries written by host
                known_num_uars = cur_uars;
                known_num_qps = cur_qps;
            }

            bool any_work = false;

            for (int u = 0; u < known_num_uars; u++) {
                // Fast poll: fake doorbell lives in GPU HBM (L2 hit)
                uint64_t val =
                        *static_cast<volatile uint64_t*>(reinterpret_cast<uint64_t*>(channel->uars[u].fake_addr));

                // printf("%llu | %llu\n", val, last_db_val[u]);

                if (val == last_db_val[u]) {
                    continue;
                }

                printf("UPDATING\n");

                any_work = true;

                // Device fence: ensure SQ writes from NVSHMEM thread are visible
                // (they were committed to L2 by the st.release.sys on the doorbell)
                __threadfence();

                // Forward doorbell to real NIC UAR IMMEDIATELY.
                // threadfence_system ensures SQ data is at PCIe coherence point
                // before the NIC sees the doorbell.
                __threadfence_system();
                atomicExch(reinterpret_cast<unsigned long long*>(channel->uars[u].real_addr),
                           static_cast<unsigned long long>(val));

                last_db_val[u] = val;

                continue;

                // Parse doorbell fields
                uint32_t dw0 = bswap32(static_cast<uint32_t>(val >> 32)); // qpn_ds
                uint32_t dw1 = bswap32(static_cast<uint32_t>(val));       // opmod_idx_opcode
                uint32_t qpn = dw0 >> 8;
                uint16_t prod_idx = (dw1 >> 8) & 0xFFFF;
                uint64_t ts = clock64();

                // Find QP (linear scan — small array, single thread)
                int qp_idx = -1;
                for (int q = 0; q < known_num_qps; q++) {
                    if (channel->qps[q].qpn == qpn) {
                        qp_idx = q;
                        break;
                    }
                }
                if (qp_idx < 0)
                    continue; // unknown QP — forwarded but not parsed

                const auto& qp = channel->qps[qp_idx];
                const uint16_t old_prod = last_prod[qp_idx];
                last_prod[qp_idx] = prod_idx;

                const uint32_t wq_mask = (1 << qp.log_wq_size) - 1;
                const uint8_t* sq = static_cast<const uint8_t*>(qp.sq_base);

                for (uint16_t i = old_prod; i != prod_idx;) {
                    const uint8_t* wqe = sq + (i & wq_mask) * WQEBB_SIZE;
                    auto* ctrl = reinterpret_cast<const DevCtrlSeg*>(wqe);
                    uint8_t ds = bswap32(ctrl->qpn_ds) & 0xFF;
                    uint8_t bbs = (ds + 3) / 4;

                    // parse_and_record(wqe, qp, prod_idx, ts, const_cast<GpuWqeRecord*>(channel->result_ring),
                    //                  local_result_head, &channel->result_tail);
                    i += bbs;
                }
            }

            // Publish results to host
            if (any_work) {
                __threadfence_system();
                channel->result_head = local_result_head;
            } else {
                // #if __CUDA_ARCH__ >= 700
                __nanosleep(50);
                // #endif
            }
        }
    }

    __global__ void test_kernel() {}

    CUresult launch_test_kernel(cudaStream_t stream) {
        static CUfunction test_kernel_function = std::invoke([]() {
            CUfunction f;
            CUDA_CHECK(cudaGetFuncBySymbol(&f, reinterpret_cast<void*>(test_kernel)));
            return f;
        });

        void* args[]{};
        return cuLaunchKernel(test_kernel_function, 1, 1, 1, 1, 1, 1, 0, stream, args, nullptr);
    }
} // namespace Profiler::GpuRepeater::detail
