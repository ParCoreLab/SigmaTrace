Sigma_add_library(HookLib INTERFACE)

target_sources(HookLib INTERFACE
        CallbackManager.hpp
        CompositeHookManager.hpp
        HookTrait.hpp
        detail/ObjectOpsHookManager.hpp
        detail/PreloadHookManager.hpp
)

target_include_directories(HookLib INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(HookLib INTERFACE SigmaHelpers)
