// Frame rate: replaces the vanilla 100 FPS busy-wait with a precise, configurable limiter,
// and keeps AI / NPC / physics actors at a safe simulation rate when the frame rate is high.
//
// Main loop (Launch, 0x1090BE60): DeltaTime is clamped to [MinFrameTime, MaxFrameTime]
// read from two globals (0.01 / 0.1). ULevel::Tick (0x10A791F0) then calls
// AActor::Tick(DeltaSeconds, TickType) through vtable slot 0xB0 for every dynamic actor.
//
// "Logic rate": AI controllers, NPC pawns and Havok props only tick on "logic frames" that
// happen at most LogicRate times per second, receiving the accumulated delta. The player,
// camera, HUD, effects etc. keep ticking every rendered frame. To that code the game simply
// looks like it runs at <= LogicRate FPS, which is the range the AI was designed for.
#include "pch.h"
#include "features.h"
#include "perf.h"
#include <timeapi.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace
{
    // ---- Limiter --------------------------------------------------------------------------
    float g_minFrameTime = 0.0f;   // replaces the game's 0.01 (its own spin-wait threshold)
    float g_maxFrameTime = 0.1f;   // replaces the game's 0.1 (delta clamp)

    LARGE_INTEGER g_freq{};
    int64_t g_next = 0;
    int64_t g_intervalTicks = 0;
    HANDLE g_timer = nullptr;
    HMONITOR g_monitor = nullptr;
    int g_lastLimit = -2;

    int MonitorRefresh()
    {
        HMONITOR mon = Game::hWnd ? MonitorFromWindow(Game::hWnd, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        g_monitor = mon;
        MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
        DEVMODEW dm{}; dm.dmSize = sizeof(dm);
        if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            return static_cast<int>(dm.dmDisplayFrequency);
        return 60;
    }

    void UpdateInterval()
    {
        int limit = g_settings.fpsLimit;
        const bool dxvkPaces = limit < 0 && D3D9Proxy::UsingDxvk();
        if (limit < 0)
            limit = dxvkPaces ? 0 : MonitorRefresh();   // DXVK's vsync paces frames itself
        if (limit != g_lastLimit)
        {
            LOG("Framerate: frame limit %s%d FPS%s", limit ? "" : "off (", limit,
                limit ? "" : dxvkPaces ? ", DXVK vsync paces frames)" : ")");
            g_lastLimit = limit;
        }
        const double interval = limit > 0 ? 1.0 / std::clamp(limit, 10, 1000) : 0.0;
        Perf::SetTarget(interval * 1000.0);
        if (g_settings.preciseLimiter)
        {
            g_minFrameTime = 0.0f;
            g_intervalTicks = static_cast<int64_t>(interval * double(g_freq.QuadPart));
        }
        else
        {
            g_minFrameTime = static_cast<float>(interval); // vanilla busy-wait
            g_intervalTicks = 0;
        }
    }

    int64_t QpcNow()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    // Sleep most of the way with a high resolution timer, then spin for accuracy.
    void WaitUntil(int64_t target)
    {
        int64_t remaining = target - QpcNow();
        if (remaining <= 0)
            return;
        const int64_t spinTicks = g_freq.QuadPart / 1000 * 3 / 2; // ~1.5 ms
        if (g_timer && remaining > spinTicks)
        {
            const int64_t sleepTicks = remaining - spinTicks;
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(sleepTicks * 10000000LL / g_freq.QuadPart); // relative, 100 ns units
            if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(g_timer, 50);
        }
        while (QpcNow() < target)
            YieldProcessor();
    }

    // Refresh-synced pacing (borderless/windowed at the monitor's refresh rate).
    // Free-running at the refresh rate drifts against the display: some refreshes show a frame twice,
    // others skip one (looks like a lower frame rate). D3D9's own windowed vsync waits on the
    // compositor and stalls ~30 ms now and then. Instead every frame starts just after a vblank,
    // using the compositor's timing, so each refresh gets exactly one new frame.
    // Never combined with vsync: D3D9's windowed vsync Present returns just after the vblank, so the
    // synced wait would skip a refresh every frame (tested: ~50 fps).
    bool RefreshSyncWanted()
    {
        return g_settings.refreshSync && g_settings.fpsLimit < 0 && g_settings.windowMode != 0 && g_settings.vsync <= 0 &&
               !D3D9Proxy::UsingDxvk();
    }

    bool RefreshSyncedWait()
    {
        DWM_TIMING_INFO ti{};
        ti.cbSize = sizeof(ti);
        if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)) || ti.qpcRefreshPeriod == 0 || ti.qpcVBlank == 0)
            return false;
        static int64_t lastStart = 0;
        static bool logged = false;
        const int64_t period = static_cast<int64_t>(ti.qpcRefreshPeriod);
        const int64_t offset = period / 20; // start ~5% into the refresh interval
        const int64_t now = QpcNow();
        const int64_t base = static_cast<int64_t>(ti.qpcVBlank) + offset;
        int64_t target = base + ((now - base) / period + 1) * period;
        while (target <= now) target += period;
        if (lastStart && target - lastStart < period / 2)
            target += period; // never start two frames inside the same refresh
        if (!logged)
        {
            logged = true;
            LOG("Framerate: refresh-synced pacing active (%.3f Hz from the compositor)", double(g_freq.QuadPart) / double(period));
            Perf::SetTarget(double(period) * 1000.0 / double(g_freq.QuadPart));
        }
        WaitUntil(target);
        lastStart = target;
        return true;
    }

    void Limit()
    {
        static unsigned frame = 0;
        // Re-evaluate the refresh rate occasionally (window moved to another monitor, mode change).
        if ((frame++ & 255) == 0 || (g_settings.fpsLimit < 0 && Game::hWnd && MonitorFromWindow(Game::hWnd, MONITOR_DEFAULTTOPRIMARY) != g_monitor))
            UpdateInterval();

        // Debug: hold the slow key to run at 30 FPS (compare behaviour at low and high frame rates).
        if (g_settings.slowKey > 0 && (GetAsyncKeyState(g_settings.slowKey) & 0x8000) && Game::hWnd && GetForegroundWindow() == Game::hWnd)
        {
            static int64_t next = 0;
            const int64_t step = g_freq.QuadPart / 30;
            const int64_t now = QpcNow();
            if (next == 0 || now - next > step) next = now;
            next += step;
            WaitUntil(next);
            g_next = 0;
            return;
        }
        if (g_intervalTicks <= 0)
            return;
        if (RefreshSyncWanted() && RefreshSyncedWait())
            return;

        const int64_t now = QpcNow();
        if (g_next == 0 || now - g_next > g_intervalTicks)
            g_next = now; // first frame, or we fell far behind: don't try to catch up
        g_next += g_intervalTicks;
        WaitUntil(g_next);
    }

    void __cdecl OnBeforeMessagePump(Mem::Regs*)
    {
        Watchdog::Heartbeat();
        Perf::FrameEnd();
        Limit();
        Perf::FrameStart();
    }

    // ---- Logic rate -----------------------------------------------------------------------
    constexpr uintptr_t kActorClass = 0x24;     // UObject::Class
    constexpr uintptr_t kObjectName = 0x20;     // UObject::Name (FName index)
    constexpr uintptr_t kClassSuper = 0x28;     // UField::SuperField
    constexpr uintptr_t kActorFlags = 0x28;     // AActor first bitfield (bit 2 bDeleteMe, bit 5 bTicked)
    constexpr uintptr_t kActorXLevel = 0x7C;    // AActor::XLevel
    constexpr uintptr_t kLevelTicked = 0x3A20;  // ULevel::Ticked
    constexpr int kLevelTickAll = 2;            // ELevelTick::LEVELTICK_All
    constexpr size_t kTickSlot = 0xB0 / 4;

    using Tick_t = int(__fastcall*)(void* actor, void* edx, float dt, int type);

    float g_logicStep = 0.0f;

    enum : uint8_t { kFull = 0, kThrottle = 1, kPlayer = 2, kOptic = 3 };
    constexpr uint8_t kAIPawnBit = 0x80;   // class cache flag: NPC pawn (guard personalities)
    struct ClassKey
    {
        void* cls; int name;
        bool operator==(const ClassKey& o) const { return cls == o.cls && name == o.name; }
    };
    struct ClassKeyHash { size_t operator()(const ClassKey& k) const { return std::hash<void*>()(k.cls) ^ (size_t(k.name) * 2654435761u); } };
    std::unordered_map<ClassKey, uint8_t, ClassKeyHash> g_classCache;

    struct LevelClock
    {
        void* level = nullptr;
        int lastTicked = -1;
        float acc = 0.0f;
        float logicDt = 0.0f;
        bool logicFrame = true;
    };
    LevelClock g_clocks[4];
    unsigned g_throttledClasses = 0, g_fullClasses = 0;

    // SEH-guarded: reads the class chain names of an object into 'names' (most-derived first).
    int ReadClassChain(void* actor, wchar_t (*names)[64], int max)
    {
        int n = 0;
        __try
        {
            wchar_t buf[512];
            void* cls = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(actor) + kActorClass);
            for (; cls && n < max; cls = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(cls) + kClassSuper))
            {
                buf[0] = 0;
                const wchar_t* full = Game::GetFullName(cls, nullptr, buf);
                if (!full)
                    break;
                const wchar_t* dot = wcsrchr(full, L'.');
                const wchar_t* sp = wcsrchr(full, L' ');
                const wchar_t* name = dot ? dot + 1 : (sp ? sp + 1 : full);
                wcsncpy_s(names[n], 64, name, _TRUNCATE);
                ++n;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        return n;
    }

    uint8_t ClassifySlow(void* actor)
    {
        wchar_t chain[24][64];
        const int n = ReadClassChain(actor, chain, 24);
        if (n <= 0)
            return kFull;

        auto has = [&](const wchar_t* s) { for (int i = 0; i < n; ++i) if (_wcsicmp(chain[i], s) == 0) return true; return false; };
        auto contains = [&](const wchar_t* s)
        {
            for (int i = 0; i < n; ++i)
            {
                wchar_t lower[64];
                wcsncpy_s(lower, chain[i], _TRUNCATE);
                _wcslwr_s(lower);
                if (wcsstr(lower, s)) return true;
            }
            return false;
        };
        uint8_t r = kFull;
        const wchar_t* why = L"";
        // Anything the player drives or looks through always runs at full rate.
        if (has(L"EOpticCable"))
            r = kOptic, why = L"optic cable";
        else if (has(L"ESam"))
            r = kPlayer, why = L"player pawn";
        else if (has(L"PlayerController") || contains(L"esam") || contains(L"cam") || contains(L"turret") ||
            contains(L"transition") || contains(L"crane"))
            r = kFull, why = L"player-driven";
        else if (g_settings.logicRate <= 0)
            r = kFull, why = L"logic rate off";
        else if (g_settings.throttleAI && has(L"Controller"))
            r = kThrottle, why = L"AI controller";
        else if (g_settings.throttleNPCs && has(L"Pawn"))
            r = kThrottle, why = L"NPC pawn";
        else if (g_settings.throttlePhysicsProps && has(L"EHkRigidBody"))
            r = kThrottle, why = L"physics prop";

        r == kThrottle ? ++g_throttledClasses : ++g_fullClasses;
        const uint8_t guardBit = has(L"EAIPawn") ? kAIPawnBit : 0;
        if (r == kThrottle || g_settings.logActorClasses)
        {
            std::wstring s;
            for (int i = 0; i < n && i < 6; ++i) { if (i) s += L" < "; s += chain[i]; }
            LOG("LogicRate: %-8s %ls (%ls)", r == kThrottle ? "throttle" : "full", s.c_str(), why);
        }
        return r | guardBit;
    }

    uint8_t Classify(void* actor)
    {
        const auto base = reinterpret_cast<uintptr_t>(actor);
        void* cls = *reinterpret_cast<void**>(base + kActorClass);
        if (!cls)
            return kFull;
        const ClassKey key{ cls, *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(cls) + kObjectName) };
        auto it = g_classCache.find(key);
        if (it != g_classCache.end())
            return it->second;
        const uint8_t r = ClassifySlow(actor);
        g_classCache.emplace(key, r);
        return r;
    }

    LevelClock& ClockFor(void* level)
    {
        for (auto& c : g_clocks)
            if (c.level == level)
                return c;
        // Unknown level (new map): reset caches whose keys may now be stale.
        static unsigned rr = 0;
        auto& c = g_clocks[rr++ & 3];
        c = LevelClock{};
        c.level = level;
        g_classCache.clear();
        LOG("LogicRate: new level %p, logic step %.2f ms", level, g_logicStep * 1000.0f);
        return c;
    }

    // Replaces 'call dword ptr [eax+0B0h]' (AActor::Tick) at the ULevel::Tick call sites.
    int __fastcall TickDispatch(void* actor, void* edx, float dt, int type)
    {
        const auto base = reinterpret_cast<uintptr_t>(actor);
        const Tick_t orig = reinterpret_cast<Tick_t>((*reinterpret_cast<void***>(actor))[kTickSlot]);
        const int64_t perfStart = Perf::Begin();
        struct PerfScope { int64_t t; ~PerfScope() { Perf::AddActors(t); } } perfScope{ perfStart };
        if (type != kLevelTickAll)
            return orig(actor, edx, dt, type);

        void* level = *reinterpret_cast<void**>(base + kActorXLevel);
        if (!level)
            return orig(actor, edx, dt, type);

        LevelClock& clock = ClockFor(level);
        const int ticked = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(level) + kLevelTicked);
        if (ticked != clock.lastTicked)
        {
            // First actor of a new ULevel::Tick: advance this level's logic clock.
            clock.lastTicked = ticked;
            clock.acc += dt;
            clock.logicFrame = clock.acc >= g_logicStep * 0.999f;
            if (clock.logicFrame)
            {
                clock.logicDt = std::min(clock.acc, g_maxFrameTime);
                clock.acc = 0.0f;
            }
        }

        const uint8_t raw = Classify(actor);
        const uint8_t cls = raw & ~kAIPawnBit;
        struct AfterNpcTick { void* a; ~AfterNpcTick() { if (a) Perception::AfterPawnTick(a); } } afterNpc{ nullptr };
        if (raw & kAIPawnBit)
        {
            Guards::OnPawnTick(actor);
            Noise::OnNpcTick(actor);
            Alert::OnNpcTick(actor);
            Clusterfuck::OnNpcTick(actor);
            if (Perception::BeforePawnTick(actor)) afterNpc.a = actor;
        }
        if (cls == kPlayer)
        {
            Stuck::BeforePlayerTick(actor);
            const int r = orig(actor, edx, dt, type);
            Stuck::AfterPlayerTick(actor, dt);
            Watch::OnPlayerTick(actor);
            Guards::OnPlayerTick(actor);
            Perception::OnPlayerTick(actor);
            Reaction::OnPlayerTick(actor);
            Noise::OnPlayerTick(actor);
            Alert::OnPlayerTick(actor);
            Scripted::OnPlayerTick(actor);
            Clusterfuck::OnPlayerTick(actor);
            Movement::OnPlayerTick(actor);
            Scripting::OnPlayerTick(actor, dt);
            return r;
        }
        if (cls == kOptic)
        {
            Movement::BeforeOpticTick(actor, dt);
            const int r = orig(actor, edx, dt, type);
            Movement::AfterOpticTick(actor);
            return r;
        }
        if (cls != kThrottle || g_logicStep <= 0.0f)
            return orig(actor, edx, dt, type);

        if (clock.logicFrame)
            return orig(actor, edx, clock.logicDt, type);

        // Skipped this frame: mark it ticked so owner-first ordering doesn't wait for it.
        auto& flags = *reinterpret_cast<uint32_t*>(base + kActorFlags);
        flags = (flags & ~0x20u) | ((ticked & 1u) << 5);
        return 1;
    }

    void InstallLogicRate()
    {
        // The tick hook also brackets the player pawn's tick for the unstick fix.
        if (g_settings.logicRate <= 0 && !Stuck::Wanted() && g_settings.opticCableSpeed <= 0.0f && !g_settings.watchVisCrouchMul &&
            !g_settings.guardPersonalities && !g_settings.rescueStuckNPCs && !g_settings.wallBounce)
        {
            LOG("Framerate: logic rate throttling disabled");
            return;
        }
        if (!Game::GetFullName)
        {
            LOG("Framerate: GetFullName not found, logic rate throttling unavailable");
            return;
        }
        // Three call sites: main actor loop, deferred (owner-first) list, paused/always-tick list.
        auto sites = Mem::FindPattern("8B 54 24 38 8B 01 52 8B 54 24 40 52 FF 90 B0 00 00 00");
        if (sites.size() != 3)
        {
            LOG("  [!] actor tick call sites: %u found (expected 3), logic rate unavailable", unsigned(sites.size()));
            return;
        }
        for (auto s : sites)
            Mem::MakeCall(s + 12, reinterpret_cast<void*>(&TickDispatch), 6);
        if (g_settings.logicRate > 0)
        {
            g_logicStep = 1.0f / static_cast<float>(g_settings.logicRate);
            LOG("Framerate: logic rate %d Hz (AI=%d NPCs=%d props=%d)", g_settings.logicRate,
                g_settings.throttleAI, g_settings.throttleNPCs, g_settings.throttlePhysicsProps);
        }
        else
            LOG("Framerate: logic rate throttling disabled (tick hook kept for the player)");
    }
}

