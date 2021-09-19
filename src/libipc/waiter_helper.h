#pragma once

#include <atomic>
#include <limits>
#include <utility>

#include "libipc/def.h"
#include "libipc/utility/scope_guard.h"

namespace ipc {
namespace detail {

struct waiter_helper {

    struct wait_counter {
        std::atomic<long> waiting_ { 0 };
        long counter_ = 0;
    };

    struct wait_flags {
        std::atomic<bool> is_waiting_ { false };
        std::atomic<bool> is_closed_  { true  };
        std::atomic<bool> need_dest_  { false };
    };

    template <typename Mutex, typename Ctrl, typename F>
    static bool wait_if(Ctrl & ctrl, Mutex & mtx, F && pred, std::uint64_t tm) {
        auto & flags = ctrl.flags();
        if (flags.is_closed_.load(std::memory_order_acquire)) {
            return false;
        }

        auto & counter = ctrl.counter();
        counter.waiting_.fetch_add(1, std::memory_order_release);
        flags.is_waiting_.store(true, std::memory_order_relaxed);
        auto finally = ipc::guard([&ctrl, &counter, &flags] {
            for (auto curr_wait = counter.waiting_.load(std::memory_order_relaxed); curr_wait > 0;) {
                if (counter.waiting_.compare_exchange_weak(curr_wait, curr_wait - 1, std::memory_order_acq_rel)) {
                    break;
                }
            }
            flags.is_waiting_.store(false, std::memory_order_relaxed);
        });
        {
            IPC_UNUSED_ auto guard = ctrl.get_lock();
            if (!std::forward<F>(pred)()) return true;
            counter.counter_ = counter.waiting_.load(std::memory_order_relaxed);
        }
        mtx.unlock();

        bool ret = false;
        do {
            bool is_waiting = flags.is_waiting_.load(std::memory_order_relaxed);
            bool is_closed  = flags.is_closed_ .load(std::memory_order_acquire);
            if (!is_waiting || is_closed) {
                flags.need_dest_.store(false, std::memory_order_release);
                ret = false;
                break;
            }
            else if (flags.need_dest_.exchange(false, std::memory_order_release)) {
                ret = false;
                ctrl.sema_wait(default_timeout);
                break;
            }
            else {
                ret = ctrl.sema_wait(tm);
            }
        } while (flags.need_dest_.load(std::memory_order_acquire));
        finally.do_exit();
        ret = ctrl.handshake_post(1) && ret;

        mtx.lock();
        return ret;
    }

    template <typename Ctrl>
    static void clear_handshake(Ctrl & ctrl) {
        while (ctrl.handshake_wait(0)) ;
    }

    template <typename Ctrl>
    static bool notify(Ctrl & ctrl) {
        auto & counter = ctrl.counter();
        if ((counter.waiting_.load(std::memory_order_acquire)) == 0) {
            return true;
        }
        bool ret = true;
        IPC_UNUSED_ auto guard = ctrl.get_lock();
        clear_handshake(ctrl);
        if (counter.counter_ > 0) {
            ret = ctrl.sema_post(1);
            counter.counter_ -= 1;
            ret = ret && ctrl.handshake_wait(default_timeout);
        }
        return ret;
    }

    template <typename Ctrl>
    static bool broadcast(Ctrl & ctrl) {
        auto & counter = ctrl.counter();
        if ((counter.waiting_.load(std::memory_order_acquire)) == 0) {
            return true;
        }
        bool ret = true;
        IPC_UNUSED_ auto guard = ctrl.get_lock();
        clear_handshake(ctrl);
        if (counter.counter_ > 0) {
            ret = ctrl.sema_post(counter.counter_);
            auto tm = default_timeout / counter.counter_;
            do {
                counter.counter_ -= 1;
                ret = ret && ctrl.handshake_wait(tm);
            } while (counter.counter_ > 0);
            counter.waiting_.store(0, std::memory_order_release);
        }
        return ret;
    }

    template <typename Ctrl>
    static bool quit_waiting(Ctrl & ctrl) {
        auto & flags = ctrl.flags();
        flags.need_dest_.store(true, std::memory_order_relaxed);
        if (!flags.is_waiting_.exchange(false, std::memory_order_release)) {
            return true;
        }
        auto & counter = ctrl.counter();
        if ((counter.waiting_.load(std::memory_order_acquire)) == 0) {
            return true;
        }
        bool ret = true;
        IPC_UNUSED_ auto guard = ctrl.get_lock();
        clear_handshake(ctrl);
        if (counter.counter_ > 0) {
            ret = ctrl.sema_post(counter.counter_);
            counter.counter_ -= 1;
            ret = ret && ctrl.handshake_wait(default_timeout);
        }
        return ret;
    }
};

} // namespace detail
} // namespace ipc
