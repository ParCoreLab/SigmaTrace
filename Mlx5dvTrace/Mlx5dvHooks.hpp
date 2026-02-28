#pragma once

#include <variant>

#include "infiniband/mlx5dv.h"

#include "HookTrait.hpp"

DEFINE_PRELOAD_HOOK(mlx5dv, open_device, "mlx5dv_open_device", ibv_context*, ibv_device*, mlx5dv_context_attr*);
DEFINE_PRELOAD_HOOK(mlx5dv, query_device, "mlx5dv_query_device", int, ibv_context*, mlx5dv_context*);

DEFINE_PRELOAD_HOOK(mlx5dv, init_obj, "mlx5dv_init_obj", int, mlx5dv_obj*, uint64_t);

/*
 * https://manpages.debian.org/unstable/libibverbs-dev/mlx5dv_devx_obj_create.3.en.html
 * mlx5dv_devx_obj_create - Creates a devx object
 * mlx5dv_devx_obj_destroy - Destroys a devx object
 * mlx5dv_devx_obj_modify - Modifies a devx object
 * mlx5dv_devx_obj_query - Queries a devx object
 * mlx5dv_devx_obj_query_async - Queries a devx object in an asynchronous mode
 * mlx5dv_devx_general_cmd - Issues a general command over the devx interface
 */
// clang-format off
DEFINE_PRELOAD_HOOK(mlx5dv, obj_create, "mlx5dv_devx_obj_create", mlx5dv_devx_obj *, ibv_context *, const void *, size_t, void *, size_t);
DEFINE_PRELOAD_HOOK(mlx5dv, obj_modify, "mlx5dv_devx_obj_modify", int, mlx5dv_devx_obj *, const void *, size_t, void *, size_t);

DEFINE_PRELOAD_HOOK(mlx5dv, devx_umem_reg, "mlx5dv_devx_umem_reg", mlx5dv_devx_umem *, ibv_context *, void *, size_t, uint32_t);
DEFINE_PRELOAD_HOOK(mlx5dv, devx_umem_dereg, "mlx5dv_devx_umem_dereg", int, mlx5dv_devx_umem *);

DEFINE_PRELOAD_HOOK(mlx5dv, devx_alloc_uar, "mlx5dv_devx_alloc_uar", mlx5dv_devx_uar *, ibv_context *, uint32_t);
DEFINE_PRELOAD_HOOK(mlx5dv, devx_free_uar, "mlx5dv_devx_free_uar", std::monostate, struct mlx5dv_devx_uar *devx_uar);
// clang-format on
