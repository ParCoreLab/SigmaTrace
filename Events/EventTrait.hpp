#pragma once

#include <concepts>
#include <type_traits>

#include "Events.hpp"

namespace Events {
    // Primary template - must be specialized for each event type
    template<typename EventT>
    struct EventTrait {
        static_assert(sizeof(EventT) == 0,
                      "EventTrait must be specialized for this event type. "
                      "Use DEFINE_EVENT_TRAIT(EventStruct, EventTypeId, \"name\") to define traits.");
    };

} // namespace Events

// Macro to simplify trait definition
// Must be used at global scope or in Events namespace
// Usage: DEFINE_EVENT_TRAIT(namespace::CustomEvent, Events::EventTypeId::CustomEventID, "custom_event")
#define DEFINE_EVENT_TRAIT(EventStruct, EventId, EventName)                                                            \
    template<>                                                                                                         \
    struct Events::EventTrait<EventStruct> {                                                                           \
        static constexpr Events::EventTypeId id = EventId;                                                             \
        static constexpr const char *name = EventName;                                                                 \
        static constexpr size_t size = sizeof(EventStruct);                                                            \
        static constexpr size_t alignment = alignof(EventStruct);                                                      \
        using type = EventStruct;                                                                                      \
    }

namespace Events {
    // Convenience for removing cv-qualifiers and references
    template<typename T>
    using BareType_t = std::remove_cvref_t<T>;

    // Making sure the event has an ID and is usable as a vaue struct
    // Uses bare_type_t to handle references and cv-qualified types
    template<typename T>
    concept HasEventTrait = requires {
        { EventTrait<BareType_t<T>>::id } -> std::convertible_to<EventTypeId>;
        { EventTrait<BareType_t<T>>::name } -> std::convertible_to<const char *>;
        { EventTrait<BareType_t<T>>::size } -> std::convertible_to<size_t>;
    };

    // Helpers for future
    template<HasEventTrait EventT>
    inline constexpr EventTypeId event_id_v = EventTrait<BareType_t<EventT>>::id;

    template<HasEventTrait EventT>
    inline constexpr const char *event_name_v = EventTrait<BareType_t<EventT>>::name;

    template<HasEventTrait EventT>
    inline constexpr auto event_size_v = EventTrait<BareType_t<EventT>>::size;

    template<HasEventTrait EventT>
    inline constexpr auto event_alignment_v = EventTrait<BareType_t<EventT>>::alignment;

} // namespace Events
