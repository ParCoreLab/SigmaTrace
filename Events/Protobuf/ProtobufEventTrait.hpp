#pragma once

#define DEFINE_PROTOBUF_EVENT_TRAIT(EventStruct, EventId, EventName)                                                   \
    template<>                                                                                                         \
    struct Events::EventTrait<EventStruct> {                                                                           \
        static constexpr Events::EventTypeId id = EventId;                                                             \
        static constexpr const char *name = EventName;                                                                 \
        static auto GetSize(const EventStruct &s) { return s.ByteSizeLong(); };                                        \
        static bool SerializeTo(const EventStruct &s, void *data) { return s.SerializeToArray(data, GetSize(s)); };    \
        static constexpr size_t alignment = alignof(EventStruct);                                                      \
        using type = EventStruct;                                                                                      \
    }
