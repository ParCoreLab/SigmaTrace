#pragma once

#include "infiniband/mlx5dv.h"

#include "HookTrait.hpp"

DEFINE_PRELOAD_HOOK(mlx5dv, init_obj, "mlx5dv_init_obj", int, mlx5dv_obj *, uint64_t);
