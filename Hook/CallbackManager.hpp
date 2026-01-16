#pragma once

#include <mutex>
#include <tuple>
#include <variant>
#include <vector>

#include "HookTrait.hpp"

namespace hook {
    // ================================================================
    // Tagged wrappers to ensure unique types in the tuple
    // (multiple tags may have the same callback signature)
    // ================================================================
    template<typename Tag>
    struct TaggedPreCallbacks {
        std::vector<typename HookTrait<Tag>::PreCallback> list;
    };

    template<typename Tag>
    struct TaggedPostCallbacks {
        std::vector<typename HookTrait<Tag>::PostCallback> list;
    };

    // ================================================================
    // Callback storage for a set of tags
    // Stores pre and post callbacks for each tag type
    // ================================================================
    template<typename... Tags>
    class CallbackStorage {
        static_assert((ValidHookTag<Tags> && ...), "All tags must have valid HookTrait specializations");

    public:
        template<typename Tag>
        using PreCallbackList = std::vector<typename HookTrait<Tag>::PreCallback>;

        template<typename Tag>
        using PostCallbackList = std::vector<typename HookTrait<Tag>::PostCallback>;

        template<typename Tag>
        PreCallbackList<Tag> &pre_callbacks() {
            return std::get<TaggedPreCallbacks<Tag>>(m_pre_callbacks).list;
        }

        template<typename Tag>
        const PreCallbackList<Tag> &pre_callbacks() const {
            return std::get<TaggedPreCallbacks<Tag>>(m_pre_callbacks).list;
        }

        template<typename Tag>
        PostCallbackList<Tag> &post_callbacks() {
            return std::get<TaggedPostCallbacks<Tag>>(m_post_callbacks).list;
        }

        template<typename Tag>
        const PostCallbackList<Tag> &post_callbacks() const {
            return std::get<TaggedPostCallbacks<Tag>>(m_post_callbacks).list;
        }

        template<typename Tag>
        void clear() noexcept {
            std::get<TaggedPreCallbacks<Tag>>(m_pre_callbacks).list.clear();
            std::get<TaggedPostCallbacks<Tag>>(m_post_callbacks).list.clear();
        }

        void clear_all() noexcept { (clear<Tags>(), ...); }

    private:
        std::tuple<TaggedPreCallbacks<Tags>...> m_pre_callbacks;
        std::tuple<TaggedPostCallbacks<Tags>...> m_post_callbacks;
    };

    // ================================================================
    // CallbackManager - Generic CRTP base for hook managers
    //
    // Provides:
    //   - Callback registration (register_pre, register_post)
    //   - Callback invocation (invoke_pre_callbacks, invoke_post_callbacks)
    //   - Optional derived class hooks via SFINAE
    //
    // Derived classes can:
    //   - Optionally override pre(Tag, args...) and post(Tag, ret, args...)
    //   - Implement hook installation mechanism specific to their hook type
    // ================================================================

    template<typename Derived, typename... Tags>
    class CallbackManager {

    public:
        using Storage = CallbackStorage<Tags...>;

        // ================================================================
        // Callback registration
        // ================================================================

        template<ValidCallbackTag Tag>
        void register_pre(HookTrait<Tag>::PreCallback callback) noexcept {
            std::lock_guard lock(m_mutex);
            m_callbacks.template pre_callbacks<Tag>().push_back(std::move(callback));
        }

        template<ValidCallbackTag Tag>
        void register_post(HookTrait<Tag>::PostCallback callback) noexcept {
            std::lock_guard lock(m_mutex);
            m_callbacks.template post_callbacks<Tag>().push_back(std::move(callback));
        }

        template<ValidCallbackTag Tag>
        void clear_callbacks() noexcept {
            std::lock_guard lock(m_mutex);
            m_callbacks.template clear<Tag>();
        }

        void clear_all_callbacks() noexcept {
            std::lock_guard lock(m_mutex);
            m_callbacks.clear_all();
        }

    protected:
        CallbackManager() = default;

        // ================================================================
        // Detecting if Derived has pre/post overloads (not callbacks)
        // ================================================================
        template<typename Tag, typename... Args>
        static constexpr bool has_derived_pre_v =
                requires(Derived &d, Tag t, Args &&...args) { d.pre(t, std::forward<Args>(args)...); };

        template<typename Tag, typename Ret, typename... Args>
        static constexpr bool has_derived_post_v =
                requires(Derived &d, Tag t, Ret r, Args &&...args) { d.post(t, r, std::forward<Args>(args)...); };

        // ================================================================
        // Invoke registered callbacks
        // ================================================================
        template<ValidCallbackTag Tag, typename... Args>
        void invoke_pre_callbacks(Args &&...args) noexcept {
            // Note: no lock here
            // We assume no callbacks will be registered after the first is called
            for (auto &cb: m_callbacks.template pre_callbacks<Tag>()) {
                cb(args...);
            }
        }

        template<ValidCallbackTag Tag, typename Ret, typename... Args>
        void invoke_post_callbacks(Ret ret, Args &&...args) noexcept {
            for (auto &cb: m_callbacks.template post_callbacks<Tag>()) {
                cb(ret, args...);
            }
        }

        // ================================================================
        // Call derived class hooks if they exist
        // ================================================================
        template<ValidCallbackTag Tag, typename... Args>
        void call_derived_pre(Args &&...args) noexcept {
            if constexpr (has_derived_pre_v<Tag, Args &&...>) {
                static_cast<Derived &>(*this).pre(Tag{}, std::forward<Args>(args)...);
            }
        }

        template<ValidCallbackTag Tag, typename Ret, typename... Args>
        void call_derived_post(Ret ret, Args &&...args) noexcept {
            if constexpr (has_derived_post_v<Tag, Ret, Args &&...>) {
                static_cast<Derived &>(*this).post(Tag{}, ret, std::forward<Args>(args)...);
            }
        }

        // ================================================================
        // Invoke all pre/post hooks
        // derived pre -> callbacks pre -> ... -> derived post -> callbacks post
        // TODO: consider using threads here
        // ================================================================
        template<ValidCallbackTag Tag, typename... Args>
        void invoke_all_pre(Args &&...args) noexcept {
            call_derived_pre<Tag>(args...);
            invoke_pre_callbacks<Tag>(std::forward<Args>(args)...);
        }

        template<ValidCallbackTag Tag, typename Ret, typename... Args>
        void invoke_all_post(Ret ret, Args &&...args) noexcept {
            call_derived_post<Tag>(ret, args...);
            invoke_post_callbacks<Tag>(ret, std::forward<Args>(args)...);
        }

        mutable std::mutex m_mutex;
        Storage m_callbacks;
    };

} // namespace hook
