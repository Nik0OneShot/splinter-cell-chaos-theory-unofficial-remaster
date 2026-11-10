// Player collision: unstick fix ([Gameplay] UnstickPlayer) and diagnostics ([Debug] StuckDiagnostics).
//
// Some spots in the original levels trap Sam (seen in Lighthouse against SM_StructureCellar02_LGH_15):
// his collision box ends up slightly INSIDE a wall mesh. Unreal's swept collision blocks every move that
// starts overlapping something (Hit.Time 0), in every direction, so he cannot walk out and cannot stand up
// (the taller box does not fit either). High frame rates make it harder to slip out (smaller moves per
// frame), but the trap is vanilla.
//
// Unstick: when Sam is walking, every step-up the engine tries for him is blocked at its start and he has
// not moved for a quarter of a second:
//  1. If his collision box overlaps level geometry (ULevel::SinglePointCheck, the same test the engine
//     uses to place spawned and teleported actors), search outwards for the nearest spot where it fits
//     (a few units at most, never further than his own radius, so never through a wall) and put him there
//     with ULevel::FarMoveActor, the engine's teleport (what UnrealScript SetLocation uses).
//  2. Otherwise, if the blocking surface is level geometry he is NOT pushing into, nudge him a few units
//     with ULevel::MoveActor (collision-checked, the same as UnrealScript Move()) and check that he can now
//     move the way he wants. A failed nudge is undone.
// Walking straight into a wall or into a corner never triggers either: the box does not overlap anything
// there (MoveActor keeps it 2 units away), and the wall faces against the desired direction.
//
// APawn::stepUp(GravDir, DesiredDir, Delta, FCheckResult& Hit) is at 0x10A8C3E0 (AActor::stepUp at 0x10A8CB20).
// ULevel::MoveActor(Actor, Delta, Rotation, Hit, bTest, bIgnorePawns, bIgnoreBases, bNoFail) = ULevel vtable +84h.
// ULevel::FarMoveActor(Actor, Dest, bTest, bNoCheck, bAttachedMove) = vtable +88h (0x10A94200). With bNoCheck 0 it
// runs FindSpot (vtable +A4h) and the encroachment check (vtable +ACh) itself.
// ULevel::SinglePointCheck(Location, Extent, TraceFlags, SourceActor, FCheckResult* Hit) = vtable +B0h
// (0x10A946B0); returns 1 if the box fits. FarMoveActor's trace flags: 4, or 8004h/10004h/20004h for actor
// flag bits (+2Ch) 20h/40h/10000h; SinglePointCheck adds 14h.
// AActor: Physics +4Ch (byte, 1 = PHYS_Walking), XLevel +7Ch, Location +E8h, Rotation +F4h, Velocity +100h,
// Acceleration +10Ch, CollisionRadius +220h, CollisionHeight +224h.
// FCheckResult: Actor +04h, Location +08h, Normal +14h, Time +24h, Item +28h.
#include "pch.h"
#include "features.h"

namespace
{
    constexpr uintptr_t kClass = 0x24, kName = 0x20;
    constexpr uintptr_t kPhysics = 0x4C, kXLevel = 0x7C, kLocation = 0xE8, kRotation = 0xF4, kVelocity = 0x100, kAccel = 0x10C;
    constexpr uintptr_t kActorFlags = 0x2C, kCollisionRadius = 0x220, kCollisionHeight = 0x224;
    constexpr uintptr_t kHitActor = 0x04, kHitNormal = 0x14, kHitTime = 0x24, kHitItem = 0x28;
    constexpr uint8_t kPhysWalking = 1;
    constexpr size_t kMoveActorSlot = 0x84 / 4;

    constexpr float kTriggerTime = 0.25f;   // seconds of being blocked before a nudge
    constexpr int kTriesPerSpot = 3;        // nudges per spot (within kSpotRadius, kSpotMemoryMs)
    constexpr float kSpotRadius = 24.0f;
    constexpr DWORD kSpotMemoryMs = 5000;

