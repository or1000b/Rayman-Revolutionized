// The graphics thread: implementation, included by ps2_memory.cpp only (see ps2_gfx_thread.h).
#include "ps2_gfx_thread.h"
#include "ThreadNaming.h"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

extern "C" bool ps2xDeterministic(); // ps2_test_harness.cpp

namespace ps2x::gfx
{
    namespace
    {
        struct Task
        {
            std::function<void()> run;
            size_t bytes = 0;
            uint64_t display[5]{};
        };
        constexpr size_t kMaxQueuedBytes = 4u << 20;

        std::mutex g_mutex;
        std::condition_variable g_workCv; // the worker waits for a task
        std::condition_variable g_doneCv; // the game thread waits for room or for everything to be done
        std::deque<Task> g_queue;
        size_t g_bytes = 0;
        uint64_t g_posted = 0, g_done = 0;
        uint64_t g_shadow[5]{};
        bool g_stop = false, g_started = false;
        std::atomic<int> g_state{-1}; // -1 undecided, 0 off, 1 on
        std::thread g_thread;
        thread_local bool t_onWorker = false;
        // Counters for the [gfx] line at the end: how often the game had to wait for the worker.
        std::atomic<uint64_t> g_syncWaits{0}, g_roomWaits{0};

        void workerLoop()
        {
            ThreadNaming::SetCurrentThreadName("GfxThread");
            t_onWorker = true;
            for (;;)
            {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_workCv.wait(lock, [] { return g_stop || !g_queue.empty(); });
                    if (g_queue.empty())
                        return;
                    task = std::move(g_queue.front());
                    g_queue.pop_front();
                    std::memcpy(g_shadow, task.display, sizeof(g_shadow));
                }
                try
                {
                    task.run();
                }
                catch (const std::exception &e)
                {
                    std::fprintf(stderr, "[gfx] task failed: %s\n", e.what());
                }
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    ++g_done;
                    g_bytes -= task.bytes;
                }
                g_doneCv.notify_all();
            }
        }
    }

    bool enabled()
    {
        int state = g_state.load(std::memory_order_relaxed);
        if (state < 0)
        {
            const char *env = std::getenv("PS2X_GFX_THREAD");
            state = (env && env[0] == '0') ? 0 : (env && env[0] == '1') ? 1 : (ps2xDeterministic() ? 0 : 1);
            int expected = -1;
            if (!g_state.compare_exchange_strong(expected, state))
                state = expected;
            else
                std::fprintf(stderr, "[gfx] graphics thread %s\n", state ? "on" : "off");
        }
        return state == 1;
    }

    void forceOff()
    {
        g_state.store(0);
    }

    bool onWorker()
    {
        return t_onWorker;
    }

    bool shouldPost()
    {
        return !t_onWorker && enabled();
    }

    bool shouldPost(unsigned kind)
    {
        static const unsigned mask = []
        {
            const char *env = std::getenv("PS2X_GFX_MASK");
            return env ? static_cast<unsigned>(std::atoi(env)) : 7u;
        }();
        return (mask & kind) != 0u && shouldPost();
    }

    void post(std::function<void()> task, size_t bytes, const GSRegisters *priv)
    {
        Task t;
        t.run = std::move(task);
        t.bytes = bytes;
        if (priv)
        {
            t.display[0] = priv->pmode;
            t.display[1] = priv->dispfb1;
            t.display[2] = priv->display1;
            t.display[3] = priv->dispfb2;
            t.display[4] = priv->display2;
        }
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_started)
        {
            g_started = true;
            g_thread = std::thread(workerLoop);
        }
        if (g_bytes >= kMaxQueuedBytes)
        {
            g_roomWaits.fetch_add(1, std::memory_order_relaxed);
            g_doneCv.wait(lock, [] { return g_bytes < kMaxQueuedBytes || g_stop; });
        }
        g_bytes += bytes;
        ++g_posted;
        g_queue.push_back(std::move(t));
        lock.unlock();
        g_workCv.notify_one();
    }

    void sync()
    {
        if (t_onWorker || g_state.load(std::memory_order_relaxed) != 1)
            return;
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_started || g_done == g_posted)
            return;
        g_syncWaits.fetch_add(1, std::memory_order_relaxed);
        g_doneCv.wait(lock, [] { return g_done == g_posted || g_stop; });
    }

    bool shadowDisplay(uint64_t out[5])
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_started || g_done == g_posted)
            return false;
        std::memcpy(out, g_shadow, sizeof(g_shadow));
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_started || g_stop)
                return;
            g_stop = true;
        }
        g_workCv.notify_all();
        g_doneCv.notify_all();
        if (g_thread.joinable())
            g_thread.join();
        std::fprintf(stderr, "[gfx] posts %llu, the game waited for the worker: %llu syncs, %llu full queue\n",
                     static_cast<unsigned long long>(g_posted), static_cast<unsigned long long>(g_syncWaits.load()),
                     static_cast<unsigned long long>(g_roomWaits.load()));
    }
}