void Framerate::Init()
{
    QueryPerformanceFrequency(&g_freq);
    g_maxFrameTime = g_settings.maxFrameTime;

    // The main loop reads its min/max frame time from two globals; point it at ours.
    if (auto a = Mem::Find("frame time clamp", "8B 15 ? ? ? ? A1 ? ? ? ? 89 54 24 10 89 44 24 18"))
    {
        const float vanillaMin = *Mem::Read<float*>(a + 2);
        const float vanillaMax = *Mem::Read<float*>(a + 7);
        Mem::Write<float*>(a + 2, &g_minFrameTime);
        Mem::Write<float*>(a + 7, &g_maxFrameTime);
        LOG("Framerate: vanilla min/max frame time %.4f / %.4f replaced", vanillaMin, vanillaMax);
    }

    g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!g_timer)
    {
        g_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        timeBeginPeriod(1);
        LOG("Framerate: high resolution timer unavailable, using standard waitable timer");
    }
    UpdateInterval();

    // Limiter runs at the end of each frame, right before the message pump (lowest latency).
    if (auto a = Mem::Find("frame end", "8B 47 44 39 58 30 74 ? 8B 50 2C 8B 02 8B 48 5C 8B 11 FF 92 B4 00 00 00"))
        Mem::MidHook("frame limiter", a, 6, &OnBeforeMessagePump, Mem::MidMode::OriginalAfter);

    InstallLogicRate();
}

int Game::ClassChain(void* obj, wchar_t (*names)[64], int max)
{
    return Game::GetFullName ? ReadClassChain(obj, names, max) : -1;
}
