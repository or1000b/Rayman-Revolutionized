// The graphics thread: implementation, included by ps2_memory.cpp only (see ps2_gfx_thread.h).
#include "ps2_gfx_thread.h"
#include "ThreadNaming.h"
#include <chrono>
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
        // How far the game may run ahead of the worker, in bytes of queued transfers (PS2X_GFX_QUEUE_MB, default 16).
        // Busy scenes send ~4 MiB per frame: the old 4 MiB kept the two threads in lock-step (hub run at 120 FPS:
        // plain 54.6 -> 71.6 game FPS with 16). 16 MiB is ~4 frames, so at most ~33 ms more picture lag at 120 FPS,
        // and only while the worker is the slower one.
        const size_t kMaxQueuedBytes = []
        {
            const char *env = std::getenv("PS2X_GFX_QUEUE_MB");
            const int mb = env ? std::atoi(env) : 0;
            return static_cast<size_t>(mb > 0 ? mb : 16) << 20;
        }();

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
        uint64_t g_bytesPosted = 0, g_maxQueued = 0; // under g_mutex
        std::atomic<uint64_t> g_busyNs{0};           // time the worker spent on tasks (ps2xGfxStatsLine: busy %)

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
                const auto busyFrom = std::chrono::steady_clock::now();
                try
                {
                    task.run();
                }
                catch (const std::exception &e)
                {
                    std::fprintf(stderr, "[gfx] task failed: %s\n", e.what());
                }
                g_busyNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - busyFrom).count()),
                                   std::memory_order_relaxed);
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
        g_bytesPosted += bytes;
        g_maxQueued = std::max<uint64_t>(g_maxQueued, g_bytes);
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
        std::fprintf(stderr, "[gfx] posts %llu, the game waited for the worker: %llu syncs, %llu full queue; %llu MiB posted, "
                             "queue limit %llu MiB, most queued %llu KiB\n",
                     static_cast<unsigned long long>(g_posted), static_cast<unsigned long long>(g_syncWaits.load()),
                     static_cast<unsigned long long>(g_roomWaits.load()),
                     static_cast<unsigned long long>(g_bytesPosted >> 20),
                     static_cast<unsigned long long>(kMaxQueuedBytes >> 20),
                     static_cast<unsigned long long>(g_maxQueued >> 10));
    }
}

// For the [threads] line (ps2_runtime.cpp): the worker's share of real time since the last call (busy, 100% = it is the
// limit) and how often the game waited for room in its queue in that time.
extern "C" int ps2xGfxStatsLine(char *out, int size)
{
    using namespace ps2x::gfx;
    if (g_state.load() != 1 || !out || size <= 0)
        return 0;
    static auto since = std::chrono::steady_clock::now();
    static uint64_t roomWaitsBefore = 0;
    const auto now = std::chrono::steady_clock::now();
    const double windowNs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - since).count());
    const int busy = windowNs > 0.0 ? static_cast<int>(g_busyNs.exchange(0u) * 100.0 / windowNs + 0.5) : 0;
    const uint64_t roomWaits = g_roomWaits.load();
    since = now;
    const int n = std::snprintf(out, static_cast<size_t>(size), "gfx busy=%d%% full=%llu", busy,
                                static_cast<unsigned long long>(roomWaits - roomWaitsBefore));
    roomWaitsBefore = roomWaits;
    return n;
}
