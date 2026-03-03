/*
 * Copyright (c) 2018-2020, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 *
 * See COPYRIGHT.txt for license information
 */

#include <cstdio>

#include <mpi.h>

#include <nvshmem.h>
#include <nvshmemx.h>

#define NTHREADS 512

#undef CUDA_CHECK
#define CUDA_CHECK(stmt)                                                          \
    do {                                                                          \
        cudaError_t result = (stmt);                                              \
        if (cudaSuccess != result) {                                              \
            fprintf(stderr, "[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, \
                    cudaGetErrorString(result));                                  \
            exit(-1);                                                             \
        }                                                                         \
    } while (0)

__global__ void distributed_vector_sum(int *x, int *y, int *partial_sum, int *sum,
                                       int use_threadgroup, int mype, int npes) {
    int index = threadIdx.x;
    int nelems = blockDim.x;
    partial_sum[index] = x[index] + y[index];

    if (use_threadgroup) {
        /* all threads realize the entire fcollect operation */
        nvshmemx_int_fcollect_block(NVSHMEM_TEAM_WORLD, sum, partial_sum, nelems);
    } else {
        /* thread 0 realizes the entire fcollect operation */
        if (0 == index) {
            nvshmem_int_fcollect(NVSHMEM_TEAM_WORLD, sum, partial_sum, nelems);
        }
    }
}

int main(int argc, char *argv[]) {
    int *x;
    int *y;
    int *partial_sum;
    int *sum;
    int use_threadgroup = 1;
    int nthreads = NTHREADS;

    int rank, nranks;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    nvshmemx_init_attr_t attr = NVSHMEMX_INIT_ATTR_INITIALIZER;
    nvshmemx_uniqueid_t id = NVSHMEMX_UNIQUEID_INITIALIZER;

    // PE 0 queries the unique ID
    if (rank == 0) {
        nvshmemx_get_uniqueid(&id);
    }

    // PE 0 broadcast the unique ID to all peers
    MPI_Bcast(&id, sizeof(nvshmemx_uniqueid_t), MPI_UINT8_T, 0, MPI_COMM_WORLD);
    nvshmemx_set_attr_uniqueid_args(rank, nranks, &id, &attr);
    nvshmemx_init_attr(NVSHMEMX_INIT_WITH_UNIQUEID, &attr);
    int mype_node = nvshmem_team_my_pe(NVSHMEMX_TEAM_NODE);

    CUDA_CHECK(cudaSetDevice(mype_node));

    int npes = nvshmem_n_pes();
    int mype = nvshmem_my_pe();
    mype_node = nvshmem_team_my_pe(NVSHMEMX_TEAM_NODE);

    CUDA_CHECK(cudaSetDevice(mype_node));

    x = (int *)nvshmem_malloc(sizeof(int) * nthreads);
    y = (int *)nvshmem_malloc(sizeof(int) * nthreads);
    partial_sum = (int *)nvshmem_malloc(sizeof(int) * nthreads);
    sum = (int *)nvshmem_malloc(sizeof(int) * nthreads * npes);

    void *args[] = {&x, &y, &partial_sum, &sum, &use_threadgroup, &mype, &npes};
    dim3 dimBlock(nthreads);
    dim3 dimGrid(1);
    nvshmemx_collective_launch((const void *)distributed_vector_sum, dimGrid, dimBlock, args, 0, 0);
    CUDA_CHECK(cudaDeviceSynchronize());

    printf("[%d of %d] run complete \n", mype, npes);

    nvshmem_free(x);
    nvshmem_free(y);
    nvshmem_free(partial_sum);
    nvshmem_free(sum);

    nvshmem_finalize();
    MPI_Finalize();

    return 0;
}
