#pragma once

#include <concepts>
#include <ranges>

namespace Helpers {
    template<typename R, typename T>
    concept RangeOf = std::ranges::input_range<R> && std::convertible_to<std::ranges::range_value_t<R>, T>;
}
