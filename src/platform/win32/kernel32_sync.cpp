// kernel32: critical sections, events, mutexes, semaphores, waits, threads, Interlocked*, sleeping,
// user APCs (T-045).
#include <sched.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

#include "win32_util.h"

namespace bo1 {

// ---- Interlocked* (exported by kernel32 on x86; 109M InterlockedExchange calls in a 90 s baseline run) ----
static uint32_t *a32(uint32_t a) { return reinterpret_cast<uint32_t *>(g_mem + a); }
SHIM(kernel32, InterlockedExchange, 2) { return __atomic_exchange_n(a32(a[0]), a[1], __ATOMIC_SEQ_CST); }
SHIM(kernel32, InterlockedIncrement, 1) { return __atomic_add_fetch(a32(a[0]), 1, __ATOMIC_SEQ_CST); }
SHIM(kernel32, InterlockedDecrement, 1) { return __atomic_sub_fetch(a32(a[0]), 1, __ATOMIC_SEQ_CST); }
SHIM(kernel32, InterlockedCompareExchange, 3) {
    uint32_t expected = a[2];
    __atomic_compare_exchange_n(a32(a[0]), &expected, a[1], 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

// ---- critical sections --------------------------------------------------------------------------------
// The guest CRITICAL_SECTION (24 bytes) is used directly: LockCount (+4) is -1 when free, 0 when held;
// RecursionCount (+8), OwningThread (+12). Waiters spin briefly, then yield, then sleep (no kernel wait
// object: the baseline shows ~20M uncontended enter/leave pairs per run, contention is the rare case).
enum { CS_LOCK = 4, CS_RECURSION = 8, CS_OWNER = 12, CS_SPIN = 20 };
static void cs_init(uint32_t cs, uint32_t spin) {
    memset(g_mem + cs, 0, 24);
    wr32(cs + CS_LOCK, 0xFFFFFFFFu);
    wr32(cs + CS_SPIN, spin);
}
static bool cs_try(Cpu *cpu, uint32_t cs) {
    uint32_t tid = rd32(cpu->teb + TEB_TID);
    if (rd32(cs + CS_OWNER) == tid) {
        wr32(cs + CS_RECURSION, rd32(cs + CS_RECURSION) + 1);
        return true;
    }
    uint32_t expected = 0xFFFFFFFFu;
    if (!__atomic_compare_exchange_n(a32(cs + CS_LOCK), &expected, 0, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return false;
    wr32(cs + CS_OWNER, tid);
    wr32(cs + CS_RECURSION, 1);
    return true;
}
SHIM(kernel32, InitializeCriticalSection, 1) {
    cs_init(a[0], 0);
    return 0;
}
SHIM(kernel32, InitializeCriticalSectionAndSpinCount, 2) {
    cs_init(a[0], a[1]);
    return 1;
}
SHIM(kernel32, DeleteCriticalSection, 1) { return 0; }
SHIM(kernel32, TryEnterCriticalSection, 1) { return cs_try(cpu, a[0]) ? 1 : 0; }
SHIM(kernel32, EnterCriticalSection, 1) {
    for (unsigned spins = 0; !cs_try(cpu, a[0]); spins++) {
        if (spins < 200) continue;
        if (spins < 2000) sched_yield();
        else usleep(100);
    }
    return 0;
}
SHIM(kernel32, LeaveCriticalSection, 1) {
    uint32_t r = rd32(a[0] + CS_RECURSION) - 1;
    wr32(a[0] + CS_RECURSION, r);
    if (r == 0) {
        wr32(a[0] + CS_OWNER, 0);
        __atomic_store_n(a32(a[0] + CS_LOCK), 0xFFFFFFFFu, __ATOMIC_RELEASE);
    }
    return 0;
}

// ---- waitable objects -------------------------------------------------------------------------------------
struct Waitable : HObject {
    std::mutex mu;
    std::condition_variable cv;
    // Returns true when acquired; called with mu held.
    virtual bool try_acquire(uint32_t tid) = 0;
};
struct EventObject : Waitable {
    bool manual, signaled;
    uint64_t pulse = 0; // generation counter for PulseEvent
    EventObject(bool m, bool s) : manual(m), signaled(s) {}
    const char *kind() const override { return "event"; }
    bool try_acquire(uint32_t) override {
        if (!signaled) return false;
        if (!manual) signaled = false;
        return true;
    }
};
struct MutexObject : Waitable {
    uint32_t owner = 0, count = 0;
    const char *kind() const override { return "mutex"; }
    bool try_acquire(uint32_t tid) override {
        if (count && owner != tid) return false;
        owner = tid;
        count++;
        return true;
    }
};
struct SemaphoreObject : Waitable {
    int32_t count, max;
    SemaphoreObject(int32_t c, int32_t m) : count(c), max(m) {}
    const char *kind() const override { return "semaphore"; }
    bool try_acquire(uint32_t) override {
        if (count <= 0) return false;
        count--;
        return true;
    }
};

static std::mutex g_named_mu;
static std::map<std::string, std::shared_ptr<HObject>> g_named;
// Named objects: a second Create* with the same name returns the existing object and ERROR_ALREADY_EXISTS.
template <class T, class Make> static uint32_t create_named(Cpu *cpu, uint32_t name_va, Make make) {
    last_error(cpu) = 0;
    if (!name_va) return handle_new(make());
    std::string name = gstr(name_va);
    std::lock_guard<std::mutex> lk(g_named_mu);
    auto it = g_named.find(name);
    if (it != g_named.end()) {
        if (!std::dynamic_pointer_cast<T>(it->second)) {
            last_error(cpu) = ERROR_INVALID_HANDLE;
            return 0;
        }
        last_error(cpu) = ERROR_ALREADY_EXISTS;
        return handle_new(it->second);
    }
    std::shared_ptr<HObject> o = make();
    g_named[name] = o;
    return handle_new(o);
}

SHIM(kernel32, CreateEventA, 4) {
    return create_named<EventObject>(cpu, a[3], [&] { return std::make_shared<EventObject>(a[1] != 0, a[2] != 0); });
}
SHIM(kernel32, SetEvent, 1) {
    auto e = handle_as<EventObject>(a[0]);
    if (!e) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0;
    }
    {
        std::lock_guard<std::mutex> lk(e->mu);
        e->signaled = true;
    }
    e->cv.notify_all();
    return 1;
}
SHIM(kernel32, ResetEvent, 1) {
    auto e = handle_as<EventObject>(a[0]);
    if (!e) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0;
    }
    std::lock_guard<std::mutex> lk(e->mu);
    e->signaled = false;
    return 1;
}
SHIM(kernel32, PulseEvent, 1) {
    // Releases the threads waiting right now and leaves the event non-signaled.
    auto e = handle_as<EventObject>(a[0]);
    if (!e) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0;
    }
    {
        std::lock_guard<std::mutex> lk(e->mu);
        e->pulse++;
        e->signaled = false;
    }
    if (e->manual) e->cv.notify_all();
    else e->cv.notify_one();
    return 1;
}
SHIM(kernel32, CreateMutexA, 3) {
    uint32_t tid = rd32(cpu->teb + TEB_TID);
    uint32_t h = create_named<MutexObject>(cpu, a[2], [&] {
        auto m = std::make_shared<MutexObject>();
        if (a[1]) m->owner = tid, m->count = 1;
        return m;
    });
    return h;
}
SHIM(kernel32, ReleaseMutex, 1) {
    auto m = handle_as<MutexObject>(a[0]);
    if (!m) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0;
    }
    {
        std::lock_guard<std::mutex> lk(m->mu);
        if (!m->count || m->owner != rd32(cpu->teb + TEB_TID)) {
            last_error(cpu) = 288; // ERROR_NOT_OWNER
            return 0;
        }
        if (--m->count == 0) m->owner = 0;
    }
    m->cv.notify_one();
    return 1;
}
SHIM(kernel32, CreateSemaphoreA, 4) {
    return create_named<SemaphoreObject>(cpu, a[3], [&] { return std::make_shared<SemaphoreObject>((int32_t)a[1], (int32_t)a[2]); });
}
SHIM(kernel32, ReleaseSemaphore, 3) {
    auto s = handle_as<SemaphoreObject>(a[0]);
    if (!s) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0;
    }
    {
        std::lock_guard<std::mutex> lk(s->mu);
        if (s->count + (int32_t)a[1] > s->max) {
            last_error(cpu) = 298; // ERROR_TOO_MANY_POSTS
            return 0;
        }
        if (a[2]) wr32(a[2], (uint32_t)s->count);
        s->count += (int32_t)a[1];
    }
    s->cv.notify_all();
    return 1;
}