    struct Vec
    {
        float x, y, z;
        Vec operator+(const Vec& o) const { return { x + o.x, y + o.y, z + o.z }; }
        Vec operator-(const Vec& o) const { return { x - o.x, y - o.y, z - o.z }; }
        Vec operator*(float s) const { return { x * s, y * s, z * s }; }
    };
    float Dot(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    float Len(const Vec& v) { return std::sqrt(Dot(v, v)); }
    // Horizontal part of v, normalised. False if it is shorter than minLen (no usable direction).
    bool Flat(const Vec& v, Vec& out, float minLen)
    {
        const float l = std::sqrt(v.x * v.x + v.y * v.y);
        if (l < minLen)
            return false;
        out = { v.x / l, v.y / l, 0.0f };
        return true;
    }
    template <class T> T& At(void* p, uintptr_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p) + off); }
    Vec Loc(void* actor) { return At<Vec>(actor, kLocation); }

    using StepUp_t = void(__fastcall*)(void* self, void* edx, float gx, float gy, float gz, float dx, float dy, float dz,
                                       float mx, float my, float mz, uint8_t* hit);
    using MoveActor_t = int(__fastcall*)(void* level, void* edx, void* actor, float dx, float dy, float dz,
                                         int pitch, int yaw, int roll, uint8_t* hit,
                                         int bTest, int bIgnorePawns, int bIgnoreBases, int bNoFail);
    using FarMoveActor_t = int(__fastcall*)(void* level, void* edx, void* actor, float x, float y, float z,
                                            int bTest, int bNoCheck, int bAttachedMove);
    using SinglePointCheck_t = int(__fastcall*)(void* level, void* edx, float lx, float ly, float lz,
                                                float ex, float ey, float ez, uint32_t traceFlags, void* source, uint8_t* hit);
    StepUp_t oPawnStepUp = nullptr;
    StepUp_t oActorStepUp = nullptr;
    FarMoveActor_t pFarMoveActor = nullptr;
    SinglePointCheck_t pSinglePointCheck = nullptr;

    // Set while the player pawn is ticking (Before/AfterPlayerTick).
    void* g_player = nullptr;
    Vec g_before{};
    int g_stepDepth = 0;
    struct Blocked { bool valid; Vec normal; Vec desired; void* actor; };
    Blocked g_blocked{};

    // Unstick state per player pawn (co-op has two).
    struct PawnState
    {
        void* pawn = nullptr;
        float blockedTime = 0.0f, quietTime = 0.0f;
        Vec spot{};
        int tries = 0;
        DWORD lastTry = 0;
    };
    PawnState g_states[2];
    unsigned g_stateNext = 0;

    PawnState& StateFor(void* pawn)
    {
        for (auto& s : g_states)
            if (s.pawn == pawn)
                return s;
        auto& s = g_states[g_stateNext++ & 1];
        s = PawnState{};
        s.pawn = pawn;
        return s;
    }

    // Diagnostics state.
    float g_stuckTime = 0.0f;
    bool g_episode = false;
    int g_logStepUps = 0;
    int g_episodes = 0;

    bool MovementKeysHeld()
    {
        static const int keys[] = { 'W', 'A', 'S', 'D', VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
        for (int k : keys)
            if (GetAsyncKeyState(k) & 0x8000) return true;
        return false;
    }

    bool SafeFullName(void* actor, wchar_t* buf)
    {
        __try { Game::GetFullName(actor, nullptr, buf); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    std::string ActorName(void* actor)
    {
        if (!actor || !Game::GetFullName) return "none";
        wchar_t buf[256] = {};
        if (!SafeFullName(actor, buf)) return "?";
        char out[256];
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), nullptr, nullptr);
        return out;
    }

    // Static level geometry: BSP (the LevelInfo), static meshes, terrain, blocking volumes.
    std::unordered_map<uint64_t, bool> g_geometryCache;
    bool IsLevelGeometry(void* actor)
    {
        if (!actor)
            return false;
        void* cls = At<void*>(actor, kClass);
        if (!cls)
            return false;
        const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(cls)) | (uint64_t(uint32_t(At<int>(cls, kName))) << 32);
        auto it = g_geometryCache.find(key);
        if (it != g_geometryCache.end())
            return it->second;
        wchar_t chain[24][64];
        const int n = Game::ClassChain(actor, chain, 24);
        bool r = false;
        for (int i = 0; i < n && !r; ++i)
            r = !_wcsicmp(chain[i], L"StaticMeshActor") || !_wcsicmp(chain[i], L"LevelInfo") ||
                !_wcsicmp(chain[i], L"TerrainInfo") || !_wcsicmp(chain[i], L"BlockingVolume");
        g_geometryCache.emplace(key, r);
        return r;
    }

    // ULevel::MoveActor for the pawn (collision-checked). Returns how far it actually moved.
    float Move(void* pawn, const Vec& d)
    {
        void* level = At<void*>(pawn, kXLevel);
        if (!level)
            return 0.0f;
        const auto fn = reinterpret_cast<MoveActor_t>((*reinterpret_cast<void***>(level))[kMoveActorSlot]);
        alignas(8) uint8_t hit[0x100] = {};
        At<float>(hit, kHitTime) = 1.0f;
        At<int>(hit, kHitItem) = -1;
        const int pitch = At<int>(pawn, kRotation), yaw = At<int>(pawn, kRotation + 4), roll = At<int>(pawn, kRotation + 8);
        const Vec before = Loc(pawn);
        fn(level, nullptr, pawn, d.x, d.y, d.z, pitch, yaw, roll, hit, 0, 0, 0, 0);
        return Len(Loc(pawn) - before);
    }

    // n: blocking surface (horizontal, pointing out of it), f: where the player wants to go (horizontal).
    bool TryFree(void* pawn, const Vec& n, const Vec& f)
    {
        Vec side{ -f.y, f.x, 0.0f };
        if (Dot(side, n) < 0.0f)
            side = side * -1.0f;
        const Vec up{ 0.0f, 0.0f, 1.0f };
        const struct { const char* name; Vec d; } tries[] = {
            { "off the surface", n * 3.0f },
            { "sideways", side * 3.0f },
            { "back", f * -3.0f },
            { "up", up * 8.0f },
            { "other side", side * -3.0f },
            { "far off the surface", n * 6.0f },
        };
        const Vec start = Loc(pawn);
        for (const auto& t : tries)
        {
            Move(pawn, t.d);
            Move(pawn, f * 6.0f);
            if (Dot(Loc(pawn) - start, f) < 1.5f)
                Move(pawn, f * 6.0f + up * 2.0f);   // rising floor
            const Vec total = Loc(pawn) - start;
            if (Dot(total, f) >= 1.5f)
            {
                LOG("[UNSTICK] freed Sam at (%.1f %.1f %.1f): nudged %s, moved (%.2f %.2f %.2f)",
                    start.x, start.y, start.z, t.name, total.x, total.y, total.z);
                return true;
            }
            Move(pawn, start - Loc(pawn));   // undo, try the next direction from the same place
        }
        const Vec total = Loc(pawn) - start;
        LOG("[UNSTICK] could not free Sam at (%.1f %.1f %.1f) (ended %.2f units away)", start.x, start.y, start.z, Len(total));
        return false;
    }

    // Trace flags FarMoveActor hands to FindSpot for this actor (SinglePointCheck adds 14h itself).
    uint32_t PlacementFlags(void* actor)
    {
        const uint32_t f = At<uint32_t>(actor, kActorFlags);
        if (f & 0x20) return 0x8004;
        if (f & 0x40) return 0x10004;
        if (f & 0x10000) return 0x20004;
        return 4;
    }

    // 1 = the box fits at p, 0 = it overlaps something (*blocker = what), -1 = the call failed.
    int PointCheck(void* level, void* pawn, const Vec& p, const Vec& ext, void** blocker)
    {
        alignas(8) uint8_t hit[0x40] = {};
        At<float>(hit, kHitTime) = 1.0f;
        At<int>(hit, kHitItem) = -1;
        int r = 0;
        __try { r = pSinglePointCheck(level, nullptr, p.x, p.y, p.z, ext.x, ext.y, ext.z, PlacementFlags(pawn), pawn, hit); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        if (blocker)
            *blocker = At<void*>(hit, kHitActor);
        return r == 1 ? 1 : 0;
    }

    // ULevel::FarMoveActor with its own checks on. 1 = moved, 0 = refused, -1 = the call failed.
    int FarMove(void* level, void* pawn, const Vec& p)
    {
        __try { return pFarMoveActor(level, nullptr, pawn, p.x, p.y, p.z, 0, 0, 0) ? 1 : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    bool g_engineCallsFailed = false;

    bool Box(void* pawn, void*& level, Vec& ext)
    {
        level = At<void*>(pawn, kXLevel);
        const float r = At<float>(pawn, kCollisionRadius), h = At<float>(pawn, kCollisionHeight);
        ext = { r, r, h };
        return level && pSinglePointCheck && pFarMoveActor && !g_engineCallsFailed &&
               r > 1.0f && r < 200.0f && h > 1.0f && h < 300.0f;
    }

    // 1 = Sam's collision box overlaps something solid where he stands, 0 = it does not, -1 = cannot tell.
    int Overlapping(void* pawn, void** blocker)
    {
        void* level;
        Vec ext;
        if (!Box(pawn, level, ext))
            return -1;
        const int fits = PointCheck(level, pawn, Loc(pawn), ext, blocker);
        if (fits < 0)
        {
            g_engineCallsFailed = true;
            LOG("[UNSTICK] SinglePointCheck failed; overlap fix disabled");
            return -1;
        }
        return fits ? 0 : 1;
    }

    // Candidate offsets, nearest first: dist (horizontal), up, dir (steps of 22.5 degrees from "away":
    // 0, +1, -1, +2, -2, ... so the first tries point out of the surface he is stuck in).
    struct Candidate { float dist, up; int dir; };
    const std::vector<Candidate>& Candidates()
    {
        static const std::vector<Candidate> list = [] {
            std::vector<Candidate> v;
            for (float u : { 1.0f, 2.0f, 4.0f, 8.0f })
                v.push_back({ 0.0f, u, 0 });
            for (float d : { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 10.0f, 12.0f, 14.0f, 16.0f, 20.0f, 24.0f })
                for (float u : { 0.0f, 1.0f, 2.0f, 4.0f })
                    for (int k = 0; k < 16; ++k)
                        v.push_back({ d, u, (k & 1) ? (k + 1) / 2 : -(k / 2) });
            std::stable_sort(v.begin(), v.end(), [](const Candidate& a, const Candidate& b) {
                return a.dist + a.up * 1.5f < b.dist + b.up * 1.5f;
            });
            return v;
        }();
        return list;
    }

    // Sam's box is inside something: put him at the nearest spot where it fits. The search stays within
    // his own radius, so the new spot always overlaps where he was and can never be past a wall.
    bool MoveOut(void* pawn, const Vec& away, void* inside)
    {
        void* level;
        Vec ext;
        if (!Box(pawn, level, ext))
            return false;
        const Vec start = Loc(pawn);
        const float maxDist = std::clamp(ext.x * 0.9f, 4.0f, 24.0f);
        const float base = std::atan2(away.y, away.x);
        int checks = 0;
        for (const auto& c : Candidates())
        {
            if (c.dist > maxDist)
                continue;
            const float a = base + static_cast<float>(c.dir) * 0.39269908f;
            const Vec p = start + Vec{ std::cos(a) * c.dist, std::sin(a) * c.dist, c.up };
            ++checks;
            const int fits = PointCheck(level, pawn, p, ext, nullptr);
            const int moved = fits == 1 ? FarMove(level, pawn, p) : fits;
            if (moved < 0)
            {
                g_engineCallsFailed = true;
                LOG("[UNSTICK] engine call failed; overlap fix disabled");
                return false;
            }
            if (moved == 0)
                continue;   // no room there, or FarMoveActor refused (another pawn standing there)
            const Vec d = Loc(pawn) - start;
            LOG("[UNSTICK] freed Sam at (%.1f %.1f %.1f): his collision box (radius %.1f, height %.1f) was inside %s; "
                "moved (%.2f %.2f %.2f) after %d checks",
                start.x, start.y, start.z, ext.x, ext.z, ActorName(inside).c_str(), d.x, d.y, d.z, checks);
            return true;
        }
        LOG("[UNSTICK] Sam's collision box (radius %.1f, height %.1f) is inside %s at (%.1f %.1f %.1f), "
            "and there is no free spot within %.0f units (%d checks)",
            ext.x, ext.z, ActorName(inside).c_str(), start.x, start.y, start.z, maxDist, checks);
        return false;
    }

    void Unstick(void* pawn, float dt)
    {
        PawnState& st = StateFor(pawn);
        const Vec loc = Loc(pawn);
        const float moved = Len(loc - g_before);
        const bool blocked = g_blocked.valid && At<uint8_t>(pawn, kPhysics) == kPhysWalking && moved < 0.05f;
        if (blocked)
        {
            st.blockedTime += dt;
            st.quietTime = 0.0f;
        }
        else
        {
            st.quietTime += dt;
            if (moved > 0.5f || st.quietTime > 0.1f)
                st.blockedTime = 0.0f;
        }
        if (st.blockedTime < kTriggerTime)
            return;
        st.blockedTime = 0.0f;

        Vec n{}, f{};
        const bool hasN = Flat(g_blocked.normal, n, 0.7f);   // a wall-like surface (not floor/ceiling)
        const bool hasF = Flat(g_blocked.desired, f, 0.3f);
        // Nudging only makes sense against level geometry he is not pushing into.
        const bool canNudge = hasN && hasF && Dot(f, n) > -0.5f && IsLevelGeometry(g_blocked.actor);
        // Checking for overlap is cheap and harmless when he is just standing against a wall (no overlap
        // there), so it runs for any blocked step; nothing happens unless one of the two fixes applies.
        void* inside = nullptr;
        const int overlap = Overlapping(pawn, &inside);
        if (overlap != 1 && !canNudge)
            return;

        const DWORD now = GetTickCount();
        if (now - st.lastTry > kSpotMemoryMs || Len(loc - st.spot) > kSpotRadius)
        {
            st.tries = 0;
            st.spot = loc;
        }
        if (st.tries < kTriesPerSpot)
        {
            ++st.tries;
            st.lastTry = now;
            if (overlap == 1)
            {
                // Nudges cannot help while the box overlaps something: every move from inside is blocked.
                if (MoveOut(pawn, hasN ? n : (hasF ? f * -1.0f : Vec{}), inside))
                    st.tries = 0;   // freed; walking back into the same trap may need it again
            }
            else
            {
                LOG("[UNSTICK] Sam blocked by %s, surface (%.2f %.2f %.2f), wants (%.2f %.2f)",
                    ActorName(g_blocked.actor).c_str(), g_blocked.normal.x, g_blocked.normal.y, g_blocked.normal.z, f.x, f.y);
                TryFree(pawn, n, f);
            }
        }
        else if (st.tries == kTriesPerSpot)
        {
            ++st.tries;
            LOG("[UNSTICK] giving up at (%.1f %.1f %.1f) for now", loc.x, loc.y, loc.z);
        }
    }

    void Diagnose(void* pawn, float dt)
    {
        const Vec after = Loc(pawn);
        const float moved = Len(after - g_before);
        const Vec accel = At<Vec>(pawn, kAccel);
        const Vec vel = At<Vec>(pawn, kVelocity);
        const bool trying = MovementKeysHeld() || Len(accel) > 1.0f;

        if (trying && moved < 0.02f * (dt / 0.006f + 0.01f))
        {
            g_stuckTime += dt;
            if (g_stuckTime >= 0.5f && !g_episode)
            {
                g_episode = true;
                if (++g_episodes <= 20)
                {
                    LOG("[STUCK] #%d Sam is not moving: loc (%.2f %.2f %.2f) vel (%.2f %.2f %.2f) accel (%.1f %.1f %.1f) phys %d dt %.4f s",
                        g_episodes, after.x, after.y, after.z, vel.x, vel.y, vel.z, accel.x, accel.y, accel.z,
                        At<uint8_t>(pawn, kPhysics), dt);
                    g_logStepUps = 24;
                }
            }
        }
        else if (moved > 0.5f)
        {
            if (g_episode)
                LOG("[STUCK] moving again after %.2f s (dt %.4f s, moved %.2f this frame)", g_stuckTime, dt, moved);
            g_stuckTime = 0.0f;
            g_episode = false;
        }
    }

    void LogStepUp(const char* which, void* self, float mx, float my, float mz, const Vec& inN, float inT,
                   uint8_t* hit, const Vec& locBefore)
    {
        const Vec n = At<Vec>(hit, kHitNormal);
        const Vec moved = Loc(self) - locBefore;
        LOG("[STUCK]   %s Delta(%.3f %.3f %.3f) in: t=%.3f n=(%.2f %.2f %.2f) out: t=%.3f n=(%.2f %.2f %.2f) %s -> moved (%.3f %.3f %.3f)",
            which, mx, my, mz, inT, inN.x, inN.y, inN.z, At<float>(hit, kHitTime), n.x, n.y, n.z,
            ActorName(At<void*>(hit, kHitActor)).c_str(), moved.x, moved.y, moved.z);
    }

    void __fastcall hkPawnStepUp(void* self, void* edx, float gx, float gy, float gz, float dx, float dy, float dz,
                                 float mx, float my, float mz, uint8_t* hit)
    {
        if (!g_player || self != g_player)
            return oPawnStepUp(self, edx, gx, gy, gz, dx, dy, dz, mx, my, mz, hit);

        const Vec before = Loc(self);
        const Vec inNormal = At<Vec>(hit, kHitNormal);
        const float inTime = At<float>(hit, kHitTime);
        ++g_stepDepth;   // stepUp retries itself recursively; judge the outermost call
        oPawnStepUp(self, edx, gx, gy, gz, dx, dy, dz, mx, my, mz, hit);
        if (--g_stepDepth != 0)
            return;

        if (At<float>(hit, kHitTime) < 0.01f && Len(Loc(self) - before) < 0.01f)
            g_blocked = { true, At<Vec>(hit, kHitNormal), { dx, dy, dz }, At<void*>(hit, kHitActor) };
        if (g_logStepUps > 0)
        {
            --g_logStepUps;
            LogStepUp("pawn stepUp", self, mx, my, mz, inNormal, inTime, hit, before);
        }
    }

    void __fastcall hkActorStepUp(void* self, void* edx, float gx, float gy, float gz, float dx, float dy, float dz,
                                  float mx, float my, float mz, uint8_t* hit)
    {
        if (g_logStepUps > 0 && g_player && self == g_player)
        {
            const Vec before = Loc(self);
            const Vec inNormal = At<Vec>(hit, kHitNormal);
            const float inTime = At<float>(hit, kHitTime);
            --g_logStepUps;
            oActorStepUp(self, edx, gx, gy, gz, dx, dy, dz, mx, my, mz, hit);
            LogStepUp("actor stepUp", self, mx, my, mz, inNormal, inTime, hit, before);
            return;
        }
        oActorStepUp(self, edx, gx, gy, gz, dx, dy, dz, mx, my, mz, hit);
    }
}

bool Stuck::Wanted()
{
    return g_settings.unstickPlayer || g_settings.stuckDiagnostics;
}

void Stuck::Init()
{
    if (!Wanted())
        return;
    if (!Game::GetFullName)
    {
        LOG("Stuck: GetFullName not found, unstick/diagnostics unavailable");
        return;
    }
    if (auto a = Mem::Find("APawn::stepUp", "83 EC 34 D9 44 24 38 53 D8 0D ? ? ? ? 55 56 57 8B 7C 24 6C"))
        Mem::Detour("APawn::stepUp", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkPawnStepUp), reinterpret_cast<void**>(&oPawnStepUp));
    if (g_settings.stuckDiagnostics)
        if (auto a = Mem::Find("AActor::stepUp", "83 EC 30 D9 44 24 34 53 D8 0D ? ? ? ? 55 56"))
            Mem::Detour("AActor::stepUp", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkActorStepUp), reinterpret_cast<void**>(&oActorStepUp));
    if (g_settings.unstickPlayer)
    {
        pFarMoveActor = reinterpret_cast<FarMoveActor_t>(
            Mem::Find("ULevel::FarMoveActor", "A1 ? ? ? ? 81 EC DC 00 00 00 85 C0 53 55 56 8B B4 24 EC 00 00 00 57 8B E9"));
        pSinglePointCheck = reinterpret_cast<SinglePointCheck_t>(
            Mem::Find("ULevel::SinglePointCheck", "83 EC 0C 8B 15 ? ? ? ? 53 8B 5C 24 2C 55 56 8B 74 24 28 A1 ? ? ? ? 57 8B 7C 24 3C"));
    }
    LOG("Stuck: unstick=%d (overlap fix %s) diagnostics=%d", g_settings.unstickPlayer,
        pFarMoveActor && pSinglePointCheck ? "on" : "off", g_settings.stuckDiagnostics);
}

void Stuck::BeforePlayerTick(void* pawn)
{
    if (!oPawnStepUp)
        return;
    g_player = pawn;
    g_before = Loc(pawn);
    g_blocked = {};
    g_stepDepth = 0;
}

void Stuck::AfterPlayerTick(void* pawn, float dt)
{
    if (!g_player || pawn != g_player)
        return;
    if (g_settings.stuckDiagnostics)
        Diagnose(pawn, dt);
    if (g_settings.unstickPlayer)
        Unstick(pawn, dt);
    g_player = nullptr;
}
