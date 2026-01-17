#pragma once

#include <cstddef>
#include <cstring>

#include <concepts>
#include <type_traits>

namespace FileIO {
    // Convenience for serializing random types
    template<typename StructT>
    struct SerializableTrait {
        static_assert(sizeof(StructT) == 0, "SerializableTrait must be specialized for this type. "
                                            "Use DEFINE_SERIALIZABLE_TRAIT(Struct");
    };
}; // namespace FileIO

// TODO:
#define DEFINE_SERIALIZABLE_TRAIT(Struct)                                                                              \
    template<>                                                                                                         \
    struct FileIO::SerializableTrait<Struct> {                                                                         \
        /* TODO: Returning bool for Protobuf compatibility. memcpy return is unused for now */                         \
        static auto GetSize(auto&&) {                                                                                  \
            return sizeof(Struct);                                                                                     \
        };                                                                                                             \
        static bool SerializeTo(const Struct& s, void* data) {                                                         \
            std::memcpy(data, &s, sizeof(Struct));                                                                     \
            return true;                                                                                               \
        };                                                                                                             \
        using type = Struct;                                                                                           \
    }

namespace FileIO {
    // TODO: repeated with events. Move to a central location...
    template<typename T>
    using BareType_t = std::remove_cvref_t<T>;

    template<typename T>
    concept Serializable = requires(BareType_t<T>& t) {
        { SerializableTrait<BareType_t<T>>::GetSize(t) } -> std::convertible_to<std::size_t>;
        // TODO: made stream writing easier
        // { SerializableTrait<BareType_t<T>>::SerializeTo(t, std::declval<void *>()) } -> std::same_as<bool>;
        { std::is_trivially_copyable_v<BareType_t<T>> };
    };
} // namespace FileIO