// ---- user APCs (ReadFileEx completion routines run from alertable waits) ------------------------------
struct Apc {
    uint32_t routine, a0, a1, a2;
};
static thread_local std::deque<Apc> t_apcs;
void queue_apc(uint32_t routine, uint32_t a0, uint32_t a1, uint32_t a2) { t_apcs.push_back({routine, a0, a1, a2}); }
bool run_apcs(Cpu *cpu) {
    bool any = false;
    while (!t_apcs.empty()) {
        Apc apc = t_apcs.front();
        t_apcs.pop_front();
        guest_call(cpu, apc.routine, {apc.a0, apc.a1, apc.a2});
        any = true;
    }
    return any;
}

uint32_t wait_one(Cpu *cpu, uint32_t handle, uint32_t ms, bool alertable) {
    if (alertable && run_apcs(cpu)) return WAIT_IO_COMPLETION;
    std::shared_ptr<HObject> obj = handle_get(handle);
    if (!obj) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return WAIT_FAILED;
    }
    bool is_thread = false;
    bool done = thread_wait(obj.get(), ms, &is_thread);
    if (is_thread) return done ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    auto w = std::dynamic_pointer_cast<Waitable>(obj);
    if (!w) {
        log("WaitForSingleObject on a %s handle: treated as signaled", obj->kind());
        return WAIT_OBJECT_0;
    }
    uint32_t tid = rd32(cpu->teb + TEB_TID);
    std::unique_lock<std::mutex> lk(w->mu);
    auto *ev = dynamic_cast<EventObject *>(w.get());
    uint64_t pulse0 = ev ? ev->pulse : 0;
    auto ready = [&] { return w->try_acquire(tid) || (ev && ev->pulse != pulse0); };
    if (ms == INFINITE_MS) {
        w->cv.wait(lk, ready);
        return WAIT_OBJECT_0;
    }
    if (ms == 0) return ready() ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    return w->cv.wait_for(lk, std::chrono::milliseconds(ms), ready) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
