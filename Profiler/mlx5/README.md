# mlx5 headers

These are internal headers that NVSHMEM and DOCA
use to directly initialize mlx5dv objects instead
of calling `mlx5dv_init_obj`. The objects are then
fed to `mlx5dv_devx_obj_create`

We need these headers here because they have internal
mlx5dv definitions and helper macros.

Copied from `nvshmem/src/modules/transport/common/`