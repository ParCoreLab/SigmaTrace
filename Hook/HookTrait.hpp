#pragma once

#include <functional>
#include <tuple>
#include <type_traits>

// TODO
#define STR(x) #x

#define FE_1(m, x) m(x)
#define FE_2(m, x, ...) m(x), FE_1(m, __VA_ARGS__)
#define FE_3(m, x, ...) m(x), FE_2(m, __VA_ARGS__)
#define FE_4(m, x, ...) m(x), FE_3(m, __VA_ARGS__)
#define FE_5(m, x, ...) m(x), FE_4(m, __VA_ARGS__)
#define FE_6(m, x, ...) m(x), FE_5(m, __VA_ARGS__)
#define FE_7(m, x, ...) m(x), FE_6(m, __VA_ARGS__)
#define FE_8(m, x, ...) m(x), FE_7(m, __VA_ARGS__)

#define GET_FE(_1, _2, _3, _4, _5, _6, _7, _8, NAME, ...) NAME
#define FOR_EACH(m, ...) GET_FE(__VA_ARGS__, FE_8, FE_7, FE_6, FE_5, FE_4, FE_3, FE_2, FE_1)(m, __VA_ARGS__)

#define VA_ARGS_TO_STRING_TUPLE(...) std::make_tuple(FOR_EACH(STR, __VA_ARGS__))

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
        { HookTrait<Tag>::name } -> std::convertible_to<const char *>;
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
    // TODO: ret_type before sym_name, so it looks more natural. e.g. ... int "ibv_open_device" ...
    // ================================================================
#define DEFINE_PRELOAD_HOOK(ns, hook_name, sym_name, ret_type, ...)                                                    \
    namespace ns {                                                                                                     \
        struct hook_name##_tag {};                                                                                     \
    }                                                                                                                  \
    template<>                                                                                                         \
    struct hook::HookTrait<ns::hook_name##_tag> {                                                                      \
        using FuncPtr = ret_type (*)(__VA_ARGS__);                                                                     \
        using RetType = ret_type;                                                                                      \
        using PreCallback = std::function<void(__VA_ARGS__)>;                                                          \
        using PostCallback = std::function<void(ret_type, __VA_ARGS__)>;                                               \
        static constexpr hook::HookType type = hook::HookType::Preload;                                                \
        static constexpr const char *name = #hook_name;                                                                \
        static constexpr const char *symbol_name = sym_name;                                                           \
        static constexpr auto arg_names = VA_ARGS_TO_STRING_TUPLE(__VA_ARGS__);                                        \
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
#define DEFINE_OBJECT_OPS_HOOK(ns, hook_name, sym_name, ret_type, ...)                                                 \
    namespace ns {                                                                                                     \
        struct hook_name##_tag {};                                                                                     \
    }                                                                                                                  \
    template<>                                                                                                         \
    struct hook::HookTrait<ns::hook_name##_tag> {                                                                      \
        using FuncPtr = ret_type (*)(__VA_ARGS__);                                                                     \
        using RetType = ret_type;                                                                                      \
        using PreCallback = std::function<void(__VA_ARGS__)>;                                                          \
        using PostCallback = std::function<void(ret_type, __VA_ARGS__)>;                                               \
        static constexpr hook::HookType type = hook::HookType::ObjectOps;                                              \
        static constexpr const char *name = #hook_name;                                                                \
        static constexpr const char *symbol_name = sym_name;                                                           \
        static constexpr auto arg_names = VA_ARGS_TO_STRING_TUPLE(__VA_ARGS__);                                        \
    }

    // TODO: this is WRONG. There should not be pre/post callbacks..........
    // C++23 will fix this (maybe)
#define DEFINE_CUSTOM_HOOK(ns, hook_name, ...)                                                                         \
    namespace ns {                                                                                                     \
        struct hook_name##_tag {};                                                                                     \
    }                                                                                                                  \
    template<>                                                                                                         \
    struct hook::HookTrait<ns::hook_name##_tag> {                                                                      \
        using FuncPtr = void (*)(__VA_ARGS__);                                                                         \
        using RetType = void;                                                                                          \
        using Callback = std::function<void(__VA_ARGS__)>;                                                             \
        using PreCallback = std::function<void(__VA_ARGS__)>;                                                          \
        using PostCallback = std::function<void(__VA_ARGS__)>;                                                         \
        static constexpr hook::HookType type = hook::HookType::Custom;                                                 \
        static constexpr const char *name = #hook_name;                                                                \
        static constexpr auto arg_names = VA_ARGS_TO_STRING_TUPLE(__VA_ARGS__);                                        \
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