SHIM(kernel32, WaitForSingleObject, 2) { return wait_one(cpu, a[0], a[1], false); }

// ---- sleeping ----------------------------------------------------------------------------------------------
SHIM(kernel32, Sleep, 1) {
    if (a[0] == 0) sched_yield();
    else usleep(a[0] == INFINITE_MS ? 0x7FFFFFFF : a[0] * 1000u);
    return 0;
}
SHIM(kernel32, SleepEx, 2) {
    if (a[1] && run_apcs(cpu)) return WAIT_IO_COMPLETION;
    if (a[0] == 0) sched_yield();
    else usleep(a[0] == INFINITE_MS ? 0x7FFFFFFF : a[0] * 1000u);
    return 0;
}
SHIM(kernel32, SwitchToThread, 0) {
    sched_yield();
    return 1;
}

// ---- threads -------------------------------------------------------------------------------------------------
SHIM(kernel32, CreateThread, 6) {
    // (lpAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId)
    uint32_t tid = 0;
    uint32_t h = thread_create(a[2], a[3], a[1], (a[4] & 4) != 0 /* CREATE_SUSPENDED */, &tid);
    if (a[5]) wr32(a[5], tid);
    if (!h) last_error(cpu) = ERROR_NOT_ENOUGH_MEMORY;
    if (g_verbose) log("CreateThread(start %08x, param %08x, stack %x%s) -> handle %x tid %x", a[2], a[3], a[1], (a[4] & 4) ? ", suspended" : "", h, tid);
    return h;
}
SHIM(kernel32, ResumeThread, 1) {
    auto o = handle_get(a[0]);
    if (!o || !thread_resume(o.get())) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0xFFFFFFFFu;
    }
    return 1; // previous suspend count
}
SHIM(kernel32, ExitThread, 1) { thread_exit_current(a[0]); }
SHIM(kernel32, GetExitCodeThread, 2) {
    auto o = handle_get(a[0]);
    uint32_t code;
    if (!o || !thread_exit_code(o.get(), &code)) {
        last_error(cpu) = ERROR_INVALID_HANDLE;
        return 0;
    }
    wr32(a[1], code);
    return 1;
}
SHIM(kernel32, SetThreadPriority, 2) { return 1; }
SHIM(kernel32, GetThreadPriority, 1) { return 0; } // THREAD_PRIORITY_NORMAL

} // namespace bo1
