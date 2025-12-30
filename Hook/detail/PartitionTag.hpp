#pragma once

#include <type_traits>

#include "HookTrait.hpp"

// ================================================================
// Helper to partition tags by hook type
// TODO: uusedunused for now
// ================================================================
namespace hook::detail {
    template<typename... Tags>
    struct TagList {};

    template<typename Tag, typename PreloadTags, typename ObjectOpsTags>
    struct PartitionTag;

    template<typename Tag, typename... PreloadTags, typename... ObjectOpsTags>
    struct PartitionTag<Tag, TagList<PreloadTags...>, TagList<ObjectOpsTags...>> {
        using Preload =
                std::conditional_t<is_preload_hook_v<Tag>, TagList<PreloadTags..., Tag>, TagList<PreloadTags...>>;
        using ObjectOps = std::conditional_t<is_object_ops_hook_v<Tag>, TagList<ObjectOpsTags..., Tag>,
                                             TagList<ObjectOpsTags...>>;
    };

    template<typename PreloadTags, typename ObjectOpsTags, typename... Tags>
    struct PartitionTagsImpl;

    template<typename PreloadTags, typename ObjectOpsTags>
    struct PartitionTagsImpl<PreloadTags, ObjectOpsTags> {
        using Preload = PreloadTags;
        using ObjectOps = ObjectOpsTags;
    };

    template<typename PreloadTags, typename ObjectOpsTags, typename Tag, typename... Rest>
    struct PartitionTagsImpl<PreloadTags, ObjectOpsTags, Tag, Rest...> {
        using Step = PartitionTag<Tag, PreloadTags, ObjectOpsTags>;
        using Result = PartitionTagsImpl<typename Step::Preload, typename Step::ObjectOps, Rest...>;
        using Preload = typename Result::Preload;
        using ObjectOps = typename Result::ObjectOps;
    };

    template<typename... Tags>
    using PartitionTags = PartitionTagsImpl<TagList<>, TagList<>, Tags...>;

} // namespace hook::detail
