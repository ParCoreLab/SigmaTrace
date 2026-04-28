if (SIGMA_HAS_NVSHMEM AND SIGMA_HAS_CUDA)
    function(set_cuda_rpath target)
        set_target_properties(${target} PROPERTIES LINK_FLAGS "-Wl,-rpath,${CUDAToolkit_LIBRARY_DIR}")
    endfunction()

    function(add_nvshmem_target target)
        Sigma_add_executable(${target} EXCLUDE_FROM_ALL "${target}.cu")
        set_cuda_rpath(${target})

        set_target_properties(${target} PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
        target_link_options(${target} PRIVATE -no-pie)

        target_link_libraries(${target} PRIVATE SigmaHelpers)
        target_link_libraries(${target} PRIVATE CUDA::cudart)
        target_link_libraries(${target} PRIVATE CUDA::nvml)
        target_link_libraries(${target} PRIVATE nvshmem::nvshmem_host)
        target_link_libraries(${target} PRIVATE nvshmem::nvshmem_device)
        target_link_libraries(${target} PRIVATE MPI::MPI_CXX)
    endfunction()

    find_package(MPI QUIET)

    add_nvshmem_target(nvshmem_mpi_uniqueid)
    add_nvshmem_target(nvshmem_thread_group)

    add_nvshmem_target(nvshmem_force_hca)
endif ()

if (SIGMA_HAS_NCCL AND SIGMA_HAS_CUDA)
    Sigma_add_executable(nccl_force_hca EXCLUDE_FROM_ALL nccl_force_hca.cu)
    target_link_libraries(nccl_force_hca PRIVATE SigmaHelpers)
    # target_link_libraries(nccl_force_hca PRIVATE CUDA::cudart)

    # For testing
    # target_link_libraries(nccl_force_hca PRIVATE ibverbs)
    target_link_libraries(nccl_force_hca PRIVATE nccl)
endif ()
