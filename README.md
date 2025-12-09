# libibverbs tracer

__**Work in progress**__

### Usage:

**Using launcher**:
```bash
$ ./build/Ibtrace/ibverbs_profiler <program> [program args]
```

**Using LD_PRELOAD**:
```bash
$ LD_PRELOAD=./build/lib/libprofiler_trace.so <program> [program args]
```

**With nsys** (for NVTX):
```bash
$ nsys profile --trace=cuda,nvtx [nsys args] ibverbs_profiler <program> [program args]
# OR
$ LD_PRELOAD=... nsys profile --trace=cuda,nvtx [nsys args] <program> [program args]
# OR
$ mpirun -np <num ranks> nsys profile --trace=cuda,nvtx [nsys args] ibverbs_profiler <program> [program args]
````

### Building:

**With bundled libraries** (will compile nccl and rdma-core):
```bash
mkdir build && cd build/
cmake -S ..
make -j
```

**With system libraries**:
```bash
mkdir build && cd build/
cmake -S .. -DBUILD_NCCL=OFF -DBUILD_RDMA_CORE=OFF
make -j
```

### Dependencies:
- CMAKE 4.0+ (gnumake only, ninja may break)
- CUDA Toolkit 13.0+
- `nccl` and `rdma-core` (`libibverbs`, `libmlx5`) if local built is turned off
