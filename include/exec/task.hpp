/*
 * Copyright (c) 2021-2024 NVIDIA Corporation
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <any>
#include <cassert>
#include <exception>
#include <utility>

#include "../stdexec/execution.hpp"
#include "../stdexec/__detail/__meta.hpp"
#include "../stdexec/__detail/__optional.hpp"
#include "../stdexec/__detail/__variant.hpp"

#include "any_sender_of.hpp"
#include "at_coroutine_exit.hpp"
#include "inline_scheduler.hpp"
#include "scope.hpp"

STDEXEC_PRAGMA_PUSH()
STDEXEC_PRAGMA_IGNORE_GNU("-Wundefined-inline")

namespace exec {
  namespace __task {
    using namespace stdexec;

    // The required set_value_t() scheduler-sender completion signature is added in
    // any_receiver_ref::any_sender::any_scheduler.
    using __any_scheduler_completions =
      completion_signatures<set_error_t(std::exception_ptr), set_stopped_t()>;

    using __any_scheduler =
      any_receiver_ref<__any_scheduler_completions>::any_sender<>::any_scheduler<>;

    static_assert(scheduler<__any_scheduler>);

    template <class _Ty>
    concept __stop_token_provider = requires(const _Ty& t) { get_stop_token(t); };

    template <class _Ty>
    concept __indirect_stop_token_provider = requires(const _Ty& t) {
      { get_env(t) } -> __stop_token_provider;
    };

    template <class _Ty>
    concept __indirect_scheduler_provider = requires(const _Ty& t) {
      { get_env(t) } -> __scheduler_provider;
    };

    template <class _ParentPromise>
    constexpr auto __check_parent_promise_has_scheduler() noexcept -> bool {
      static_assert(
        __indirect_scheduler_provider<_ParentPromise>,
        "exec::task<T> cannot be co_await-ed in a coroutine that "
        "does not have an associated scheduler.");
      return __indirect_scheduler_provider<_ParentPromise>;
    }

    struct __forward_stop_request {
      inplace_stop_source& __stop_source_;

      void operator()() noexcept {
        __stop_source_.request_stop();
      }
    };

    template <class _ParentPromise>
    struct __default_awaiter_context;

    ////////////////////////////////////////////////////////////////////////////////
    // This is the context that is associated with basic_task's promise type
    // by default. It handles forwarding of stop requests from parent to child.
    enum class __scheduler_affinity {
      __none,
      __sticky
    };

    struct __parent_promise_t { };

    template <__scheduler_affinity _SchedulerAffinity = __scheduler_affinity::__sticky>
    class __default_task_context_impl {
      template <class _ParentPromise>
      friend struct __default_awaiter_context;

      static constexpr bool __with_scheduler = _SchedulerAffinity == __scheduler_affinity::__sticky;

      STDEXEC_ATTRIBUTE(no_unique_address)
      __if_c<__with_scheduler, __any_scheduler, __ignore> __scheduler_{exec::inline_scheduler{}};
      inplace_stop_token __stop_token_;

     public:
      template <class _ParentPromise>
      explicit __default_task_context_impl(__parent_promise_t, _ParentPromise& __parent) noexcept {
        if constexpr (_SchedulerAffinity == __scheduler_affinity::__sticky) {
          if constexpr (__check_parent_promise_has_scheduler<_ParentPromise>()) {
            __scheduler_ = get_scheduler(get_env(__parent));
          }
        }
      }

      template <scheduler _Scheduler>
      explicit __default_task_context_impl(_Scheduler&& __scheduler)
        : __scheduler_{static_cast<_Scheduler&&>(__scheduler)} {
      }

      [[nodiscard]]
      auto query(get_scheduler_t) const noexcept -> const __any_scheduler&
        requires(__with_scheduler)
      {
        return __scheduler_;
      }

      [[nodiscard]]
      auto query(get_stop_token_t) const noexcept -> inplace_stop_token {
        return __stop_token_;
      }

      [[nodiscard]]
      auto stop_requested() const noexcept -> bool {
        return __stop_token_.stop_requested();
      }

      template <scheduler _Scheduler>
      void set_scheduler(_Scheduler&& __sched)
        requires(__with_scheduler)
      {
        __scheduler_ = static_cast<_Scheduler&&>(__sched);
      }

      template <class _ThisPromise>
      using promise_context_t = __default_task_context_impl;

      template <class _ThisPromise, class _ParentPromise = void>
        requires(!__with_scheduler) || __indirect_scheduler_provider<_ParentPromise>
      using awaiter_context_t = __default_awaiter_context<_ParentPromise>;
    };

    template <class _Ty>
    using default_task_context = __default_task_context_impl<__scheduler_affinity::__sticky>;

    template <class _Ty>
    using __raw_task_context = __default_task_context_impl<__scheduler_affinity::__none>;

    // This is the context associated with basic_task's awaiter. By default
    // it does nothing.
    template <class _ParentPromise>
    struct __default_awaiter_context {
      template <__scheduler_affinity _Affinity>
      explicit __default_awaiter_context(
        __default_task_context_impl<_Affinity>& __self,
        _ParentPromise& __parent) noexcept {
      }
    };

    ////////////////////////////////////////////////////////////////////////////////
    // This is the context to be associated with basic_task's awaiter when
    // the parent coroutine's promise type is known, is a __stop_token_provider,
    // and its stop token type is neither inplace_stop_token nor unstoppable.
    template <__indirect_stop_token_provider _ParentPromise>
    struct __default_awaiter_context<_ParentPromise> {
      using __stop_token_t = stop_token_of_t<env_of_t<_ParentPromise>>;
      using __stop_callback_t =
        typename __stop_token_t::template callback_type<__forward_stop_request>;

      template <__scheduler_affinity _Affinity>
      explicit __default_awaiter_context(
        __default_task_context_impl<_Affinity>& __self,
        _ParentPromise& __parent) noexcept
        // Register a callback that will request stop on this basic_task's
        // stop_source when stop is requested on the parent coroutine's stop
        // token.
        : __stop_callback_{
            get_stop_token(get_env(__parent)),
            __forward_stop_request{__stop_source_}} {
        static_assert(std::is_nothrow_constructible_v<
                      __stop_callback_t,
                      __stop_token_t,
                      __forward_stop_request>);
        __self.__stop_token_ = __stop_source_.get_token();
      }

      inplace_stop_source __stop_source_{};
      __stop_callback_t __stop_callback_;
    };

    // If the parent coroutine's type has a stop token of type inplace_stop_token,
    // we don't need to register a stop callback.
    template <__indirect_stop_token_provider _ParentPromise>
      requires std::same_as<inplace_stop_token, stop_token_of_t<env_of_t<_ParentPromise>>>
    struct __default_awaiter_context<_ParentPromise> {
      template <__scheduler_affinity _Affinity>
      explicit __default_awaiter_context(
        __default_task_context_impl<_Affinity>& __self,
        _ParentPromise& __parent) noexcept {
        __self.__stop_token_ = get_stop_token(get_env(__parent));
      }
    };

    // If the parent coroutine's stop token is unstoppable, there's no point
    // forwarding stop tokens or stop requests at all.
    template <__indirect_stop_token_provider _ParentPromise>
      requires unstoppable_token<stop_token_of_t<env_of_t<_ParentPromise>>>
    struct __default_awaiter_context<_ParentPromise> {
      template <__scheduler_affinity _Affinity>
      explicit __default_awaiter_context(
        __default_task_context_impl<_Affinity>&,
        _ParentPromise&) noexcept {
      }
    };

    // Finally, if we don't know the parent coroutine's promise type, assume the
    // worst and save a type-erased stop callback.
    template <>
    struct __default_awaiter_context<void> {
      template <__scheduler_affinity _Affinity, class _ParentPromise>
      explicit __default_awaiter_context(
        __default_task_context_impl<_Affinity>& __self,
        _ParentPromise& __parent) noexcept {
      }

      template <__scheduler_affinity _Affinity, __indirect_stop_token_provider _ParentPromise>
      explicit __default_awaiter_context(
        __default_task_context_impl<_Affinity>& __self,
        _ParentPromise& __parent) {
        // Register a callback that will request stop on this basic_task's
        // stop_source when stop is requested on the parent coroutine's stop
        // token.
        using __stop_token_t = stop_token_of_t<env_of_t<_ParentPromise>>;
        using __stop_callback_t = stop_callback_for_t<__stop_token_t, __forward_stop_request>;

        if constexpr (std::same_as<__stop_token_t, inplace_stop_token>) {
          __self.__stop_token_ = get_stop_token(get_env(__parent));
        } else if (auto __token = get_stop_token(get_env(__parent)); __token.stop_possible()) {
          __stop_callback_
            .emplace<__stop_callback_t>(std::move(__token), __forward_stop_request{__stop_source_});
          __self.__stop_token_ = __stop_source_.get_token();
        }
      }

      inplace_stop_source __stop_source_{};
      std::any __stop_callback_{};
    };

    template <class _Promise, class _ParentPromise = void>
    using awaiter_context_t =
      typename __decay_t<env_of_t<_Promise>>::template awaiter_context_t<_Promise, _ParentPromise>;

    ////////////////////////////////////////////////////////////////////////////////
    // In a base class so it can be specialized when _Ty is void:
    template <class _Ty>
    struct __promise_base {
      void return_value(_Ty value) {
        __data_.template emplace<0>(std::move(value));
      }

      __variant_for<_Ty, std::exception_ptr> __data_{};
    };

    template <>
    struct __promise_base<void> {
      struct __void { };

      void return_void() {
        __data_.template emplace<0>(__void{});
      }

      __variant_for<__void, std::exception_ptr> __data_{};
    };

    enum class disposition : unsigned {
      stopped,
      succeeded,
      failed,
    };

    struct __reschedule_coroutine_on {
      template <class _Scheduler>
      struct __wrap {
        _Scheduler __sched_;
      };

      template <scheduler _Scheduler>
      auto operator()(_Scheduler __sched) const noexcept -> __wrap<_Scheduler> {
        return {static_cast<_Scheduler&&>(__sched)};
      }
    };

    ////////////////////////////////////////////////////////////////////////////////
    // basic_task
    template <class _Ty, class _Context = default_task_context<_Ty>>
    class [[nodiscard]] basic_task {
      struct __promise;
     public:
      using __t = basic_task;
      using __id = basic_task;
      using promise_type = __promise;

      basic_task(basic_task&& __that) noexcept
        : __coro_(std::exchange(__that.__coro_, {})) {
      }

      ~basic_task() {
        if (__coro_)
          __coro_.destroy();
      }

     private:
      struct __final_awaitable {
        static constexpr auto await_ready() noexcept -> bool {
          return false;
        }

        static auto await_suspend(__coro::coroutine_handle<__promise> __h) noexcept
          -> __coro::coroutine_handle<> {
          return __h.promise().continuation().handle();
        }

        static void await_resume() noexcept {
        }
      };

      using __promise_context_t = typename _Context::template promise_context_t<__promise>;

      struct __promise
        : __promise_base<_Ty>
        , with_awaitable_senders<__promise> {
        using __t = __promise;
        using __id = __promise;

        auto get_return_object() noexcept -> basic_task {
          return basic_task(__coro::coroutine_handle<__promise>::from_promise(*this));
        }

        auto initial_suspend() noexcept -> __coro::suspend_always {
          return {};
        }

        auto final_suspend() noexcept -> __final_awaitable {
          return {};
        }

        [[nodiscard]]
        auto disposition() const noexcept -> __task::disposition {
          switch (this->__data_.index()) {
          case 0:
            return __task::disposition::succeeded;
          case 1:
            return __task::disposition::failed;
          default:
            return __task::disposition::stopped;
          }
        }

        void unhandled_exception() noexcept {
          this->__data_.template emplace<1>(std::current_exception());
        }

#ifndef __clang_analyzer__
        template <sender _Awaitable>
          requires __scheduler_provider<_Context>
        auto await_transform(_Awaitable&& __awaitable) noexcept -> decltype(auto) {
          // TODO: If we have a complete-where-it-starts query then we can optimize
          // this to avoid the reschedule
          return stdexec::as_awaitable(
            continues_on(static_cast<_Awaitable&&>(__awaitable), get_scheduler(*__context_)),
            *this);
        }

        template <class _Scheduler>
          requires __scheduler_provider<_Context>
        auto await_transform(__reschedule_coroutine_on::__wrap<_Scheduler> __box) noexcept
          -> decltype(auto) {
          if (!std::exchange(__rescheduled_, true)) {
            // Create a cleanup action that transitions back onto the current scheduler:
            auto __sched = get_scheduler(*__context_);
            auto __cleanup_task = at_coroutine_exit(schedule, std::move(__sched));
            // Insert the cleanup action into the head of the continuation chain by making
            // direct calls to the cleanup task's awaiter member functions. See type
            // _cleanup_task in at_coroutine_exit.hpp:
            __cleanup_task.await_suspend(__coro::coroutine_handle<__promise>::from_promise(*this));
            (void) __cleanup_task.await_resume();
          }
          __context_->set_scheduler(__box.__sched_);
          return stdexec::as_awaitable(schedule(__box.__sched_), *this);
        }
#endif

        template <class _Awaitable>
        auto await_transform(_Awaitable&& __awaitable) noexcept -> decltype(auto) {
          return with_awaitable_senders<__promise>::await_transform(
            static_cast<_Awaitable&&>(__awaitable));
        }

        auto get_env() const noexcept -> const __promise_context_t& {
          return *__context_;
        }

        __optional<__promise_context_t> __context_{};
        bool __rescheduled_{false};
      };

      template <class _ParentPromise = void>
      struct __task_awaitable {
        __coro::coroutine_handle<__promise> __coro_;
        __optional<awaiter_context_t<__promise, _ParentPromise>> __context_{};

        ~__task_awaitable() {
          if (__coro_)
            __coro_.destroy();
        }

        static constexpr auto await_ready() noexcept -> bool {
          return false;
        }

        template <class _ParentPromise2>
        auto await_suspend(__coro::coroutine_handle<_ParentPromise2> __parent) noexcept
          -> __coro::coroutine_handle<> {
          static_assert(__one_of<_ParentPromise, _ParentPromise2, void>);
          __coro_.promise().__context_.emplace(__parent_promise_t(), __parent.promise());
          __context_.emplace(*__coro_.promise().__context_, __parent.promise());
          __coro_.promise().set_continuation(__parent);
          if constexpr (requires { __coro_.promise().stop_requested() ? 0 : 1; }) {
            if (__coro_.promise().stop_requested())
              return __parent.promise().unhandled_stopped();
          }
          return __coro_;
        }

        auto await_resume() -> _Ty {
          __context_.reset();
          scope_guard __on_exit{[this]() noexcept {
            std::exchange(__coro_, {}).destroy();
          }};
          if (__coro_.promise().__data_.index() == 1)
            std::rethrow_exception(std::move(__coro_.promise().__data_.template get<1>()));
          if constexpr (!std::is_void_v<_Ty>)
            return std::move(__coro_.promise().__data_.template get<0>());
        }
      };

     public:
      // Make this task awaitable within a particular context:
      template <class _ParentPromise>
        requires constructible_from<
          awaiter_context_t<__promise, _ParentPromise>,
          __promise_context_t&,
          _ParentPromise&>
      auto as_awaitable(_ParentPromise&) && noexcept -> __task_awaitable<_ParentPromise> {
        return __task_awaitable<_ParentPromise>{std::exchange(__coro_, {})};
      }

      // Make this task generally awaitable:
      auto operator co_await() && noexcept -> __task_awaitable<>
        requires __mvalid<awaiter_context_t, __promise>
      {
        return __task_awaitable<>{std::exchange(__coro_, {})};
      }

      // From the list of types [_Ty], remove any types that are void, and send
      //   the resulting list to __qf<set_value_t>, which uses the list of types
      //   as arguments of a function type. In other words, set_value_t() if _Ty
      //   is void, and set_value_t(_Ty) otherwise.
      using __set_value_sig_t = __minvoke<__mremove<void, __qf<set_value_t>>, _Ty>;

      // Specify basic_task's completion signatures
      //   This is only necessary when basic_task is not generally awaitable
      //   owing to constraints imposed by its _Context parameter.
      using __task_traits_t =
        completion_signatures<__set_value_sig_t, set_error_t(std::exception_ptr), set_stopped_t()>;

      auto get_completion_signatures(__ignore = {}) const -> __task_traits_t {
        return {};
      }

      explicit basic_task(__coro::coroutine_handle<promise_type> __coro) noexcept
        : __coro_(__coro) {
      }

      __coro::coroutine_handle<promise_type> __coro_;
    };
  } // namespace __task

  using task_disposition = __task::disposition;

  template <class _Ty>
  using default_task_context = __task::default_task_context<_Ty>;

  template <class _Promise, class _ParentPromise = void>
  using awaiter_context_t = __task::awaiter_context_t<_Promise, _ParentPromise>;

  template <class _Ty, class _Context = default_task_context<_Ty>>
  using basic_task = __task::basic_task<_Ty, _Context>;

  template <class _Ty>
  using task = basic_task<_Ty, default_task_context<_Ty>>;

  inline constexpr __task::__reschedule_coroutine_on reschedule_coroutine_on{};
} // namespace exec

namespace stdexec {
  template <class _Ty, class _Context>
  inline constexpr bool enable_sender<exec::basic_task<_Ty, _Context>> = true;
} // namespace stdexec

STDEXEC_PRAGMA_POP()
