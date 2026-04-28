# SigmaTrace

Library-agnostic observability for CPU and GPU-initiated RDMA communication.
SigmaTrace intercepts the common-denominator layer (`libibverbs` + `mlx5dv`)
and operates beneath libraries like NCCL, NVSHMEM/IBGDA, UCX, libfabric.
For GPU-initiated transports (NVSHMEM IBGDA, NCCL GDAKI), SigmaTrace
interposes on the GPU's UAR doorbell page through the *Memory Hijack-and-Repeat*
mechanism, exposing per-doorbell timing without source modifications,
recompilation, or binary instrumentation. Tracing data is recorded as
per-thread binary event streams that the bundled `ProfilerEventReader`
correlates and exports to Perfetto.

The companion artifact repository
[`SigmaTrace-Artifacts`](https://zenodo/.../SigmaTrace-Artifacts)
hosts the SC26 evaluation case studies. .

## Dependencies

### Required

| Dependency             | Notes                                                                                                                                                                                                 |
|------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Linux x86_64           | The UAR hijack uses `monitorx`/`mwaitx` (AMD Zen 2+) or `umonitor`/`umwait` (Intel Tremont+). It falls back to `_mm_pause` if neither is available. Run `Tools/check-cpu-wait.sh` to confirm support. |
| CMake                  | ≥ 3.31. Use Make (Ninja currently breaks the build).                                                                                                                                                  |
| GCC ≥ 13 or Clang ≥ 20 | C++20 toolchain required.                                                                                                                                                                             |
| CUDA Toolkit           | ≥ 12.9 for the GPU-initiated communication tracking.                                                                                                                                                  |

### Bundled dependencies

| Submodule            | Branch / tag     | Used for                                   |
|----------------------|------------------|--------------------------------------------|
| `Imports/rdma-core`  | tracked          | Bundled `libibverbs` + `libmlx5`           |
| `Imports/perfetto`   | `releases/v50.x` | Trace export (`ProfilerEventReader`)       |
| `Imports/abseil-cpp` | `20260107.0`     | Hash containers used by the trace exporter |
| `Imports/boost`      | `boost-1.90.0`   | `program_options`, intrusive containers    |
| `Imports/protobuf`   | `v34.0`          | Perfetto `.pftrace` schema                 |

Run `git submodule update --init --recursive Imports/X` to selectively pull the source code.
The build system will try to use system libraries before building bundled versions.

### Optional dependencies

- `libibverbs` + `libmlx5` (skip the bundled rdma-core with `-DBUILD_RDMA_CORE=OFF`)
- `nccl` (skip with `-DBUILD_NCCL=OFF`)
- `nvshmem` (skip with `-DBUILD_NVSHMEM=OFF`; will look in `$NVSHMEM_ROOT`
  or under the NVHPC SDK `comm_libs/nvshmem`)
- `ucx` >= 1.16 (via `pkg-config`)
- `libfabric` 2.x (via `pkg-config`)
- `liburing` (via `pkg-config`)
- MPI (`mpicxx`) is required if you build the `ThingNVSHMEM` sample.

## Building

```bash
cmake -S . -B build
cmake --build build -j
```

The first configure pulls in the submodules and the first build compiles them
through CMake's `ExternalProject_Add` — the NCCL build, in particular, takes
several minutes. Subsequent builds reuse the artefacts.

### Against system `nccl` + `rdma-core`

```bash
cmake -S . -B build -DBUILD_NCCL=OFF -DBUILD_RDMA_CORE=OFF
cmake --build build -j
```

### Useful CMake options

| Option                                            | Default | Effect                                                                                      |
|---------------------------------------------------|---------|---------------------------------------------------------------------------------------------|
| `CMAKE_BUILD_TYPE`                                | `Debug` | `Debug` / `Release` / `RelWithDebInfo`                                                      |
| `CMAKE_CUDA_ARCHITECTURES`                        | `80;90` | Override GPUs architecture (e.g. `90`, `80;90`)                                             |
| `SIGMA_GPU_BACKEND`                               | `auto`  | `auto`, `CUDA`, `HIP`, `CUDA;HIP`, `NONE`                                                   |
| `BUILD_RDMA_CORE`                                 | `OFF`   | Build `libibverbs` + `libmlx5` from `Imports/rdma-core`.                                    |
| `BUILD_NCCL`                                      | `OFF`   | Build NCCL from `Imports/nccl`. Requires `BUILD_RDMA_CORE=ON` if rdma-core is also bundled. |
| `BUILD_NVSHMEM`                                   | `OFF`   | Build NVSHMEM from `Imports/nvshmem`.                                                       |
| `SIGMA_ENABLE_RDMA_CORE`                          | `ON`    | Enables/disables ibverbs/mlx5dv tracing.                                                    |
| `SIGMA_ENABLE_UCX`                                | `ON`    | Enables/disables UCX tracing.                                                               |
| `SIGMA_ENABLE_LIBFABRIC`                          | `ON`    | Enables/disables libfabric tracing.                                                         |
| `SIGMA_ENABLE_LTO` / `_ASAN` / `_UBSAN` / `_TSAN` | `OFF`   | Standard build hardening switches.                                                          |

The CMake configure step prints a configuration summary at the end; check that
the providers you expect (`RDMA-core`, `NCCL`, `NVSHMEM`, `UCX`, `libfabric`)
are reported as found.

### Build outputs

under `build/target/`:

```
build/target/
├── Profiler                     CLI launcher binary (`./Profiler --help`)
├── libProfilerShim.so           Profiler LD_PRELOAD shim (loaded last)
├── ProfilerEventReader          Trace correlator / Perfetto exporter
├── lib/                         Internal lib dependencies
└── providers/                   Trace providers
    ├── libibverbs_trace.so
    ├── libmlx5dv_trace.so
    ├── libcupti_trace.so
    ├── libucx_trace.so
    └── libfabric_trace.so
```

## Usage

`Profiler` is the entry point that loads enabled providers dynamically as needed and launches a target application.

The provider `LD_PRELOAD` shims are loaded first before `libProfilerShim.so` which is the control plane
of the profiling workflow. The launcher can be invoked from any working path. It will resolve paths to its
companion shims automatically.

```bash
./build/target/Profiler [options] <program> [program args]
```

### Common invocations

```bash
# Default: load every provider that was compiled in, simple trace mode.
./build/target/Profiler ./my_app

# Trace only the ibverbs path.
./build/target/Profiler --load-none --load-ibtrace ./my_app

# GPU-initiated NVSHMEM/IBGDA workload. Enable the UAR doorbell hijack.
./build/target/Profiler --enable-doorbell-hijack ./my_app

# Multi-rank: run one Profiler per rank under your launcher of choice.
mpirun -np 8 ./build/target/Profiler -f -o traces/ ./my_app
srun --ntasks-per-node=4 ./build/target/Profiler -f --load-cupti=0 ./my_app
```

**When profiling multi-node/multi-process applications**, it is advised to enable
the `--output-overwrite (-f)` flag to ensure `Profiler` processes don't erroneously
exit due to shared output directory. It is safe and expected for all processes
across all nodes to write to the same base directory.

Additionally, each `SigmaTrace` process will attempt to guess the rank of the process its profiling
from its environment (`OMPI_COMM_WORLD_RANK`, etc.). You can overwrite this behavior by explicitly
passing a `--rank-hint=num` or `--rank-hint-env=env_name`.

### Provider flags

| Flag                       | Default | Backend feature flag                                                                                       |
|----------------------------|---------|------------------------------------------------------------------------------------------------------------|
| `--load-none`              | off     | Disable every provider; opt back in selectively.                                                           |
| `--load-ibtrace[=BOOL]`    | on      | RDMA-core (libibverbs)                                                                                     |
| `--load-mlx5dv[=BOOL]`     | on      | RDMA-core (mlx5 direct verbs)                                                                              |
| `--load-cupti[=BOOL]`      | on      | CUDA kernel events                                                                                         |
| `--load-libfabric[=BOOL]`  | on      | libfabric                                                                                                  |
| `--load-ucx[=BOOL]`        | on      | UCX                                                                                                        |
| `--enable-doorbell-hijack` | off     | RDMA-core ∧ CUDA. Interposes on the `mlx5dv_devx_alloc_uar` page and records GPU-initiated doorbell writes |

### Trace-collection flags

| Flag                                   | Default         | Effect                                                                                                                                                                                                                                                       |
|----------------------------------------|-----------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `--trace-mode={simple,aggregate,full}` | `simple`        | `aggregate`: thread-local counters only, no per-event timestamps. `simple`: per-event timestamps without high-frequency operations like `ibv_poll_cq`. `full`: every event including individual `ibv_post_send` work-requests and `ibv_poll_cq` completions. |
| `--rank-hint <N>`                      | unset           | Override the process rank guess in multi-process scenarios.                                                                                                                                                                                                  |
| `--output-dir,-o <path>`               | `EventRecorder` | Trace directory. Can and should be shared across all processes and nodes in a given run.                                                                                                                                                                     |
| `--output-overwrite,-f`                | off             | Reuse `--output-dir` if it already exists. Should be set for multi-process launches.                                                                                                                                                                         |
| `--help,-h`                            | —               | Print the help text.                                                                                                                                                                                                                                         |

### Bypassing the launcher (not recommended)

If you need to integrate with a tool that manages the `LD_PRELOAD` environment, you can preload the providers
and master shim directly:

```bash
# See Profiler.cpp
PROVIDERS=build/target/providers
MASTER=build/target/libProfilerShim.so
LD_PRELOAD=$PROVIDERS/libibverbs_trace.so:$PROVIDERS/libmlx5dv_trace.so:$PROVIDERS/libcupti_trace.so:$PROVIDERS/libucx_trace.so:$PROVIDERS/libfabric_trace.so:$MASTER \
SIGMA_ENV_ShimDirectory='"build/target"' \
SIGMA_ENV_LoadIbtrace='"1"' SIGMA_ENV_LoadMlx5dv='"1"' \
SIGMA_ENV_LoadCupti='"1"' SIGMA_ENV_LoadLibfabric='"1"' SIGMA_ENV_LoadUcx='"1"' \
SIGMA_ENV_EnableDoorbellHijack='"0"' \
SIGMA_ENV_OutputDirectory='"EventRecorder"' SIGMA_ENV_OutputOverwrite='"1"' \
SIGMA_ENV_TraceMode='"simple"' SIGMA_ENV_RankHint='""' SIGMA_ENV_Check='"123"' \
./my_app
```

The master shim may fail without all `SIGMA_ENV_*` variables set. Prefer `Profiler` for normal use.

## Trace output

The resulting trace output is a flat directory containing per-thread binary event streams 

```
<output-dir>/
├── events/   hostname_pid_tid_<rand>_<eventid>.events
└── meta/     metadata events (process info, TSC calibration, …)
```

Each event is a packed struct prefixed by an 16-byte header (start and end timestamps).
Filenames are unique per (host, PID, TID, event-type), so multiple ranks writing to the
same shared filesystem do not collide.

### Post-processing

`ProfilerEventReader` walks the output directory, reconstructs per-QP topology
and doorbell-to-WQE associations, and emits a Perfetto trace and a JSON aggregate
communication graph.

```bash
./build/target/ProfilerEventReader <output-dir> out_file
# Drag the resulting *.pftrace into https://ui.perfetto.dev/ for the unified timeline
```

`ProfilerEventReader` is built only when Perfetto and Protobuf dependencies are available.

## Hardware / software notes

- **GPU-initiated RDMA tracing** currently covers NVIDIA GPUs + Mellanox NICs (the
  `mlx5dv` direct-verbs path). AMD ROCm + Slingshot / EFA paths are observable
  through the `libfabric` provider only. AMD/Pensando NIC support is future
  work.
- **Compiler wrappers** (Cray CC, etc.) often inject extra runtime libraries
  that interfere with `LD_PRELOAD` interception - build with vanilla GCC or
  Clang.

## Helper tools
Helpers under `Tools/`:

- `check-cpu-wait.sh` — print whether the host CPU exposes `mwaitx` or `waitpkg`.
- `nccl-ib-env.sh` — sample NCCL environment for forcing IB and disabling P2P.
- `count_sass_instructions.sh` — quick SASS instruction histogram for a
  disassembled CUDA kernel.
- `check-clang-format.sh` — run `clang-format -n` over the tracked sources.
