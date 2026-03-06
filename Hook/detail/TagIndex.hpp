#pragma once

#include <cstddef>
#include <type_traits>

namespace hook::detail {

    // ================================================================
    // Compile-time tag-to-index mapping for index-based tuple access.
    // We unfortunately can't always use std::get<T> because it fails
    // when multiple tags share the same underlying type (function sig)
    // ================================================================
    template<typename Target, typename... Ts>
    struct TagIndex;

    // Match found: index is 0
    template<typename Target, typename... Rest>
    struct TagIndex<Target, Target, Rest...> {
        static constexpr size_t value = 0;
    };

    // No match yet: recurse
    template<typename Target, typename First, typename... Rest>
    struct TagIndex<Target, First, Rest...> {
        static constexpr size_t value = 1 + TagIndex<Target, Rest...>::value;
    };

    // Empty list: tag not found
    template<typename Target>
    struct TagIndex<Target> {
        static_assert(sizeof(Target) == 0, "Tag not found in tag list");
    };

} // namespace hook::detail
