# TODO: add to a helper module
function(add_nvshmem_target target)
    add_executable(${target} EXCLUDE_FROM_ALL "${target}.cu")
    set_cuda_rpath(${target})

    set_target_properties(${target} PROPERTIES CUDA_SEPARABLE_COMPILATION ON)

    target_link_libraries(${target} PRIVATE cuda_helper)
    target_link_libraries(${target} PRIVATE CUDA::cudart)
    target_link_libraries(${target} PRIVATE nvshmem::nvshmem_host)
    target_link_libraries(${target} PRIVATE nvshmem::nvshmem_device)
    target_link_libraries(${target} PRIVATE MPI::MPI_CXX)
endfunction()

find_package(MPI REQUIRED)

add_nvshmem_target(nvshmem_mpi_uniqueid)
