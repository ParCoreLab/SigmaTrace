#pragma once

#include <functional>
#include <tuple>
#include <type_traits>

namespace hook {

    // ================================================================
    // Hook type - how is this hook installed?
    // ================================================================
    enum class HookType {
        ObjectOps, // Replace function pointer in object (ctx->ops.X)
        Preload,   // LD_PRELOAD / dlsym interception
        Custom,
        // Mixed,
    };

    // ================================================================
    // Base hook trait - specialized per tag
    // All hook traits must provide:
    //   - FuncPtr: function pointer type
    //   - RetType: return type
    //   - PreCallback: callback signature for pre-hooks
    //   - PostCallback: callback signature for post-hooks (includes return value)
    //   - type: HookType enum
    //   - name: human-readable name
    // ================================================================
    template<typename Tag>
    struct HookTrait;

    // ================================================================
    // Helper to check if a trait is defined for a tag
    // ================================================================
    template<typename Tag, typename = void>
    struct has_hook_trait : std::false_type {};

    template<typename Tag>
    struct has_hook_trait<Tag, std::void_t<typename HookTrait<Tag>::FuncPtr>> : std::true_type {};

    template<typename Tag>
    inline constexpr bool has_hook_trait_v = has_hook_trait<Tag>::value;

    // ================================================================
    // Concept for valid hook tags
    // ================================================================
    template<typename Tag>
    concept BaseValidTag = requires {
        { HookTrait<Tag>::type } -> std::convertible_to<HookType>;
        // TODO:
        // { HookTrait<Tag>::name } -> std::convertible_to<const char *>;
    };

    template<typename Tag>
    concept ValidEventTag = BaseValidTag<Tag> && requires { typename HookTrait<Tag>::Callback; };

    // TODO: bad naming 1
    template<typename Tag>
    concept ValidCallbackTag = BaseValidTag<Tag> && requires {
        typename HookTrait<Tag>::FuncPtr;
        typename HookTrait<Tag>::RetType;
        typename HookTrait<Tag>::PreCallback;
        typename HookTrait<Tag>::PostCallback;
    };

    // TODO: bad naming 2
    template<typename Tag>
    concept ValidHookTag = ValidEventTag<Tag> || ValidCallbackTag<Tag>;

    // ================================================================
    // Macro to define a preload-style hook (global function interposition)
    //
    // Usage:
    //   DEFINE_PRELOAD_HOOK(namespace, func_name, symbol_name, symbol_version, ret_type, arg_types...)
    //
    // Example:
    //   DEFINE_PRELOAD_HOOK(ibv, reg_mr, "ibv_reg_mr", ibv_mr*, ibv_pd*, void*, size_t, int)
    //
    // TODO: merge and add HookType as an argument
    // ================================================================
#define DEFINE_PRELOAD_HOOK(ns, name, sym_name, ret_type, ...)                                                         \
    namespace ns {                                                                                                     \
        struct name##_tag {};                                                                                          \
    }                                                                                                                  \
    template<>                                                                                                         \
    struct hook::HookTrait<ns::name##_tag> {                                                                           \
        using FuncPtr = ret_type (*)(__VA_ARGS__);                                                                     \
        using RetType = ret_type;                                                                                      \
        using PreCallback = std::function<void(__VA_ARGS__)>;                                                          \
        using PostCallback = std::function<void(ret_type, __VA_ARGS__)>;                                               \
        static constexpr hook::HookType type = hook::HookType::Preload;                                                \
        static constexpr const char *name = #name;                                                                     \
        static constexpr const char *symbol_name = sym_name;                                                           \
    }

    // ================================================================
    // Macro to define an object-ops style hook (function pointer in object)
    //
    // Usage:
    //   DEFINE_OBJECT_OPS_HOOK(namespace, func_name, ret_type, arg_types...)
    //
    // Example:
    //   DEFINE_OBJECT_OPS_HOOK(ibv, post_send, int, ibv_qp*, ibv_send_wr*, ibv_send_wr**)
    // ================================================================
#define DEFINE_OBJECT_OPS_HOOK(ns, name, sym_name, ret_type, ...)                                                      \
    namespace ns {                                                                                                     \
        struct name##_tag {};                                                                                          \
    }                                                                                                                  \
    template<>                                                                                                         \
    struct hook::HookTrait<ns::name##_tag> {                                                                           \
        using FuncPtr = ret_type (*)(__VA_ARGS__);                                                                     \
        using RetType = ret_type;                                                                                      \
        using PreCallback = std::function<void(__VA_ARGS__)>;                                                          \
        using PostCallback = std::function<void(ret_type, __VA_ARGS__)>;                                               \
        static constexpr hook::HookType type = hook::HookType::ObjectOps;                                              \
        static constexpr const char *name = #name;                                                                     \
        static constexpr const char *symbol_name = sym_name;                                                           \
    }

    // TODO: this is WRONG. There should not be pre/post callbacks..........
    // C++23 will fix this (maybe)
#define DEFINE_CUSTOM_HOOK(ns, name, ...)                                                                              \
    namespace ns {                                                                                                     \
        struct name##_tag {};                                                                                          \
    }                                                                                                                  \
    template<>                                                                                                         \
    struct hook::HookTrait<ns::name##_tag> {                                                                           \
        using FuncPtr = void (*)(__VA_ARGS__);                                                                                    \
        using RetType = void;                                                                                          \
        using Callback = std::function<void(__VA_ARGS__)>;                                                             \
        using PreCallback = std::function<void(__VA_ARGS__)>;                                                                     \
        using PostCallback = std::function<void(__VA_ARGS__)>;                                                                    \
        static constexpr hook::HookType type = hook::HookType::Custom;                                                 \
        static constexpr const char *name = #name;                                                                     \
    }

    // ================================================================
    // Type traits for hook categorization
    // ================================================================
    template<typename Tag>
    struct is_preload_hook : std::bool_constant<HookTrait<Tag>::type == HookType::Preload> {};

    template<typename Tag>
    inline constexpr bool is_preload_hook_v = is_preload_hook<Tag>::value;

    template<typename Tag>
    struct is_object_ops_hook : std::bool_constant<HookTrait<Tag>::type == HookType::ObjectOps> {};

    template<typename Tag>
    inline constexpr bool is_object_ops_hook_v = is_object_ops_hook<Tag>::value;

    template<typename Tag>
    struct is_custom_hook : std::bool_constant<HookTrait<Tag>::type == HookType::Custom> {};

    template<typename Tag>
    inline constexpr bool is_custom_hook_v = is_custom_hook<Tag>::value;

} // namespace hook
