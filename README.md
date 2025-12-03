# libibverbs tracer

__**Work in progress**__

### Building:
```bash
mkdir build && cd build/
cmake -S ...
make -j
```

### Dependencies:
CMAKE 4.0+ (gnumake only, ninja may break)

CUDA Toolkit 13.0+

`nccl` and `rdma-core` (`libibverbs`, `libmlx5`) are built locally
