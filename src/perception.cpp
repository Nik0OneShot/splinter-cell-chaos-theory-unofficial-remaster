// MGS2-style perception options ([Perception] in SCCTRemaster_Guards.ini).
//
// The guards' sight check (around 0x10BB2716) multiplies Sam's light level by several factors:
//   movement   0x10B9EC20  x1 up to 75 units/s, rising to x2 at 420 units/s (player: average of 10 samples)
//   distance   0x10B9A850  falloff to 0 at the guard's sight range, up to x2 inside 35% of it
//   silhouette 0x10B9EB70  beyond 30% of sight range: 0.7 + 0.3 x (box height / 175) x (range - dist) / (0.7 range)
//   peripheral 0x10B9ED20  computes the angle to the target, then discards it and returns 1 (disabled)
// plus the light/zone factor, the guard's fVisibilityDecreaseFactor and the difficulty multiplier.
//
// Options:
//  - ShadowDetection: ELight::PostBeginPlay (native, 0x10C16DA0) registers a light with the AI manager's
//    ShadowDetectLights (guards check them round-robin for Sam's shadow) only if bCastShadows (+2F8h bit 1)
//    and bNPCShadowDetect (+360h bit 2) are set; only a few lights in 9 maps have the latter. 1 = every
//    shadow-casting light counts.
//  - PeripheralVision: the peripheral factor returns 1 at the centre of the guard's view, fading to
//    PeripheralEdge at the edge of his cone (smoothstep from PeripheralStart of the half-cone). Targets the
//    guard is already fully aware of are not affected.
//  - CrouchSilhouette: a standing player is judged by his crouched silhouette (CrouchHeight / CollisionHeight
//    applied to the height term), so standing is no easier to spot at a distance than crouching.
//  - DirectionalLighting / DarkSide: each guard sees the side of Sam facing him; lamps behind Sam light the side
//    he can't see. PartialCover: of the engine's four light points on Sam, a guard only counts the ones he can
//    see (see "Sam's light as each guard sees it" below).
//  - Backlighting: a background brighter than Sam outlines him (see "Backlighting" below).
// Both of the last two act on the light level the sight check reads (hook after it is stored at [esp+14h]).
#include "pch.h"
#include "features.h"
#include "aievents.h"

extern HMODULE g_self;

namespace
{
    struct Vec { float x, y, z; };

    bool g_peripheral = true;
    float g_edge = 0.4f, g_start = 0.35f;
    bool g_crouchSilhouette = true;
    bool g_log = false;

    using Peripheral_t = float(__fastcall*)(void* ctrl, void* edx, float vx, float vy, float vz, int idx, int a, int b);
    using Silhouette_t = float(__fastcall*)(void* ctrl, void* edx, void* target, float dist, float range);
    Peripheral_t oPeripheral = nullptr;
    Silhouette_t oSilhouette = nullptr;

    template <class T> T& At(void* p, uintptr_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p) + off); }

    // AI controller: perception table +4F0h (0x64 per target, awareness byte +34h, 7Ah = fully aware),
    // Pawn +41Ch, Rotation +F4h. Pawn: Location +E8h, EyeHeight +310h, m_VisibilityConeAngle +9E4h: the HALF-angle
    // in degrees (the engine's own cone test, pawn function 0x10B7F4B0, compares acos(dot) directly against it).
    float PeripheralFactor(void* ctrl, const Vec& target, int idx, float* angleOut)
    {
        *angleOut = -1;
        if (idx < 0) return 1.0f;
        const uintptr_t table = At<uintptr_t>(ctrl, 0x4F0);
        if (!table || *reinterpret_cast<const uint8_t*>(table + idx * 0x64 + 0x34) >= 0x7A) return 1.0f;
        void* pawn = At<void*>(ctrl, 0x41C);
        if (!pawn) return 1.0f;
        const float cone = At<float>(pawn, 0x9E4);
        if (!(cone > 1.0f && cone < 360.0f)) return 1.0f;
        // The guard's real eye position and view rotation (head included): the same pawn virtual (+304h,
        // (FVector* loc, FRotator* rot, 0)) the original function calls before discarding its result.
        // The second output is the view DIRECTION (a vector), not a rotator: the original function dots it
        // with the normalised direction to the target.
        Vec eye{}, fwd{};
        using ViewPoint_t = void(__fastcall*)(void* pawn, void* edx, Vec* loc, Vec* dir, int flag);
        const auto viewPoint = reinterpret_cast<ViewPoint_t>((*reinterpret_cast<void***>(pawn))[0x304 / 4]);
        viewPoint(pawn, nullptr, &eye, &fwd, 0);
        const float fl = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
        if (!(fl > 0.01f && fl < 100.0f)) return 1.0f;
        fwd = { fwd.x / fl, fwd.y / fl, fwd.z / fl };
        Vec to{ target.x - eye.x, target.y - eye.y, target.z - eye.z };
        const float len = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
        if (len < 1.0f) return 1.0f;
        const float c = std::clamp((fwd.x * to.x + fwd.y * to.y + fwd.z * to.z) / len, -1.0f, 1.0f);
        const float angle = std::acos(c) * (180.0f / 3.14159265f);
        *angleOut = angle;
        const float t = angle / cone;
        if (t <= g_start) return 1.0f;
        const float u = std::clamp((t - g_start) / (1.0f - g_start), 0.0f, 1.0f);
        return 1.0f + (g_edge - 1.0f) * (u * u * (3.0f - 2.0f * u));
    }

    void MaybeLog(const char* what, float a, float b, float c)
    {
        if (!g_log) return;
        static DWORD last[2] = {};
        const int i = what[0] == 'p' ? 0 : 1;
        const DWORD now = GetTickCount();
        if (now - last[i] < 2000) return;
        last[i] = now;
        LOG("[PERCEPTION] %s %.2f %.2f -> x%.2f", what, a, b, c);
    }

    float __fastcall hkPeripheral(void* ctrl, void* edx, float vx, float vy, float vz, int idx, int a, int b)
    {
        const float orig = oPeripheral(ctrl, edx, vx, vy, vz, idx, a, b);
        if (!g_peripheral || !ctrl) return orig;
        float angle = -1;
        const float f = PeripheralFactor(ctrl, { vx, vy, vz }, idx, &angle);
        if (angle >= 0) MaybeLog("peripheral angle/cone", angle, At<float>(At<void*>(ctrl, 0x41C), 0x9E4), f);
        return orig * f;
    }

    // Pawn: bIsPlayerPawn = Actor +2Ch bit 20h, bIsCrouched +2B8h bit 2, CrouchHeight +320h,
    // CollisionHeight +224h.
    float __fastcall hkSilhouette(void* ctrl, void* edx, void* target, float dist, float range)
    {
        const float f = oSilhouette(ctrl, edx, target, dist, range);
        if (!g_crouchSilhouette || !target || f >= 0.9999f || f <= 0.7f) return f;
        if (!(At<uint32_t>(target, 0x2C) & 0x20) || (At<uint32_t>(target, 0x2B8) & 2)) return f;
        const float crouch = At<float>(target, 0x320), stand = At<float>(target, 0x224);
        if (!(crouch > 1.0f && stand > crouch && stand < 1000.0f)) return f;
        const float g = 0.7f + (f - 0.7f) * (crouch / stand);
        MaybeLog("silhouette standing/crouched", f, crouch / stand, g);
        return g;
    }

    // ---- Backlighting --------------------------------------------------------------------------------
    // The sight check (0x10BB2210, thiscall on the AI controller; edi = target, ebx = target point) asks the
    // target for its light level (target virtual +1E8h, float(int)) and stores it at [esp+14h]; everything
    // after multiplies that value. Hooked right after the store: the background behind Sam as seen by the
    // guard is traced (ULevel virtual +DCh, SingleLineCheck(Hit, Source, &End, &Start, Flags, Extent, 0, 0),
    // Hit.Actor +4, Hit.Location +8). No hit within range, or a hit further than SkyDistance, counts as sky
    // (SkyBrightness); otherwise the engine's own light formula (ULevel::GetLuminosity) is evaluated at the
    // surface point with the level lights in range of it (see RefreshLevelLights). Sam's light-level virtual
    // can't be reused for this: called with 0 (as here) it just returns his cached LuminosityFactor (+264h), and
    // otherwise it samples four of his bones with only the lights touching him. If the background is brighter
    // than Sam, his light level rises by BacklightStrength x the difference: a dark silhouette against a bright
    // window, lit wall or the sky is visible. Parts of Sam the guard can't see (behind cover) don't count.
    bool g_backlight = true;
    float g_blStrength = 0.6f, g_sky = 40.0f, g_skyDist = 3000.0f, g_blRange = 6000.0f;

    using SingleLineCheck_t = int(__fastcall*)(void* level, void* edx, uint8_t* hit, void* source, const Vec* end, const Vec* start,
                                               uint32_t flags, float ex, float ey, float ez, int node, int extra);
    // ULevel virtual +D4h: GetLuminosity(FVector* point, TArray<ALight*>* lights, FColor* ambient, AActor* cacheActor).
    // Sums the given lights at the point (with shadow traces that ignore cacheActor) on top of the ambient.
    using GetLuminosity_t = float(__fastcall*)(void* level, void* edx, const Vec* point, void* lights, const uint32_t* ambient, void* cacheActor);

    struct BgCache { void* ctrl = nullptr; DWORD tick = 0; float bg = 0; DWORD seenTick = 0; unsigned seen = 0; };
    BgCache g_bgCache[32];

    bool g_blFailed = false;

    // Lights that could light the background. Sam's own light level only uses EPawn::RelevantLights (+1524h), the
    // lights the renderer found touching Sam, so a wall lit by a lamp that doesn't reach Sam would read as dark as
    // Sam. The background is measured with the level's lights instead, filtered the way the renderer fills
    // RelevantLights (Light.RelevantToAI, light set matching Sam's LightingSet) and by range (DiffuseRadius).
    // ULevel: Actors +28h (data) / +2Ch (count). Actor: bDeleteMe +28h bit 4, bIsLight +28h bit 8, LightingSet +4Eh.
    // Light: DiffuseRadius +2DCh, LightType +2E4h (0 = off), LightSet +2FDh, RelevantToAI +314h bit 2,
    // LightBrightness +318h.
    std::vector<void*> g_levelLights;
    void* g_levelLightsOf = nullptr;
    DWORD g_levelLightsTick = 0;
    const uint32_t* g_lightSetMasks = nullptr;   // per LightSet: bit mask of the actor LightingSets it affects

    void RefreshLevelLights(void* level)
    {
        const DWORD now = GetTickCount();
        if (level == g_levelLightsOf && now - g_levelLightsTick < 1000) return;
        g_levelLightsOf = level;
        g_levelLightsTick = now;
        g_levelLights.clear();
        void** actors = At<void**>(level, 0x28);
        const int n = At<int>(level, 0x2C);
        if (!actors || n <= 0 || n > 500000) return;
        for (int i = 0; i < n; ++i)
        {
            void* a = actors[i];
            if (!a) continue;
            const uint32_t f = At<uint32_t>(a, 0x28);
            if ((f & 4) || !(f & 8)) continue;                         // deleted, or not a light
            if (!(At<uint8_t>(a, 0x314) & 2) || !At<uint8_t>(a, 0x318)) continue;
            g_levelLights.push_back(a);
        }
    }

    // Kept free of C++ objects for __try. Sam is passed as the cache actor so the shadow traces ignore him; his
    // cached light level (LastPointCalculated +254h, LastValueCalculated +260h) is put back afterwards. Every
    // entry is a live light, so the engine never removes (reallocates) anything from the list.
    bool CallGetLuminosity(void* level, void* target, const Vec& point, void** list, int count, float& out)
    {
        struct { void** data; int num, max; } arr{ list, count, count };
        uint8_t saved[0x10];
        std::memcpy(saved, reinterpret_cast<uint8_t*>(target) + 0x254, sizeof(saved));
        At<float>(target, 0x260) = -1.0f;   // never answer from Sam's cache (same point twice in a row)
        bool ok = true;
        __try
        {
            const auto lum = reinterpret_cast<GetLuminosity_t>((*reinterpret_cast<void***>(level))[0xD4 / 4]);
            out = lum(level, nullptr, &point, &arr, &At<uint32_t>(target, 0x1520), target);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        std::memcpy(reinterpret_cast<uint8_t*>(target) + 0x254, saved, sizeof(saved));
        return ok;
    }

    constexpr int kMaxLights = 32;

    // The level lights that can reach the point, nearest first (at most kMaxLights). GetLuminosity only adds
    // lights with LightEffect (+2FCh) 2 (point) or 3+ (spot/projector); a point light fades to nothing at
    // 0.8 x DiffuseRadius.
    int GatherLights(void* target, const Vec& point, void** list)
    {
        static std::vector<std::pair<float, void*>> cand;
        cand.clear();
        const uint8_t set = At<uint8_t>(target, 0x4E);
        for (void* l : g_levelLights)
        {
            const uint32_t f = At<uint32_t>(l, 0x28);
            if ((f & 4) || !(f & 8) || !At<uint8_t>(l, 0x2E4) || !At<uint8_t>(l, 0x318) || At<uint8_t>(l, 0x2FC) < 2) continue;
            if (g_lightSetMasks && set < 32 && !(g_lightSetMasks[At<uint8_t>(l, 0x2FD)] & (1u << set))) continue;
            const float r = At<float>(l, 0x2DC);
            if (!(r > 1.0f)) continue;
            const Vec& p = At<Vec>(l, 0xE8);
            const float dx = p.x - point.x, dy = p.y - point.y, dz = p.z - point.z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < r * r) cand.emplace_back(d2, l);
        }
        std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        const int n = static_cast<int>(std::min<size_t>(cand.size(), kMaxLights));
        for (int i = 0; i < n; ++i) list[i] = cand[i].second;
        return n;
    }

    // Light level the engine's own formula gives at the point, from every relevant light in range.
    bool BackgroundLuminosity(void* level, void* target, const Vec& point, float& out, int& used)
    {
        void* list[kMaxLights];
        used = GatherLights(target, point, list);
        return CallGetLuminosity(level, target, point, list, used, out);
    }

    // SingleLineCheck returns 1 when the line is clear (GetLuminosity skips a light when it returns 0).
    int LineClear(void* level, void* source, const Vec& from, const Vec& to, uint32_t flags)
    {
        alignas(8) uint8_t buf[0x40] = {};
        *reinterpret_cast<float*>(buf + 0x24) = 1.0f;
        *reinterpret_cast<int*>(buf + 0x28) = -1;
        __try
        {
            const auto slc = reinterpret_cast<SingleLineCheck_t>((*reinterpret_cast<void***>(level))[0xDC / 4]);
            return slc(level, nullptr, buf, source, &to, &from, flags, 0, 0, 0, 0, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    // [Perception] LogPerception diagnostics, every 6 s: (1) at Sam's own position, the engine's light meter vs
    // GetLuminosity with Sam's own RelevantLights vs with the remaster's level-light list, which tells whether
    // the background measurement can be trusted; (2) the wall point's nearest lights one by one.
    void LogBacklightCheck(void* level, void* target, const Vec& wall, float meter, bool force = false)
    {
        static DWORD last = 0;
        const DWORD now = GetTickCount();
        if (!force && now - last < 6000) return;
        last = now;

        void* rel[64];
        int nrel = 0, relKnown = 0;
        void** relData = At<void**>(target, 0x1524);
        const int relNum = At<int>(target, 0x1528);
        for (int i = 0; relData && i < relNum && i < 256 && nrel < 64; ++i)
        {
            void* l = relData[i];
            if (!l || (At<uint32_t>(l, 0x28) & 4) || !(At<uint32_t>(l, 0x28) & 8)) continue;
            rel[nrel++] = l;
            if (std::find(g_levelLights.begin(), g_levelLights.end(), l) != g_levelLights.end()) ++relKnown;
        }
        const Vec loc = At<Vec>(target, 0xE8);
        float engineAt = -1, mineAt = -1;
        int mineN = 0;
        CallGetLuminosity(level, target, loc, rel, nrel, engineAt);
        BackgroundLuminosity(level, target, loc, mineAt, mineN);
        LOG("[PERCEPTION] check at sam (%.0f %.0f %.0f): light meter %.1f; engine with sam's %d lights %.1f; remaster with %d level "
            "lights %.1f; %d of sam's lights are in the remaster list (%d)",
            loc.x, loc.y, loc.z, meter, nrel, engineAt, mineN, mineAt, relKnown, static_cast<int>(g_levelLights.size()));

        void* list[kMaxLights];
        const int n = GatherLights(target, wall, list);
        float base = 0;
        CallGetLuminosity(level, target, wall, nullptr, 0, base);
        LOG("[PERCEPTION] check at wall (%.0f %.0f %.0f): ambient %.1f, %d lights in range, nearest:", wall.x, wall.y, wall.z, base, n);
        for (int i = 0; i < n && i < 8; ++i)
        {
            void* l = list[i];
            const Vec& lp = At<Vec>(l, 0xE8);
            const float dist = std::sqrt((lp.x - wall.x) * (lp.x - wall.x) + (lp.y - wall.y) * (lp.y - wall.y) + (lp.z - wall.z) * (lp.z - wall.z));
            float v = 0;
            CallGetLuminosity(level, target, wall, &list[i], 1, v);
            const bool samHas = std::find(rel, rel + nrel, l) != rel + nrel;
            LOG("[PERCEPTION]   light at (%.0f %.0f %.0f) %.0f away, radius %.0f, effect %d, type %d, brightness %d, set %d%s: "
                "line %s -> adds %.1f",
                lp.x, lp.y, lp.z, dist, At<float>(l, 0x2DC), At<uint8_t>(l, 0x2FC), At<uint8_t>(l, 0x2E4), At<uint8_t>(l, 0x318),
                At<uint8_t>(l, 0x2FD), samHas ? ", lights sam" : "",
                LineClear(level, target, lp, wall, 0x80220) == 1 ? "clear" : "blocked", v - base);
        }
    }

    bool TraceBackground(void* level, void* target, const Vec& start, const Vec& end, Vec& hitLoc, bool& hit)
    {
        alignas(8) uint8_t buf[0x40] = {};
        *reinterpret_cast<float*>(buf + 0x24) = 1.0f;
        *reinterpret_cast<int*>(buf + 0x28) = -1;
        __try
        {
            const auto slc = reinterpret_cast<SingleLineCheck_t>((*reinterpret_cast<void***>(level))[0xDC / 4]);
            slc(level, nullptr, buf, target, &end, &start, 0x14, 0, 0, 0, 0, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        hit = *reinterpret_cast<void**>(buf + 4) != nullptr;
        hitLoc = *reinterpret_cast<Vec*>(buf + 8);
        return true;
    }

    // Whether a viewer can see a point on Sam: the trace the guards' own line-of-sight test uses (pawn virtual
    // +1F4h, 0x10B7F6E0: from his view point, flags 80100h, the guard as source, the target passed along), seen when
    // it hits nothing, Sam, or something within 16 units of the point (the floor or step under a foot). Without a
    // viewer (F8) Sam is the source. 1 seen, 0 blocked, -1 failed.
    int CanSee(void* level, void* viewer, void* sam, const Vec& eye, const Vec& p)
    {
        alignas(8) uint8_t buf[0x40] = {};
        *reinterpret_cast<float*>(buf + 0x24) = 1.0f;
        *reinterpret_cast<int*>(buf + 0x28) = -1;
        __try
        {
            const auto slc = reinterpret_cast<SingleLineCheck_t>((*reinterpret_cast<void***>(level))[0xDC / 4]);
            slc(level, nullptr, buf, viewer ? viewer : sam, &p, &eye, 0x80100, 0, 0, 0, reinterpret_cast<int>(sam), 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        void* hit = *reinterpret_cast<void**>(buf + 4);
        if (!hit || hit == sam) return 1;
        const Vec& h = *reinterpret_cast<const Vec*>(buf + 8);
        const float dx = h.x - p.x, dy = h.y - p.y, dz = h.z - p.z;
        return dx * dx + dy * dy + dz * dz <= 16.0f * 16.0f ? 1 : 0;
    }

    // The background is sampled with five lines from the guard's eye through Sam's silhouette (head, middle,
    // legs, left and right edge) and on past him: a bright patch behind any part of him (a window, the fire in a
    // hearth, a lit doorway) outlines that part. Each line's background counts as sky when nothing is hit within
    // range or the hit is further than SkyDistance, otherwise it is measured with BackgroundLuminosity. The
    // result is the average of the five lines (hidden parts count as no outline), so one bright patch beside him
    // counts for a fifth of a fully bright background.
    struct BgSample { float lum = 0, dist = 0; int lights = 0; bool sky = false, source = false, hidden = false; };
    struct BgResult { float bg = 0; int n = 0, skyCount = 0; BgSample s[5]; };

    // A light source right behind Sam in the guard's view (a bulb, the fire in a hearth) outlines him even when
    // the surface behind it is unlit to the engine (the fire light's line to the back of the hearth is blocked by
    // the logs). A level light within kSourceRadius of the line, beyond Sam and no further than the background
    // surface, with a clear line from Sam to it (a hit within kSourceRadius of the light counts as its own fixture),
    // counts as its LightBrightness.
    constexpr float kSourceRadius = 64.0f;
    float LightSourceBehind(void* level, void* target, const Vec& p, const Vec& d, float maxT)
    {
        const uint8_t set = At<uint8_t>(target, 0x4E);
        float best = 0;
        for (void* l : g_levelLights)
        {
            const uint32_t f = At<uint32_t>(l, 0x28);
            if ((f & 4) || !(f & 8) || !At<uint8_t>(l, 0x2E4) || At<uint8_t>(l, 0x2FC) < 2) continue;
            const float b = At<uint8_t>(l, 0x318);
            if (b <= best) continue;
            if (g_lightSetMasks && set < 32 && !(g_lightSetMasks[At<uint8_t>(l, 0x2FD)] & (1u << set))) continue;
            const Vec& L = At<Vec>(l, 0xE8);
            const Vec v{ L.x - p.x, L.y - p.y, L.z - p.z };
            const float t = v.x * d.x + v.y * d.y + v.z * d.z;
            if (t < 40.0f || t > maxT + kSourceRadius) continue;
            const Vec q{ v.x - d.x * t, v.y - d.y * t, v.z - d.z * t };
            if (q.x * q.x + q.y * q.y + q.z * q.z > kSourceRadius * kSourceRadius) continue;
            Vec hitLoc{};
            bool hit = false;
            const Vec start{ p.x + d.x * 40, p.y + d.y * 40, p.z + d.z * 40 };
            if (!TraceBackground(level, target, start, L, hitLoc, hit)) continue;
            if (hit)
            {
                const float hx = hitLoc.x - L.x, hy = hitLoc.y - L.y, hz = hitLoc.z - L.z;
                if (hx * hx + hy * hy + hz * hz > kSourceRadius * kSourceRadius) continue;
            }
            best = b;
        }
        return best;
    }

    bool MeasureBackground(void* level, void* target, void* viewer, const Vec& eye, BgResult& res)
    {
        const Vec c = At<Vec>(target, 0xE8);
        const float h = std::clamp(At<float>(target, 0x224), 20.0f, 200.0f);   // CollisionHeight (half height)
        const float rad = std::clamp(At<float>(target, 0x220), 10.0f, 100.0f);  // CollisionRadius
        Vec side{ -(c.y - eye.y), c.x - eye.x, 0 };
        const float sl = std::sqrt(side.x * side.x + side.y * side.y);
        side = sl > 1.0f ? Vec{ side.x / sl, side.y / sl, 0 } : Vec{ 1, 0, 0 };
        const float e = rad * 1.2f;
        const Vec pts[5] = { { c.x, c.y, c.z + 0.8f * h }, c, { c.x, c.y, c.z - 0.6f * h },
                             { c.x + side.x * e, c.y + side.y * e, c.z }, { c.x - side.x * e, c.y - side.y * e, c.z } };
        RefreshLevelLights(level);
        float sum = 0;
        res = {};
        for (const Vec& p : pts)
        {
            BgSample& s = res.s[res.n++];
            Vec d{ p.x - eye.x, p.y - eye.y, p.z - eye.z };
            const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (len < 1.0f)
            {
                s.hidden = true;
                continue;
            }
            d = { d.x / len, d.y / len, d.z / len };
            // Only the parts of Sam the guard can actually see are outlined: a part behind cover (the crate or the
            // trash bags he crouches behind) shows the guard the cover, not a silhouette.
            const int seen = CanSee(level, viewer, target, eye, p);
            if (seen < 0) return false;
            if (!seen)
            {
                s.hidden = true;
                continue;
            }
            Vec hitLoc{};
            bool hit = false;
            const Vec start{ p.x + d.x * 40, p.y + d.y * 40, p.z + d.z * 40 };
            const Vec end{ p.x + d.x * g_blRange, p.y + d.y * g_blRange, p.z + d.z * g_blRange };
            if (!TraceBackground(level, target, start, end, hitLoc, hit)) return false;
            s.dist = hit ? std::sqrt((hitLoc.x - p.x) * (hitLoc.x - p.x) + (hitLoc.y - p.y) * (hitLoc.y - p.y) +
                                     (hitLoc.z - p.z) * (hitLoc.z - p.z)) : 0.0f;
            if (!hit || s.dist >= g_skyDist)
            {
                s.sky = true;
                s.lum = g_sky;
                ++res.skyCount;
            }
            else
            {
                const Vec wp{ hitLoc.x - d.x * 16, hitLoc.y - d.y * 16, hitLoc.z - d.z * 16 };
                if (!BackgroundLuminosity(level, target, wp, s.lum, s.lights)) return false;
            }
            const float src = LightSourceBehind(level, target, p, d, s.sky ? g_blRange : s.dist);
            if (src > s.lum)
            {
                s.lum = src;
                s.source = true;
            }
            sum += s.lum;
        }
        // How much of his outline stands against the light: the average over all five lines (a hidden part, or a
        // dark patch behind it, outlines nothing).
        res.bg = res.n ? sum / res.n : 0.0f;
        return true;
    }

    void FormatSamples(const BgResult& res, char* out, size_t size)
    {
        out[0] = 0;
        static const char* names[5] = { "head", "middle", "legs", "left", "right" };
        size_t used = 0;
        for (int i = 0; i < res.n && used + 1 < size; ++i)
        {
            const BgSample& s = res.s[i];
            const int w = s.hidden ? snprintf(out + used, size - used, "%s%s hidden from him", i ? ", " : "", names[i])
                        : s.source ? snprintf(out + used, size - used, "%s%s %.0f (light source behind him)", i ? ", " : "", names[i], s.lum)
                        : s.sky    ? snprintf(out + used, size - used, "%s%s sky %.0f", i ? ", " : "", names[i], s.lum)
                                   : snprintf(out + used, size - used, "%s%s %.0f (%.1f m, %d lights)", i ? ", " : "", names[i], s.lum,
                                              s.dist / 100.0f, s.lights);
            if (w < 0) break;
            used += static_cast<size_t>(w);
        }
    }

    // ---- Sam's light as each guard sees it -------------------------------------------------------------------
    // Sam's light level (LuminosityFactor +264h) is kept by 0x10B7F0D0: Sam has a list of bones (EPawn +1514h,
    // count +1518h, entries { FName bone, float light }); each update measures one of them in turn (position from
    // 0x10A83AC0, GetLuminosity with every light in RelevantLights +1524h, from all sides) and the level is the
    // average over the list. The same bones are measured here every 200 ms, split into the ambient part and each
    // relevant light's part (GetLuminosity with that light alone). For each guard:
    //  - DirectionalLighting: a light's part is weighted by how much of the side of Sam it lights faces the guard:
    //    the Lambert phase ((pi - a) cos a + sin a) / pi of the angle a between the light and the guard as seen
    //    from Sam (1 with the light behind the guard, 0.32 from the side, 0 behind Sam), never below DarkSide.
    //  - PartialCover: a bone the guard can't see (the guards' sight line check) counts nothing, so a lit foot
    //    sticking out behind the cover he is looking at doesn't light Sam up for him.
    //  - SpotlightBeam: GetLuminosity lights a spotlight's whole frame (LightEffect 3+: a frustum from the light's
    //    Rotation +F4h, width +2D0h, height +2D4h at near +2D8h) right up to its edges unless a projected texture
    //    shapes it, while on screen the beam is round and fades out long before that edge: the Penthouse alley work
    //    lamp gave Sam 110 where the screen was dark, and 0 just 75 cm further. A spotlight's part is scaled by its
    //    beam: 1 within BeamCore of the narrower half-angle of the frame, fading to 0 at its edge.
    // The engine's own value is scaled by (what this guard sees) / (all bones, all lights from all sides, no beam),
    // so a guard on the lit side with a clear view of Sam in the middle of a beam sees exactly the original level.
    bool g_directional = true, g_dirFailed = false, g_cover = true, g_beam = true;
    float g_darkSide = 0.25f, g_beamCore = 0.4f;

    // Spotlight beam factor of light 'l' at point 'p' (1 for other lights).
    float BeamFactor(void* l, const Vec& p)
    {
        if (!g_beam || At<uint8_t>(l, 0x2FC) < 3) return 1.0f;
        const float n = At<float>(l, 0x2D8), w = At<float>(l, 0x2D0), h = At<float>(l, 0x2D4);
        if (!(n > 0.01f) || !(w > 0.01f) || !(h > 0.01f)) return 1.0f;
        const float tanHalf = std::min(w, h) * 0.5f / n;
        const int* rot = &At<int>(l, 0xF4);
        constexpr float k = 3.14159265f / 32768.0f;
        const float pitch = static_cast<float>(static_cast<int16_t>(rot[0] & 0xFFFF)) * k;
        const float yaw = static_cast<float>(rot[1] & 0xFFFF) * k;
        const Vec f{ std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch) };
        const Vec& L = At<Vec>(l, 0xE8);
        const Vec d{ p.x - L.x, p.y - L.y, p.z - L.z };
        const float depth = d.x * f.x + d.y * f.y + d.z * f.z;
        if (depth <= 1.0f) return 0.0f;   // behind it (GetLuminosity gives nothing there either)
        const float lat = std::sqrt(std::max(0.0f, d.x * d.x + d.y * d.y + d.z * d.z - depth * depth));
        const float r = lat / depth / tanHalf;   // 1 = the edge of the frame
        if (r <= g_beamCore) return 1.0f;
        if (r >= 1.0f) return 0.0f;
        const float x = (r - g_beamCore) / (1.0f - g_beamCore);
        return 1.0f - x * x * (3.0f - 2.0f * x);
    }

    constexpr int kPoints = 8;
    struct SamLight { Vec pos; float part[kPoints]; float beam[kPoints]; float sum; float beamAvg; };
    struct SamLights
    {
        void* sam = nullptr;
        DWORD tick = 0;
        int np = 0, n = 0;           // points measured (Sam's light bones, or middle and chest without them), lights
        bool bones = false;
        Vec pt[kPoints]{};
        float base[kPoints]{};
        float total = 0;             // the average over the points, all lights from all sides (~ the engine's value)
        SamLight l[16];
    };
    SamLights g_samLights;

    using BoneLoc_t = void*(__fastcall*)(void* pawn, void* edx, uint32_t name, Vec* out, int flag);
    BoneLoc_t g_boneLoc = nullptr;
    uint32_t g_boneListOff = 0, g_boneCountOff = 0;

    // The bones the engine measures Sam's light at (the first kPoints of his list). Kept free of C++ objects for
    // __try.
    bool SamBonePoints(void* sam, Vec* pts, int& np)
    {
        np = 0;
        if (!g_boneLoc || !g_boneListOff) return false;
        const uint8_t* list = At<const uint8_t*>(sam, g_boneListOff);
        const int count = At<int>(sam, g_boneCountOff);
        if (!list || count <= 0) return false;
        const int n = std::min(count, kPoints);
        const Vec c = At<Vec>(sam, 0xE8);
        __try
        {
            for (int i = 0; i < n; ++i)
            {
                pts[i] = c;   // left as is without a mesh
                g_boneLoc(sam, nullptr, *reinterpret_cast<const uint32_t*>(list + i * 8), &pts[i], 0);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        np = n;
        return true;
    }

    bool UpdateSamLights(void* level, void* sam)
    {
        const DWORD now = GetTickCount();
        SamLights& s = g_samLights;
        if (s.sam == sam && now - s.tick < 200) return true;
        s.sam = sam;
        s.tick = now;
        s.n = s.np = 0;
        s.total = 0;
        void* rel[16];
        int n = 0;
        void** data = At<void**>(sam, 0x1524);
        const int num = At<int>(sam, 0x1528);
        for (int i = 0; data && i < num && i < 256 && n < 16; ++i)
        {
            void* l = data[i];
            if (!l || (At<uint32_t>(l, 0x28) & 4) || !(At<uint32_t>(l, 0x28) & 8)) continue;
            s.l[n] = { At<Vec>(l, 0xE8), {}, {}, 0.0f, 0.0f };
            rel[n++] = l;
        }
        Vec pts[kPoints];
        int np = 0;
        s.bones = SamBonePoints(sam, pts, np);
        if (!s.bones)
        {
            const Vec c = At<Vec>(sam, 0xE8);
            const float h = std::clamp(At<float>(sam, 0x224), 20.0f, 200.0f);
            pts[0] = c;
            pts[1] = { c.x, c.y, c.z + 0.5f * h };
            np = 2;
        }
        float sum = 0;
        for (int b = 0; b < np; ++b)
        {
            s.pt[b] = pts[b];
            float base = 0;
            if (!CallGetLuminosity(level, sam, pts[b], nullptr, 0, base)) return false;
            s.base[b] = base;
            sum += base;
            for (int i = 0; i < n; ++i)
            {
                float v = 0;
                if (!CallGetLuminosity(level, sam, pts[b], &rel[i], 1, v)) return false;
                s.l[i].part[b] = std::max(0.0f, v - base);
                s.l[i].beam[b] = BeamFactor(rel[i], pts[b]);
                s.l[i].sum += s.l[i].part[b] / np;
                s.l[i].beamAvg += s.l[i].beam[b] / np;
                sum += s.l[i].part[b];
            }
        }
        s.n = n;
        s.np = np;
        s.total = sum / np;
        return true;
    }

    // Per relevant light, its weight for a viewer at 'eye' (DirectionalLighting) and the angle in degrees.
    void LightWeights(const Vec& c, const Vec& eye, float* weight, float* angle)
    {
        const SamLights& s = g_samLights;
        Vec g{ eye.x - c.x, eye.y - c.y, eye.z - c.z };
        const float gl = std::sqrt(g.x * g.x + g.y * g.y + g.z * g.z);
        constexpr float kPi = 3.14159265f;
        for (int i = 0; i < s.n; ++i)
        {
            weight[i] = 1.0f;
            angle[i] = 0.0f;
            const Vec l{ s.l[i].pos.x - c.x, s.l[i].pos.y - c.y, s.l[i].pos.z - c.z };
            const float ll = std::sqrt(l.x * l.x + l.y * l.y + l.z * l.z);
            if (ll < 1.0f || gl < 1.0f) continue;
            const float ca = std::clamp((l.x * g.x + l.y * g.y + l.z * g.z) / (ll * gl), -1.0f, 1.0f);
            const float a = std::acos(ca);
            angle[i] = a * (180.0f / kPi);
            if (!g_directional) continue;
            const float f = ((kPi - a) * ca + std::sin(a)) / kPi;
            weight[i] = g_darkSide + (1.0f - g_darkSide) * std::clamp(f, 0.0f, 1.0f);
        }
    }

    // Fraction of Sam's light level a viewer at 'eye' sees (1 = all of it); 'seen' bit b = point b visible to him.
    float ViewerFactor(const Vec& c, const Vec& eye, unsigned seen)
    {
        const SamLights& s = g_samLights;
        if (s.total < 0.5f || !s.np) return 1.0f;
        float weight[16], angle[16];
        LightWeights(c, eye, weight, angle);
        float lit = 0;
        for (int b = 0; b < s.np; ++b)
        {
            if (!(seen & (1u << b))) continue;
            lit += s.base[b];
            for (int i = 0; i < s.n; ++i) lit += s.l[i].part[b] * s.l[i].beam[b] * weight[i];
        }
        return std::clamp(lit / s.np / s.total, 0.0f, 1.0f);
    }

    // Which of the sample points a viewer can see (all of them without PartialCover).
    unsigned PointsSeen(void* level, void* viewer, void* sam, const Vec& eye)
    {
        const SamLights& s = g_samLights;
        unsigned seen = (1u << s.np) - 1;
        if (!g_cover) return seen;
        for (int b = 0; b < s.np; ++b)
            if (CanSee(level, viewer, sam, eye, s.pt[b]) == 0) seen &= ~(1u << b);
        return seen;
    }

    void FormatLights(const Vec& c, const Vec& eye, unsigned seen, char* out, size_t size)
    {
        out[0] = 0;
        const SamLights& s = g_samLights;
        static const char* plainNames[2] = { "middle", "chest" };
        int used = 0;
        for (int b = 0; b < s.np && used >= 0 && static_cast<size_t>(used) + 1 < size; ++b)
        {
            float v = s.base[b];
            for (int i = 0; i < s.n; ++i) v += s.l[i].part[b];
            const int w = s.bones
                ? snprintf(out + used, size - used, "%sbone%d %.0f%s", b ? ", " : "", b + 1, v, (seen & (1u << b)) ? "" : " (hidden)")
                : snprintf(out + used, size - used, "%s%s %.0f%s", b ? ", " : "", plainNames[b], v, (seen & (1u << b)) ? "" : " (hidden)");
            if (w < 0) break;
            used += w;
        }
        float weight[16], angle[16];
        LightWeights(c, eye, weight, angle);
        for (int i = 0; i < s.n && used >= 0 && static_cast<size_t>(used) + 1 < size; ++i)
        {
            const int w = s.l[i].beamAvg < 0.999f
                ? snprintf(out + used, size - used, "%s light %.1f at %.0f deg, beam x%.2f", i ? "," : " |", s.l[i].sum, angle[i], s.l[i].beamAvg)
                : snprintf(out + used, size - used, "%s light %.1f at %.0f deg", i ? "," : " |", s.l[i].sum, angle[i]);
            if (w < 0) break;
            used += w;
        }
    }

    // Applies directional lighting and partial cover, then backlighting, to the light level a viewer (a guard's
    // pawn, or none for F8) at 'eye' gets for Sam. A failed measurement switches that feature off (logged).
    // 'cache' (per guard) keeps the background and the visible points for 250 ms.
    struct SightLight { float meter = 0, facing = 1, lit = 0, final = 0; unsigned seen = ~0u; bool bgValid = false; BgResult bg; };

    void ComputeSightLight(void* level, void* sam, void* viewer, const Vec& eye, float meter, SightLight& out, BgCache* cache)
    {
        out.meter = meter;
        out.facing = 1.0f;
        if ((g_directional || g_cover || g_beam) && !g_dirFailed)
        {
            if (UpdateSamLights(level, sam))
            {
                const DWORD now = GetTickCount();
                if (cache && cache->seenTick && now - cache->seenTick <= 250)
                    out.seen = cache->seen;
                else
                {
                    out.seen = PointsSeen(level, viewer, sam, eye);
                    if (cache)
                    {
                        cache->seenTick = now;
                        cache->seen = out.seen;
                    }
                }
                out.facing = ViewerFactor(At<Vec>(sam, 0xE8), eye, out.seen);
            }
            else
            {
                g_dirFailed = true;
                LOG("Perception: light breakdown failed; directional lighting and partial cover disabled");
            }
        }
        out.lit = meter * out.facing;
        out.final = out.lit;
        if (!g_backlight || g_blFailed) return;
        float bg = 0;
        const DWORD now = GetTickCount();
        if (cache && cache->tick && now - cache->tick <= 250)
            bg = cache->bg;
        else
        {
            if (!MeasureBackground(level, sam, viewer, eye, out.bg))
            {
                g_blFailed = true;
                LOG("Perception: background measurement failed; backlighting disabled");
                return;
            }
            out.bgValid = true;
            bg = out.bg.bg;
            if (cache)
            {
                cache->tick = now;
                cache->bg = bg;
            }
        }
        if (bg > out.final) out.final += g_blStrength * (bg - out.final);
    }

    void __cdecl OnTargetLuminosity(Mem::Regs* r)
    {
        if ((!g_backlight || g_blFailed) && ((!g_directional && !g_cover && !g_beam) || g_dirFailed)) return;
        void* ctrl = reinterpret_cast<void*>(r->esi);
        void* target = reinterpret_cast<void*>(r->edi);
        if (!ctrl || !target || !(At<uint32_t>(target, 0x2C) & 0x20)) return;   // player targets only
        float& L = r->Stack<float>(0x14);
        void* pawn = At<void*>(ctrl, 0x41C);
        void* level = At<void*>(target, 0x7C);
        if (!pawn || !level) return;

        Vec eye{}, viewDir{};
        using ViewPoint_t = void(__fastcall*)(void* pawn, void* edx, Vec* loc, Vec* dir, int flag);
        reinterpret_cast<ViewPoint_t>((*reinterpret_cast<void***>(pawn))[0x304 / 4])(pawn, nullptr, &eye, &viewDir, 0);

        BgCache* c = &g_bgCache[(reinterpret_cast<uintptr_t>(ctrl) >> 4) & 31];
        if (c->ctrl != ctrl) *c = { ctrl, 0, 0, 0, 0 };
        SightLight sl;
        ComputeSightLight(level, target, pawn, eye, L, sl, c);
        L = sl.final;

        static BgResult lastBg;
        if (sl.bgValid) lastBg = sl.bg;
        if (g_log)
        {
            static DWORD lastLog = 0;
            const DWORD now = GetTickCount();
            if (now - lastLog >= 2000)
            {
                lastLog = now;
                char lights[512], bg[512];
                const Vec& sp = At<Vec>(target, 0xE8);
                const Vec& gp = At<Vec>(pawn, 0xE8);
                FormatLights(sp, eye, sl.seen, lights, sizeof(lights));
                FormatSamples(lastBg, bg, sizeof(bg));
                LOG("[PERCEPTION] sight: Sam at (%.0f %.0f %.0f), guard %.1f m away: meter %.1f [%s], he sees x%.2f -> %.1f; "
                    "background %.1f [%s] -> sam counts as %.1f",
                    sp.x, sp.y, sp.z, std::sqrt((sp.x - gp.x) * (sp.x - gp.x) + (sp.y - gp.y) * (sp.y - gp.y) + (sp.z - gp.z) * (sp.z - gp.z)) / 100.0f,
                    sl.meter, lights, sl.facing, sl.lit, c->bg, bg, sl.final);
            }
        }
    }

    // F8 with LogPerception = 1: the backlight check as a guard standing at the camera would see it, so it can be
    // tested anywhere without a guard looking. The eye is placed 3 m behind Sam along the player controller's
    // view Rotation (+F4h); Sam's light level is his cached LuminosityFactor (+264h), the value the sight check
    // uses. Also logs the self-check and the middle line's lights one by one.
    void CameraBacklightCheck(void* sam)
    {
        void* pc = At<void*>(sam, 0x2B4);
        void* level = At<void*>(sam, 0x7C);
        if (!pc || !level) return;
        const int* rot = &At<int>(pc, 0xF4);
        constexpr float kToRad = 3.14159265f / 32768.0f;
        const float p = static_cast<float>(static_cast<int16_t>(rot[0] & 0xFFFF)) * kToRad;
        const float y = static_cast<float>(rot[1] & 0xFFFF) * kToRad;
        const Vec d{ std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p) };
        const Vec tp = At<Vec>(sam, 0xE8);
        const Vec eye{ tp.x - d.x * 300, tp.y - d.y * 300, tp.z - d.z * 300 };
        const float meter = At<float>(sam, 0x264);
        SightLight sl;
        ComputeSightLight(level, sam, nullptr, eye, meter, sl, nullptr);
        char lights[512], bg[512];
        FormatLights(tp, eye, sl.seen, lights, sizeof(lights));
        if (sl.bgValid) FormatSamples(sl.bg, bg, sizeof(bg));
        else snprintf(bg, sizeof(bg), "off");
        LOG("[PERCEPTION] F8 check (a guard at your camera): meter %.1f [%s], he sees x%.2f -> %.1f; background %.1f [%s] -> sam counts as %.1f",
            meter, lights, sl.facing, sl.lit, sl.bgValid ? sl.bg.bg : 0.0f, bg, sl.final);
        if (!g_backlight || g_blFailed) return;

        // the middle line's wall point, for the light-by-light breakdown
        const Vec start{ tp.x + d.x * 40, tp.y + d.y * 40, tp.z + d.z * 40 };
        const Vec end{ tp.x + d.x * g_blRange, tp.y + d.y * g_blRange, tp.z + d.z * g_blRange };
        Vec hitLoc{};
        bool hit = false;
        if (TraceBackground(level, sam, start, end, hitLoc, hit) && hit)
            LogBacklightCheck(level, sam, { hitLoc.x - d.x * 16, hitLoc.y - d.y * 16, hitLoc.z - d.z * 16 }, meter, true);
    }

    // ---- F8 character list -----------------------------------------------------------------------------------
    // Every character (Actor.bIsPawn +2Ch bit 10h) in the level, nearest to the centre of the view first: name,
    // personality, position, distance and height relative to Sam, angle from the camera's view direction, and
    // whether and how it is drawn: bHidden (+30h bit 80h), DrawType (+4Dh), Mesh (+184h), and LastRenderTime
    // (+50h) compared with Sam's (Sam is drawn every frame, so "0.0 s ago" = on screen right now).
    bool SafeFullName(void* obj, wchar_t* buf)
    {
        buf[0] = 0;
        __try { Game::GetFullName(obj, nullptr, buf); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = 0; return false; }
    }

    void LogCharacters(void* sam)
    {
        void* level = At<void*>(sam, 0x7C);
        void* pc = At<void*>(sam, 0x2B4);
        if (!level || !pc || !Game::GetFullName) return;
        const int* rot = &At<int>(pc, 0xF4);
        constexpr float kToRad = 3.14159265f / 32768.0f;
        const float pitch = static_cast<float>(static_cast<int16_t>(rot[0] & 0xFFFF)) * kToRad;
        const float yaw = static_cast<float>(rot[1] & 0xFFFF) * kToRad;
        const Vec d{ std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch) };
        const Vec s = At<Vec>(sam, 0xE8);
        const float samDrawn = At<float>(sam, 0x50);
        void** actors = At<void**>(level, 0x28);
        const int n = At<int>(level, 0x2C);
        if (!actors || n <= 0 || n > 500000) return;
        struct Row { float ang; void* a; float dist; };
        std::vector<Row> rows;
        for (int i = 0; i < n; ++i)
        {
            void* a = actors[i];
            if (!a || a == sam || (At<uint32_t>(a, 0x28) & 4) || !(At<uint32_t>(a, 0x2C) & 0x10)) continue;
            const Vec& p = At<Vec>(a, 0xE8);
            const Vec v{ p.x - s.x, p.y - s.y, p.z - s.z };
            const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            const float c = len > 1.0f ? std::clamp((v.x * d.x + v.y * d.y + v.z * d.z) / len, -1.0f, 1.0f) : 1.0f;
            rows.push_back({ std::acos(c) * (180.0f / 3.14159265f), a, len });
        }
        std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.ang < y.ang; });
        LOG("[CHARS] F8 at sam (%.0f %.0f %.0f), camera pitch %.0f yaw %.0f: %d characters, nearest to the centre of the view first",
            s.x, s.y, s.z, pitch * 57.2958f, yaw * 57.2958f, static_cast<int>(rows.size()));
        int shown = 0;
        for (const Row& r : rows)
        {
            if (++shown > 40) break;
            wchar_t name[256];
            SafeFullName(r.a, name);
            const wchar_t* label = Guards::Label(r.a);
            const Vec& p = At<Vec>(r.a, 0xE8);
            const bool hidden = (At<uint32_t>(r.a, 0x30) & 0x80) != 0;
            const float ago = samDrawn - At<float>(r.a, 0x50);
            LOG("[CHARS] %5.1f deg off view | %6.1f m away, %+6.1f m height | %ls%s%ls | at (%.0f %.0f %.0f) | %s, draw type %d, %s, "
                "last drawn %.1f s ago | physics %d, health %d, %s",
                r.ang, r.dist / 100.0f, (p.z - s.z) / 100.0f, name, label ? " = " : "", label ? label : L"", p.x, p.y, p.z,
                hidden ? "HIDDEN" : "visible", At<uint8_t>(r.a, 0x4D), At<void*>(r.a, 0x184) ? "has a mesh" : "no mesh", ago,
                At<uint8_t>(r.a, 0x4C), At<int>(r.a, 0x420), At<void*>(r.a, 0x2B4) ? "controlled" : "no controller");
        }
    }

    // ---- Shadow sense ----------------------------------------------------------------------------------------
    // EAIController shadow check (0x10BAEEA0, called once per AI tick from 0x10BBB320): picks a random target in
    // the perception table and a random point on it, steps round-robin (controller +418h) through the AI
    // manager's ShadowDetectLights (+2F0h); if that light reaches the point it traces on to where the shadow
    // lands, and if the guard's pawn can see that spot (pawn virtuals +1ECh view cone, +1F4h line of sight) it
    // posts an AI event of type 3Ah (SEE_ENEMY_SHADOW). Nothing in it is left to chance, so a guard's shadow sense
    // (Guards::ShadowSense, the Shadow trait in the guards ini) sets how often he runs it: 0 never, 0.5 every
    // other tick, 2 twice a tick (at most 3).
    using ShadowCheck_t = void(__fastcall*)(void* ctrl, void* edx);
    ShadowCheck_t oShadowCheck = nullptr;
    std::unordered_map<void*, float> g_shadowAcc;

    void __fastcall hkShadowCheck(void* ctrl, void* edx)
    {
        void* pawn = ctrl ? At<void*>(ctrl, 0x41C) : nullptr;
        const float f = pawn ? Guards::ShadowSense(pawn) : 1.0f;
        if (f == 1.0f)
        {
            oShadowCheck(ctrl, edx);
            return;
        }
        if (g_shadowAcc.size() > 1024) g_shadowAcc.clear();
        float& acc = g_shadowAcc[ctrl];
        acc = std::min(acc + f, 3.0f);
        while (acc >= 1.0f)
        {
            acc -= 1.0f;
            oShadowCheck(ctrl, edx);
        }
    }

    // The extra shadow lights (ShadowDetection = 1) had no limits: the original check only asks whether the guard
    // could see the spot where the shadow lands, so a guard 18 m down an alley noticed a shadow on a wall. For
    // those lights (not the designers' own bNPCShadowDetect lights) a sighting now also needs the shadow within
    // ShadowSightRange of the guard, and the light to light the spot where it falls by at least ShadowMinLight
    // (same scale as the light meter; a faint lamp at the edge of its range casts no shadow anyone would notice).
    // Hooked after the guard's line-of-sight test on the spot (call [edx+1F4h]; test eax,eax; jz skip): eax = 0
    // drops it. ebx = target, esi = light, ebp = controller, [esp+24h] = the shadow spot, [esp+54h] = the spot
    // lifted 10 units off the surface (what the line of sight is tested against).
    float g_shadowRange = 1000.0f, g_shadowMinLight = 40.0f;
    float g_shadowLum = -1.0f;   // light at the spot of the sighting being posted, for OnShadowSeen's log

    void __cdecl OnShadowVisible(Mem::Regs* r)
    {
        g_shadowLum = -1.0f;
        if (!r->eax) return;
        void* ctrl = reinterpret_cast<void*>(r->ebp);
        void* light = reinterpret_cast<void*>(r->esi);
        void* target = reinterpret_cast<void*>(r->ebx);
        if (!ctrl || !light || !target || (At<uint8_t>(light, 0x360) & 2)) return;
        void* pawn = At<void*>(ctrl, 0x41C);
        if (!pawn) return;
        const Vec spot = r->Stack<Vec>(0x24);
        const Vec& gp = At<Vec>(pawn, 0xE8);
        const float dist = std::sqrt((spot.x - gp.x) * (spot.x - gp.x) + (spot.y - gp.y) * (spot.y - gp.y) + (spot.z - gp.z) * (spot.z - gp.z));
        const char* why = nullptr;
        if (g_shadowRange > 0 && dist > g_shadowRange)
            why = "too far away";
        // Player targets only (the luminosity call uses the pawn's ambient and light cache); point and spot lights
        // only (GetLuminosity ignores the other kinds, which would read as no light at all).
        else if (g_shadowMinLight > 0 && (At<uint32_t>(target, 0x2C) & 0x20) && At<uint8_t>(light, 0x2FC) >= 2)
        {
            void* level = At<void*>(target, 0x7C);
            void* list[1] = { light };
            float lum = 0, base = 0;
            if (level && CallGetLuminosity(level, target, r->Stack<Vec>(0x54), list, 1, lum) &&
                CallGetLuminosity(level, target, r->Stack<Vec>(0x54), list, 0, base))
            {
                g_shadowLum = std::max(lum - base, 0.0f) * BeamFactor(light, r->Stack<Vec>(0x54));
                if (g_shadowLum < g_shadowMinLight) why = "too faint";
            }
        }
        if (!why) return;
        r->eax = 0;
        if (!g_log) return;
        static std::unordered_map<void*, DWORD> last;
        if (last.size() > 1024) last.clear();
        const DWORD now = GetTickCount();
        DWORD& t = last[ctrl];
        if (t && now - t < 2000) return;
        t = now;
        const wchar_t* label = Guards::Label(pawn);
        const Vec& lp = At<Vec>(light, 0xE8);
        if (g_shadowLum >= 0)
            LOG("[PERCEPTION] guard (%ls) could see Sam's shadow %.1f m away, cast by the light at (%.0f %.0f %.0f), but it is %s "
                "(light %.1f where it falls, needs %.0f): ignored",
                label ? label : L"no personality", dist / 100.0f, lp.x, lp.y, lp.z, why, g_shadowLum, g_shadowMinLight);
        else
            LOG("[PERCEPTION] guard (%ls) could see Sam's shadow %.1f m away, cast by the light at (%.0f %.0f %.0f), but it is %s "
                "(range %.0f m): ignored",
                label ? label : L"no personality", dist / 100.0f, lp.x, lp.y, lp.z, why, g_shadowRange / 100.0f);
    }

    // LogPerception: logs each shadow sighting (at most every 2 s per guard), from the 'mov byte [esp+98h], 3Ah'
    // that sets the event type. ebp = controller, esi = the light, [esp+24h] = where the shadow is.
    void __cdecl OnShadowSeen(Mem::Regs* r)
    {
        if (!g_log) return;
        void* ctrl = reinterpret_cast<void*>(r->ebp);
        void* light = reinterpret_cast<void*>(r->esi);
        if (!ctrl || !light) return;
        static std::unordered_map<void*, DWORD> last;
        if (last.size() > 1024) last.clear();
        const DWORD now = GetTickCount();
        DWORD& t = last[ctrl];
        if (t && now - t < 2000) return;
        t = now;
        void* pawn = At<void*>(ctrl, 0x41C);
        if (!pawn) return;
        const wchar_t* label = Guards::Label(pawn);
        const Vec sh = r->Stack<Vec>(0x24);
        const Vec& lp = At<Vec>(light, 0xE8);
        const Vec& gp = At<Vec>(pawn, 0xE8);
        const float dist = std::sqrt((sh.x - gp.x) * (sh.x - gp.x) + (sh.y - gp.y) * (sh.y - gp.y) + (sh.z - gp.z) * (sh.z - gp.z));
        char lum[48] = "";
        if (g_shadowLum >= 0) snprintf(lum, sizeof(lum), ", light %.1f where it falls", g_shadowLum);
        LOG("[PERCEPTION] guard (%ls, shadow sense x%.2f) saw Sam's shadow %.1f m away, cast by the light at (%.0f %.0f %.0f)%s%s",
            label ? label : L"no personality", Guards::ShadowSense(pawn), dist / 100.0f, lp.x, lp.y, lp.z,
            (At<uint8_t>(light, 0x360) & 2) ? " (an original shadow-detect light)" : " (one of the extra shadow lights)", lum);
    }

    // ---- AI diagnostics (LogPerception): F7 report and the stuck/frozen watch --------------------------------
    // Object names via GetFullName; a script state is UObject.StateFrame (+0Ch) -> FStateFrame.StateNode (+1Ch)
    // (from execGetStateName 0x10CF4830). EAIController: Enemy +320h, ExclusivePattern +428h, Pattern +42Ch,
    // bCanOpenDoors +2C0h bit 4 (Controller), bDoorMode +7E8h bit 1, door flags +7FCh (WillUseDoor 1,
    // UseDoorOpener 2, ProcessDoor 4, ShouldCloseDoor 8), baseDoor +814h, door state +86Ch (Release 1, Reverse 2,
    // DoorBlocked 4, FollowerReady 8, LeaderReady 10h, DoorOpen 20h, BlockedDoor 40h), BlockedDoor +878h,
    // BlockedDoorTime +87Ch (LevelInfo.TimeSeconds +430h), LastGoalStatus +8E4h, LastGoalType +8E8h.
    struct AITrack { Vec pos{}; DWORD movedTick = 0, pushLogged = 0, frozenLogged = 0; bool init = false; };
    std::unordered_map<void*, AITrack> g_aiTrack;
    void* g_aiTrackLevel = nullptr;

    void ShortName(void* obj, wchar_t* buf, size_t size)
    {
        wchar_t full[256];
        buf[0] = 0;
        if (!obj) { wcsncpy_s(buf, size, L"-", _TRUNCATE); return; }
        if (!SafeFullName(obj, full)) { wcsncpy_s(buf, size, L"?", _TRUNCATE); return; }
        const wchar_t* dot = wcsrchr(full, L'.');
        wcsncpy_s(buf, size, dot ? dot + 1 : full, _TRUNCATE);
    }

    // "Class.state" of an object's current script state, or "-" for none.
    void StateName(void* obj, wchar_t* buf, size_t size)
    {
        wcsncpy_s(buf, size, L"-", _TRUNCATE);
        if (!obj) return;
        void* node = nullptr;
        __try
        {
            void* frame = At<void*>(obj, 0xC);
            node = frame ? At<void*>(frame, 0x1C) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { node = nullptr; }
        if (!node) return;
        wchar_t full[256];
        if (!SafeFullName(node, full)) return;
        const wchar_t* sp = wcschr(full, L' ');
        const wchar_t* path = sp ? sp + 1 : full;
        const wchar_t* dot = wcschr(path, L'.');          // drop the package
        wcsncpy_s(buf, size, dot ? dot + 1 : path, _TRUNCATE);
    }

    void DoorFlags(void* ctrl, char* out, size_t size)
    {
        const uint32_t a = At<uint32_t>(ctrl, 0x7FC), b = At<uint32_t>(ctrl, 0x86C);
        snprintf(out, size, "%s%s%s%s%s%s%s%s%s%s%s%s", (At<uint32_t>(ctrl, 0x2C0) & 4) ? "" : "CANNOT-OPEN ",
                 (At<uint32_t>(ctrl, 0x7E8) & 1) ? "doormode " : "", (a & 1) ? "willuse " : "", (a & 2) ? "opener " : "",
                 (a & 4) ? "processing " : "", (a & 8) ? "shouldclose " : "", (b & 1) ? "release " : "", (b & 2) ? "reverse " : "",
                 (b & 4) ? "DOORBLOCKED " : "", (b & 0x20) ? "dooropen " : "", (b & 0x40) ? "BLOCKEDDOOR " : "",
                 (b & 0x18) ? "teamdoor " : "");
    }

    void DescribeGuard(void* pawn, void* ctrl, const AITrack* t, DWORD now, const char* why, float dist)
    {
        wchar_t name[96], st[128], pawnSt[128], pat[96], patSt[128], ex[96], exSt[128], door[96], blocked[96], enemy[96];
        ShortName(pawn, name, 96);
        StateName(ctrl, st, 128);
        StateName(pawn, pawnSt, 128);
        void* p = At<void*>(ctrl, 0x42C);
        void* x = At<void*>(ctrl, 0x428);
        ShortName(p, pat, 96);
        StateName(p, patSt, 128);
        ShortName(x, ex, 96);
        StateName(x, exSt, 128);
        ShortName(At<void*>(ctrl, 0x814), door, 96);
        ShortName(At<void*>(ctrl, 0x878), blocked, 96);
        ShortName(At<void*>(ctrl, 0x320), enemy, 96);
        char flags[160];
        DoorFlags(ctrl, flags, sizeof(flags));
        void* level = At<void*>(ctrl, 0x78);
        const float now_s = level ? At<float>(level, 0x430) : 0.0f;
        const Vec& v = At<Vec>(pawn, 0x100);
        const float speed = std::sqrt(v.x * v.x + v.y * v.y);
        const wchar_t* label = Guards::Label(pawn);
        LOG("[AI] %s %ls%s%ls | %.1f m from Sam, speed %.0f, still %.1f s, physics %d, health %d | state %ls, body %ls | pattern %ls (%ls) | "
            "exclusive %ls (%ls) | goal type %d status %d | enemy %ls | door %ls, blocked door %ls (%.1f s ago) %s",
            why, name, label ? " = " : "", label ? label : L"", dist / 100.0f, speed, t ? (now - t->movedTick) / 1000.0f : 0.0f,
            At<uint8_t>(pawn, 0x4C), At<int>(pawn, 0x420), st, pawnSt[0] ? pawnSt : L"-", pat, patSt, ex, exSt, At<uint8_t>(ctrl, 0x8E8), At<int>(ctrl, 0x8E4),
            enemy, door, blocked, At<void*>(ctrl, 0x878) ? now_s - At<float>(ctrl, 0x87C) : 0.0f, flags);
    }

    template <class F> void ForEachAIPawn(void* sam, F f)
    {
        void* xlevel = At<void*>(sam, 0x7C);
        if (!xlevel) return;
        void** actors = At<void**>(xlevel, 0x28);
        const int n = At<int>(xlevel, 0x2C);
        if (!actors || n <= 0 || n > 500000) return;
        for (int i = 0; i < n; ++i)
        {
            void* a = actors[i];
            if (!a || a == sam || (At<uint32_t>(a, 0x28) & 4)) continue;
            const uint32_t fl = At<uint32_t>(a, 0x2C);
            if (!(fl & 0x10) || (fl & 0x20)) continue;   // pawns, not the player
            void* c = At<void*>(a, 0x2B4);
            if (!c || At<int>(a, 0x420) <= 0) continue;   // controlled and alive
            if (At<void*>(c, 0x41C) != a) continue;      // an EAIController (its EPawn +41Ch is this pawn)
            if (At<uint8_t>(a, 0x4D) != 2) continue;     // drawn as a mesh (skips invisible stand-ins like Lambert's)
            f(a, c);
        }
    }

    // Out of action: the controller's or the pawn's state says so. A knocked-out guard can keep his controller in
    // s_Default (e.g. after loading a save) while the pawn is s_Unconscious / s_Inert / ..., and his behaviour
    // pattern stays in whatever state it was (searching, following...).
    bool KnockedOut(void* pawn, void* ctrl)
    {
        static const wchar_t* const kOut[] = { L"Unconscious", L"Dead", L"Dying", L"Grabbed", L"Inert", L"Carried",
                                               L"Stunned", L"Electrocuted" };
        wchar_t st[128];
        for (void* obj : { ctrl, pawn })
        {
            StateName(obj, st, 128);
            for (const wchar_t* w : kOut)
                if (wcsstr(st, w)) return true;
        }
        return false;
    }

    // Every 500 ms: who moved. Logs a guard that keeps trying to walk (speed > 40) without getting anywhere for
    // 3 s (walking into a door or wall), and one whose behaviour is in an alert state (anything but s_Default:
    // searching, attacking, investigating...) with an enemy, who hasn't moved for 20 s while on his feet
    // (PHYS_Walking). Knocked-out guards (controller or pawn state) are skipped.
    void WatchAI(void* sam)
    {
        static DWORD last = 0;
        const DWORD now = GetTickCount();
        if (now - last < 500) return;
        last = now;
        void* xlevel = At<void*>(sam, 0x7C);
        if (xlevel != g_aiTrackLevel) { g_aiTrackLevel = xlevel; g_aiTrack.clear(); }
        if (g_aiTrack.size() > 2048) g_aiTrack.clear();
        const Vec& s = At<Vec>(sam, 0xE8);
        ForEachAIPawn(sam, [&](void* pawn, void* ctrl)
        {
            AITrack& t = g_aiTrack[pawn];
            const Vec& p = At<Vec>(pawn, 0xE8);
            const float moved = std::sqrt((p.x - t.pos.x) * (p.x - t.pos.x) + (p.y - t.pos.y) * (p.y - t.pos.y));
            if (!t.init || moved > 24.0f) { t.pos = p; t.movedTick = now; t.init = true; return; }
            const Vec& v = At<Vec>(pawn, 0x100);
            const float speed = std::sqrt(v.x * v.x + v.y * v.y);
            const float dist = std::sqrt((p.x - s.x) * (p.x - s.x) + (p.y - s.y) * (p.y - s.y) + (p.z - s.z) * (p.z - s.z));
            if (speed > 40.0f && now - t.movedTick > 3000 && now - t.pushLogged > 15000 && !KnockedOut(pawn, ctrl))
            {
                t.pushLogged = now;
                DescribeGuard(pawn, ctrl, &t, now, "PUSHING (walks but doesn't move):", dist);
            }
            // only guards on their feet (PHYS_Walking): bodies lie with physics off
            if (At<void*>(ctrl, 0x320) && At<uint8_t>(pawn, 0x4C) == 1 && now - t.movedTick > 20000 && now - t.frozenLogged > 30000 &&
                !KnockedOut(pawn, ctrl))
            {
                wchar_t patSt[128];
                StateName(At<void*>(ctrl, 0x42C), patSt, 128);
                const size_t len = wcslen(patSt);
                if (len > 0 && wcscmp(patSt, L"-") != 0 && (len < 9 || wcscmp(patSt + len - 9, L"s_Default") != 0))
                {
                    t.frozenLogged = now;
                    DescribeGuard(pawn, ctrl, &t, now, "FROZEN (alert, has an enemy, hasn't moved for 20 s):", dist);
                }
            }
        });
    }

    // F7: every guard within 40 m of Sam, nearest first.
    void AIReport(void* sam)
    {
        const Vec& s = At<Vec>(sam, 0xE8);
        std::vector<std::pair<float, std::pair<void*, void*>>> rows;
        int out = 0;
        ForEachAIPawn(sam, [&](void* pawn, void* ctrl)
        {
            const Vec& p = At<Vec>(pawn, 0xE8);
            const float d = std::sqrt((p.x - s.x) * (p.x - s.x) + (p.y - s.y) * (p.y - s.y) + (p.z - s.z) * (p.z - s.z));
            if (d >= 4000.0f) return;
            if (KnockedOut(pawn, ctrl)) { ++out; return; }
            rows.push_back({ d, { pawn, ctrl } });
        });
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        LOG("[AI] F7 report at sam (%.0f %.0f %.0f): %d active guards within 40 m (%d knocked out, not listed)", s.x, s.y, s.z,
            static_cast<int>(rows.size()), out);
        const DWORD now = GetTickCount();
        int n = 0;
        for (const auto& r : rows)
        {
            if (++n > 12) break;
            const auto it = g_aiTrack.find(r.second.first);
            DescribeGuard(r.second.first, r.second.second, it == g_aiTrack.end() ? nullptr : &it->second, now, "F7:", r.first);
        }
    }

    // ---- Hearing per guard ----------------------------------------------------------------------------------
    // AActor::MakeNoise (0x10A84860; script native 512 -> actor virtual +210h) walks the level's controller list
    // (LevelInfo +5E0h, next +2D4h). For each: bonus = controller virtual +2B0h (stored at [esp+14h]), distance
    // from his pawn to the noise ([ebp] = location), heard if distance < radius ([esp+28h], footsteps +-200 by
    // difficulty) + bonus, and then if distance < radius x occlusion (walls/zones, 1 for doors) + bonus; the
    // controller is told through virtual +2B4h. A guard's Hearing trait (Guards::HearingSense) scales the radius
    // while he is being checked (hook A, right after the bonus is stored) and it is put back before the next
    // controller (hook B, the 'mov esi,[esi+2D4h]' every iteration ends with). Nested noises (a guard reacting
    // makes a noise of his own) each keep their own frame: saved per stack address.
    struct SavedRadius { uintptr_t at; float value; };
    SavedRadius g_savedRadius[8];
    int g_savedCount = 0;

    void __cdecl OnNoiseCheck(Mem::Regs* r)
    {
        void* ctrl = reinterpret_cast<void*>(r->esi);
        if (!ctrl) return;
        void* pawn = At<void*>(ctrl, 0x2B4);
        if (!pawn) return;
        const float h = Guards::HearingSense(pawn);
        if (h == 1.0f || g_savedCount >= 8) return;
        float& radius = r->Stack<float>(0x28);
        if (!(radius > 0.0f && radius < 1e6f)) return;
        g_savedRadius[g_savedCount++] = { r->esp + 0x28, radius };
        const float orig = radius;
        radius *= h;
        if (!g_log) return;
        void* inst = r->Stack<void*>(0x34);
        if (!inst || !(At<uint32_t>(inst, 0x2C) & 0x20)) return;   // noises Sam makes
        const Vec* at = reinterpret_cast<const Vec*>(r->ebp);
        const Vec& gp = At<Vec>(pawn, 0xE8);
        const float dist = std::sqrt((gp.x - at->x) * (gp.x - at->x) + (gp.y - at->y) * (gp.y - at->y) + (gp.z - at->z) * (gp.z - at->z));
        const float bonus = r->Stack<float>(0x14);
        if (dist > std::max(orig, radius) + bonus) return;          // out of range either way
        static std::unordered_map<void*, DWORD> last;
        if (last.size() > 1024) last.clear();
        const DWORD now = GetTickCount();
        DWORD& t = last[ctrl];
        if (t && now - t < 2000) return;
        t = now;
        const wchar_t* label = Guards::Label(pawn);
        LOG("[PERCEPTION] guard (%ls, hearing x%.2f): noise type %d from Sam %.1f m away, range %.1f -> %.1f m before walls%s",
            label ? label : L"no personality", h, r->Stack<int>(0x2C), dist / 100.0f, (orig + bonus) / 100.0f, (radius + bonus) / 100.0f,
            dist < orig + bonus && dist >= radius + bonus ? " (would have heard it)" :
            dist >= orig + bonus && dist < radius + bonus ? " (hears it only thanks to his hearing)" : "");
    }

    void __cdecl OnNoiseNext(Mem::Regs* r)
    {
        if (g_savedCount && g_savedRadius[g_savedCount - 1].at == r->esp + 0x28)
        {
            --g_savedCount;
            r->Stack<float>(0x28) = g_savedRadius[g_savedCount].value;
        }
    }

    // ---- AI event log (LogPerception) ---------------------------------------------------------------------
    // Every AI event a guard receives goes through the AI manager's dispatcher (0x10BDD590, thiscall (controller,
    // event*), 0x94-byte event: type byte at +0 = AIEventType, the actor it is about at +38h). Logged for guards
    // within 30 m of Sam, at most once per type per guard every 2 s: what made a guard react, and to what.
    using Dispatch_t = void(__fastcall*)(void* mgr, void* edx, void* ctrl, const uint8_t* ev);
    Dispatch_t oDispatch = nullptr;
    void* g_samPawn = nullptr;

    void LogAIEvent(void* ctrl, const uint8_t* ev)
    {
        void* sam = g_samPawn;
        void* pawn = At<void*>(ctrl, 0x41C);
        if (!sam || !pawn || pawn == sam) return;
        const Vec& sp = At<Vec>(sam, 0xE8);
        const Vec& gp = At<Vec>(pawn, 0xE8);
        const float dist = std::sqrt((sp.x - gp.x) * (sp.x - gp.x) + (sp.y - gp.y) * (sp.y - gp.y) + (sp.z - gp.z) * (sp.z - gp.z));
        if (dist > 3000.0f) return;
        const uint8_t type = ev[0];
        static std::unordered_map<uint64_t, DWORD> last;
        if (last.size() > 4096) last.clear();
        const DWORD now = GetTickCount();
        DWORD& t = last[(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ctrl)) << 8) | type];
        if (t && now - t < 2000) return;
        t = now;
        void* about = *reinterpret_cast<void* const*>(ev + 0x38);
        wchar_t aboutName[256] = L"";
        if (about == sam) wcscpy_s(aboutName, L"Sam");
        else if (about && Game::GetFullName) SafeFullName(about, aboutName);
        const wchar_t* label = Guards::Label(pawn);
        const char* name = type < sizeof(kAIEventNames) / sizeof(kAIEventNames[0]) ? kAIEventNames[type] : "?";
        LOG("[EVENT] guard %.1f m from Sam (%ls) got %s (%u) about %ls",
            dist / 100.0f, label ? label : L"no personality", name, unsigned(type), about ? (aboutName[0] ? aboutName : L"?") : L"nothing");
    }

    // ---- Turn watch (LogPerception): a guard whose facing swings 60+ degrees within 0.15 s ---------------------
    // A pawn's facing is Rotation.Yaw (+F8h, 65536 = 360 deg). Engine rotation changes go through ULevel::MoveActor
    // (XLevel +7Ch, vtable +84h: thiscall (actor, FVector delta, FRotator rotation, FCheckResult&, 4 flags), as
    // AActor::SetRotation 0x109903E0 calls it); the hook records every call that turns a watched guard 10+ degrees
    // at once, with the code that made it. The pawn tick hook samples his yaw before and after his own tick.
    constexpr int kTurnHist = 64, kTurnCalls = 8, kTurnEvents = 6;
    constexpr int kSwingUnits = 10923;              // 60 deg
    constexpr int kCallUnits = 1820;                // 10 deg
    constexpr double kSwingWindow = 0.15;
    struct TurnTrack
    {
        struct Sample { double t; int yaw, inTick, between; };
        struct Call { double t; int from, to, chainN; uintptr_t chain[6]; };
        struct Ev { double t; uint8_t type; bool aboutSam; };
        Sample hist[kTurnHist]; int head = 0, count = 0;
        Call calls[kTurnCalls]; int callHead = 0, callCount = 0;
        Ev ev[kTurnEvents]; int evHead = 0, evCount = 0;
        int beforeYaw = 0, afterYaw = 0; bool valid = false;
        double lastLog = -100;
        bool pending = false;                 // a swing in progress, logged once it settles
        double pendT = 0, lastMoveT = 0; int pendYaw = 0;
        struct PitchSample { double t; int pitch; };   // pitch too: a drone faces you by pitching down
        PitchSample pitch[24]; int pitchHead = 0, pitchCount = 0;
        double pitchLog = -100;
    };
    std::unordered_map<void*, TurnTrack> g_turn;
    void* g_turnLevel = nullptr;
    void* oMoveActor = nullptr;
    bool g_turnHooked = false;
    int g_turnLogs = 0;

    double TurnNow()
    {
        static LARGE_INTEGER f{};
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
    }
    float Dist(void* a, void* b)
    {
        const Vec& p = At<Vec>(a, 0xE8);
        const Vec& q = At<Vec>(b, 0xE8);
        return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z));
    }
    int YawDiff(int a, int b) { return ((a - b + 0x8000) & 0xFFFF) - 0x8000; }   // signed, -32768..32767
    float YawDeg(int units) { return static_cast<float>(units) * (360.0f / 65536.0f); }

    // A return address: inside the exe's code, right after a call instruction.
    bool IsReturnAddress(uintptr_t a)
    {
        const Mem::Range t = Mem::ExeText();
        if (a < t.start + 8 || a >= t.start + t.size) return false;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(a);
        if (p[-5] == 0xE8) return true;
        if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;   // call reg / [reg]
        if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;   // call [reg+disp8]
        if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;   // call [disp32] / [reg+disp32]
        if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return true;   // call [base+index*s+disp32]
        return false;
    }

    void __cdecl OnMoveActor(void* level, const uint32_t* args, uintptr_t ret)
    {
        __try
        {
            void* actor = reinterpret_cast<void*>(args[0]);
            if (!actor) return;
            const int cur = At<int>(actor, 0xF8) & 0xFFFF, to = static_cast<int>(args[5]) & 0xFFFF;
            const int d = YawDiff(to, cur);
            if (d > -kCallUnits && d < kCallUnits) return;
            const auto it = g_turn.find(actor);
            if (it == g_turn.end()) return;
            TurnTrack& tr = it->second;
            TurnTrack::Call& c = tr.calls[tr.callHead];
            tr.callHead = (tr.callHead + 1) % kTurnCalls;
            if (tr.callCount < kTurnCalls) ++tr.callCount;
            c.t = TurnNow(); c.from = cur; c.to = to;
            c.chain[0] = ret; c.chainN = 1;
            // The callers further up: return addresses found on the stack above the arguments.
            const uintptr_t top = reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
            for (uintptr_t p = reinterpret_cast<uintptr_t>(args) + 12 * 4; p + 4 <= top && p < reinterpret_cast<uintptr_t>(args) + 0x600 && c.chainN < 6; p += 4)
            {
                const uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
                if (IsReturnAddress(v)) c.chain[c.chainN++] = v;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Detour entry: hands (level, args, return address) to OnMoveActor, then runs the original untouched.
    __declspec(naked) void hkMoveActor()
    {
        __asm
        {
            pushad
            mov eax, esp
            add eax, 32
            push dword ptr [eax]
            lea edx, [eax + 4]
            push edx
            push ecx
            call OnMoveActor
            add esp, 12
            popad
            jmp dword ptr [oMoveActor]
        }
    }

    void TurnEvent(void* ctrl, const uint8_t* ev)
    {
        void* pawn = At<void*>(ctrl, 0x41C);
        const auto it = g_turn.find(pawn);
        if (it == g_turn.end()) return;
        TurnTrack& tr = it->second;
        TurnTrack::Ev& e = tr.ev[tr.evHead];
        tr.evHead = (tr.evHead + 1) % kTurnEvents;
        if (tr.evCount < kTurnEvents) ++tr.evCount;
        e.t = TurnNow(); e.type = ev[0];
        void* about = *reinterpret_cast<void* const*>(ev + 0x38);
        e.aboutSam = about && about == g_samPawn;
    }

    // Facing relative to the direction to Sam, in degrees (0 = looking right at him).
    float FacingFromSam(void* pawn, int yaw)
    {
        if (!g_samPawn) return 999.0f;
        const Vec& s = At<Vec>(g_samPawn, 0xE8);
        const Vec& p = At<Vec>(pawn, 0xE8);
        const int toSam = static_cast<int>(std::atan2(s.y - p.y, s.x - p.x) * (32768.0 / 3.14159265358979)) & 0xFFFF;
        return std::fabs(YawDeg(YawDiff(yaw, toSam)));
    }

    // Logs a finished swing: from yaw y0 (after the sample at t0) to y1 (by t1).
    void LogSwing(void* pawn, TurnTrack& tr, double t0, int y0, double t1, int y1)
    {
        void* ctrl = At<void*>(pawn, 0x2B4);
        int inTick = 0, between = 0, frames = 0;
        for (int i = 0; i < tr.count; ++i)
        {
            const TurnTrack::Sample& s = tr.hist[(tr.head + kTurnHist - 1 - i) % kTurnHist];
            if (s.t <= t0 + 1e-6) break;
            if (s.t > t1 + 1e-6) continue;
            inTick += s.inTick; between += s.between; ++frames;
        }
        const double over = std::max(t1 - t0, 0.001);
        const int swing = YawDiff(y1, y0);
        const float dist = g_samPawn ? Dist(pawn, g_samPawn) : 0.0f;
        const wchar_t* label = Guards::Label(pawn);
        wchar_t st[128], body[128], pat[128];
        StateName(ctrl, st, 128);
        StateName(pawn, body, 128);
        StateName(ctrl ? At<void*>(ctrl, 0x42C) : nullptr, pat, 128);
        const Vec& v = At<Vec>(pawn, 0x100);
        char line[2400];
        int o = sprintf_s(line, "[TURN] guard %.1f m from Sam (%ls) turned %.0f deg in %.2f s over %d frames (%.0f deg/s): Sam %.0f deg off his facing -> %.0f deg"
                          " | in his own tick %.0f deg, between his ticks %.0f deg | state %ls, body %ls, pattern %ls | physics %d, speed %.0f |",
                          dist / 100.0f, label ? label : L"no personality", YawDeg(swing), over, frames, std::fabs(YawDeg(swing)) / over,
                          FacingFromSam(pawn, y0), FacingFromSam(pawn, y1), YawDeg(inTick), YawDeg(between), st, body, pat,
                          static_cast<int>(At<uint8_t>(pawn, 0x4C)), std::sqrt(v.x * v.x + v.y * v.y));
        auto add = [&](const char* fmt, auto... a)
        {
            if (o <= 0 || o >= static_cast<int>(sizeof(line)) - 160) return;
            const int w = sprintf_s(line + o, sizeof(line) - o, fmt, a...);
            if (w > 0) o += w;
        };
        // Big single rotation changes in the window (MoveActor calls), newest first.
        int shown = 0;
        for (int i = 0; i < tr.callCount; ++i)
        {
            const TurnTrack::Call& c = tr.calls[(tr.callHead + kTurnCalls - 1 - i) % kTurnCalls];
            if (c.t < t0 - 0.02 || c.t > t1 + 0.02) continue;
            add(" set %.0f deg at +%.2f s by %08X", YawDeg(YawDiff(c.to, c.from)), c.t - t0, unsigned(c.chain[0]));
            if (c.chainN > 1) { add(" ("); for (int k = 1; k < c.chainN; ++k) add(k > 1 ? " %08X" : "%08X", unsigned(c.chain[k])); add(")"); }
            add(";");
            ++shown;
        }
        if (!shown) add(" no single 10+ deg rotation set: turned in small steps or written directly;");
        // Animations playing (mesh instance +64h, channels +94h, 80h each: sequence +0, frame +0Ch, alpha +30h).
        add(" anims:");
        __try
        {
            void* inst = At<void*>(pawn, 0x64);
            const int num = inst ? std::min(At<int>(inst, 0x98), 16) : 0;
            const uint8_t* data = inst ? At<uint8_t*>(inst, 0x94) : nullptr;
            for (int ch = 0; data && ch < num; ++ch)
            {
                const uint8_t* c = data + ch * 0x80;
                const int seq = *reinterpret_cast<const int*>(c);
                const float alpha = *reinterpret_cast<const float*>(c + 0x30);
                if (!seq || alpha <= 0.01f) continue;
                wchar_t name[48];
                if (!Script::NameText(seq, name, 48)) swprintf_s(name, L"#%d", seq);
                add(" ch%d %ls a%.2f f%.2f", ch, name, alpha, *reinterpret_cast<const float*>(c + 0x0C));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        add(" | events:");
        if (!tr.evCount) add(" none");
        for (int i = 0; i < tr.evCount; ++i)
        {
            const TurnTrack::Ev& e = tr.ev[(tr.evHead + kTurnEvents - 1 - i) % kTurnEvents];
            if (t0 - e.t > 3.0) break;
            add(" %s%s %.2f s %s,", e.type < sizeof(kAIEventNames) / sizeof(kAIEventNames[0]) ? kAIEventNames[e.type] : "?",
                e.aboutSam ? " (Sam)" : "", std::fabs(t0 - e.t), e.t <= t0 ? "before" : "into the turn");
        }
        LOG("%s", line);
    }

    void InstallTurnHook(void* xlevel)
    {
        g_turnHooked = true;   // one attempt
        void* target = (*reinterpret_cast<void***>(xlevel))[0x84 / 4];
        const Mem::Range t = Mem::ExeText();
        const uintptr_t a = reinterpret_cast<uintptr_t>(target);
        if (a >= t.start && a < t.start + t.size &&
            Mem::Detour("level MoveActor", target, reinterpret_cast<void*>(&hkMoveActor), &oMoveActor))
            LOG("Perception: turn watch on ([TURN] lines; MoveActor at %08X)", unsigned(a));
        else
            LOG("Perception: turn watch on, without the MoveActor hook (%08X)", unsigned(a));
    }

    // After the guard's own tick: a sample per frame; a swing of 60+ degrees within 0.15 s is logged once it
    // settles (no change for 0.1 s, or after 2 s).
    // A pitch swing of 45+ degrees within the swing window (once a second per pawn): what a hovering drone does
    // to face Sam below it. Logged with how far and fast, physics and the AI states.
    void PitchSample(void* pawn, TurnTrack& tr, int pitch)
    {
        const double now = TurnNow();
        TurnTrack::PitchSample& s = tr.pitch[tr.pitchHead];
        s.t = now; s.pitch = pitch;
        tr.pitchHead = (tr.pitchHead + 1) % 24;
        if (tr.pitchCount < 24) ++tr.pitchCount;
        if (now - tr.pitchLog < 1.0) return;
        for (int i = 1; i < tr.pitchCount; ++i)
        {
            const TurnTrack::PitchSample& o = tr.pitch[(tr.pitchHead + 24 - 1 - i) % 24];
            if (now - o.t > kSwingWindow) break;
            const int d = YawDiff(pitch, o.pitch);
            if (d >= 8192 || d <= -8192)
            {
                tr.pitchLog = now;
                if (g_turnLogs >= 300 || !g_samPawn || Dist(pawn, g_samPawn) > 3000.0f || At<int>(pawn, 0x420) <= 0) return;
                ++g_turnLogs;
                void* ctrl = At<void*>(pawn, 0x2B4);
                wchar_t st[128], body[128];
                StateName(ctrl, st, 128);
                StateName(pawn, body, 128);
                const wchar_t* label = Guards::Label(pawn);
                LOG("[TURN] guard %.1f m from Sam (%ls) pitched %.0f deg in %.2f s | physics %d | state %ls, body %ls",
                    Dist(pawn, g_samPawn) / 100.0f, label ? label : L"no personality", YawDeg(d), now - o.t,
                    int(At<uint8_t>(pawn, 0x4C)), st, body);
                return;
            }
        }
    }

    void TurnSample(void* pawn, TurnTrack& tr, int yaw)
    {
        const double now = TurnNow();
        TurnTrack::Sample& s = tr.hist[tr.head];
        s.t = now; s.yaw = yaw;
        s.inTick = YawDiff(yaw, tr.beforeYaw);
        s.between = tr.valid ? YawDiff(tr.beforeYaw, tr.afterYaw) : 0;
        tr.head = (tr.head + 1) % kTurnHist;
        if (tr.count < kTurnHist) ++tr.count;
        tr.afterYaw = yaw;
        tr.valid = true;
        if (!tr.pending)
        {
            if (now - tr.lastLog < 1.0) return;
            for (int i = 1; i < tr.count; ++i)
            {
                const TurnTrack::Sample& o = tr.hist[(tr.head + kTurnHist - 1 - i) % kTurnHist];
                if (now - o.t > kSwingWindow) break;
                const int d = YawDiff(yaw, o.yaw);
                if (d >= kSwingUnits || d <= -kSwingUnits)
                {
                    // start where the turn began: walk back while the frames kept turning the same way
                    int k = i;
                    while (k + 1 < tr.count)
                    {
                        const TurnTrack::Sample& p = tr.hist[(tr.head + kTurnHist - 1 - k) % kTurnHist];
                        const int step = p.inTick + p.between;
                        if ((d > 0 ? step : -step) < 91 || now - p.t > 2.0) break;   // under 0.5 deg that frame
                        ++k;
                    }
                    const TurnTrack::Sample& start = tr.hist[(tr.head + kTurnHist - 1 - k) % kTurnHist];
                    tr.pending = true; tr.pendT = start.t; tr.pendYaw = start.yaw; tr.lastMoveT = now;
                    break;
                }
            }
            return;
        }
        if (std::abs(s.inTick + s.between) >= 91) tr.lastMoveT = now;
        if (now - tr.lastMoveT < 0.1 && now - tr.pendT < 2.0) return;
        tr.pending = false;
        tr.lastLog = now;
        void* ctrl = At<void*>(pawn, 0x2B4);
        if (g_turnLogs >= 300 || !g_samPawn || Dist(pawn, g_samPawn) > 3000.0f || At<int>(pawn, 0x420) <= 0 || !ctrl ||
            KnockedOut(pawn, ctrl))
            return;
        ++g_turnLogs;
        LogSwing(pawn, tr, tr.pendT, tr.pendYaw, tr.lastMoveT, yaw);
    }

    void __fastcall hkDispatch(void* mgr, void* edx, void* ctrl, const uint8_t* ev)
    {
        if (!Reaction::AllowEvent(ctrl, ev)) return;   // focus: a guard busy fighting another NPC
        if (ctrl && ev) Alert::OnEvent(ctrl, ev);       // flinch, suppression
        if (g_log && ctrl && ev)
        {
            __try { LogAIEvent(ctrl, ev); TurnEvent(ctrl, ev); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        oDispatch(mgr, edx, ctrl, ev);
    }

    float IniFloat(const std::wstring& ini, const wchar_t* key, float def)
    {
        wchar_t buf[64];
        GetPrivateProfileStringW(L"Perception", key, L"", buf, 64, ini.c_str());
        for (wchar_t* p = buf; *p; ++p) if (*p == L';') { *p = 0; break; }
        return buf[0] ? static_cast<float>(_wtof(buf)) : def;
    }
}

// The engine's light level at a point (Sam's light-meter scale), from the level lights in range: used to shade the
// Niko plush. Sam is the cache actor (his traces and cache are left as they were).
bool Perception::LightAt(void* sam, const float point[3], float& out)
{
    if (!sam) return false;
    void* level = At<void*>(sam, 0x7C);
    if (!level) return false;
    RefreshLevelLights(level);
    int used = 0;
    return BackgroundLuminosity(level, sam, Vec{ point[0], point[1], point[2] }, out, used);
}

void Perception::OnPlayerTick(void* pawn)
{
    if (!g_log || !pawn) return;
    g_samPawn = pawn;
    WatchAI(pawn);
    const bool fg = Game::hWnd && GetForegroundWindow() == Game::hWnd;
    static bool wasDown = false, wasDown7 = false;
    const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !wasDown && fg)
    {
        if ((g_backlight && !g_blFailed) || ((g_directional || g_cover || g_beam) && !g_dirFailed))
            CameraBacklightCheck(pawn);
        LogCharacters(pawn);
    }
    wasDown = down;
    const bool down7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (down7 && !wasDown7 && fg)
        AIReport(pawn);
    wasDown7 = down7;
}

bool Perception::BeforePawnTick(void* pawn)
{
    if (!g_log || !pawn) return false;
    void* xlevel = nullptr;
    __try { xlevel = At<void*>(pawn, 0x7C); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!xlevel) return false;
    if (xlevel != g_turnLevel) { g_turnLevel = xlevel; g_turn.clear(); g_turnLogs = 0; }
    if (!g_turnHooked) InstallTurnHook(xlevel);
    auto it = g_turn.find(pawn);
    if (it == g_turn.end())
    {
        if (g_turn.size() > 1024) return false;
        it = g_turn.emplace(pawn, TurnTrack{}).first;
    }
    it->second.beforeYaw = At<int>(pawn, 0xF8) & 0xFFFF;
    return true;
}

void Perception::AfterPawnTick(void* pawn)
{
    const auto it = g_turn.find(pawn);
    if (it == g_turn.end()) return;
    __try
    {
        TurnSample(pawn, it->second, At<int>(pawn, 0xF8) & 0xFFFF);
        PitchSample(pawn, it->second, At<int>(pawn, 0xF4) & 0xFFFF);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Perception::Init()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring ini = path;
    ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"SCCTRemaster_Guards.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        LOG("Perception: %ls not found, original perception", ini.c_str());
        return;
    }
    const bool shadows = IniFloat(ini, L"ShadowDetection", 1) != 0;
    g_peripheral = IniFloat(ini, L"PeripheralVision", 1) != 0;
    g_edge = std::clamp(IniFloat(ini, L"PeripheralEdge", 0.4f), 0.0f, 1.0f);
    g_start = std::clamp(IniFloat(ini, L"PeripheralStart", 0.35f), 0.0f, 0.95f);
    g_crouchSilhouette = IniFloat(ini, L"CrouchSilhouette", 1) != 0;
    g_log = IniFloat(ini, L"LogPerception", 0) != 0;
    g_backlight = IniFloat(ini, L"Backlighting", 1) != 0;
    g_blStrength = std::clamp(IniFloat(ini, L"BacklightStrength", 0.6f), 0.0f, 2.0f);
    g_sky = std::clamp(IniFloat(ini, L"SkyBrightness", 40.0f), 0.0f, 200.0f);
    g_skyDist = std::clamp(IniFloat(ini, L"SkyDistance", 3000.0f), 200.0f, 50000.0f);
    g_directional = IniFloat(ini, L"DirectionalLighting", 1) != 0;
    g_cover = IniFloat(ini, L"PartialCover", 1) != 0;
    g_beam = IniFloat(ini, L"SpotlightBeam", 1) != 0;
    g_beamCore = std::clamp(IniFloat(ini, L"BeamCore", 0.4f), 0.0f, 0.95f);
    g_darkSide = std::clamp(IniFloat(ini, L"DarkSide", 0.25f), 0.0f, 1.0f);
    g_shadowRange = std::clamp(IniFloat(ini, L"ShadowSightRange", 10.0f), 0.0f, 200.0f) * 100.0f;
    g_shadowMinLight = std::clamp(IniFloat(ini, L"ShadowMinLight", 40.0f), 0.0f, 500.0f);

    int ok = 0;
    if (shadows)
        if (const uintptr_t a = Mem::Find("light shadow registration",
                "56 8B F1 E8 ? ? ? ? F6 86 F8 02 00 00 01 74 ? F6 86 60 03 00 00 02 74 ? 8B 46 78 8B 88 C8 09 00 00 56 E8"))
        {
            const uint8_t nops[2] = { 0x90, 0x90 };
            Mem::WriteBytes(a + 0x18, nops, 2);   // skip the bNPCShadowDetect test
            // The guards' shadow check tests bNPCShadowDetect again before using a light (test [esi+360h],2;
            // jz skip): without this the extra lights were registered but never used.
            if (const uintptr_t b = Mem::Find("shadow check light test",
                    "F6 86 F8 02 00 00 01 0F 84 ? ? ? ? F6 86 60 03 00 00 02 0F 84"))
            {
                Mem::Nop(b + 0x14, 6);
                ++ok;
                // Range and light limits for the extra lights, on the guard's line-of-sight call to the spot.
                if (const uintptr_t c = Mem::Find("shadow spot sight",
                        "8B 8D 1C 04 00 00 8B 11 57 8D 44 24 58 50 FF 92 F4 01 00 00 85 C0 74"))
                    if (Mem::MidHook("shadow spot sight", c + 0x0E, 6, &OnShadowVisible, Mem::MidMode::OriginalBefore))
                        ++ok;
            }
        }
    // Hearing per guard: scale the noise radius while each controller is checked, restore it after.
    if (const uintptr_t a = Mem::Find("noise check", "8B 06 8B CE FF 90 B0 02 00 00 D9 5C 24 14 8B 86 B4 02 00 00 D9 80 E8 00 00 00"))
    {
        // The loop's 'mov esi,[esi+2D4h]; test esi,esi; jnz' (next controller), within the same function.
        const uintptr_t b = Mem::Find("noise next", "8B B6 D4 02 00 00 85 F6 0F 85 ? ? ? ? 5F 5E 5D 5B 83 C4 14 C2 1C 00");
        if (b && b > a && b - a < 0x200 &&
            Mem::MidHook("noise next", b, 6, &OnNoiseNext, Mem::MidMode::OriginalAfter) &&
            Mem::MidHook("noise check", a + 0x0E, 6, &OnNoiseCheck, Mem::MidMode::OriginalAfter))
            ++ok;
    }
    // Shadow sense per guard, and the shadow sighting log.
    if (const uintptr_t a = Mem::Find("shadow check", "55 8B EC 83 E4 F8 81 EC 38 02 00 00 53 55 56 57 8B E9 8B 85 F4 04 00 00"))
        if (Mem::Detour("shadow check", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkShadowCheck), reinterpret_cast<void**>(&oShadowCheck)))
            ++ok;
    if (g_log)
        if (const uintptr_t a = Mem::Find("shadow sighting", "C6 84 24 98 00 00 00 3A 89 94 24 C8 00 00 00"))
            if (Mem::MidHook("shadow sighting", a, 8, &OnShadowSeen, Mem::MidMode::OriginalAfter))
                ++ok;
    if (g_peripheral)
        if (const uintptr_t a = Mem::Find("peripheral factor", "8B 44 24 10 83 EC 30 85 C0 0F 8C ? ? ? ? 8B 91 F0 04 00 00 6B C0 64 80 7C 02 34 7A"))
            if (Mem::Detour("peripheral factor", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkPeripheral), reinterpret_cast<void**>(&oPeripheral)))
                ++ok;
    if (g_crouchSilhouette)
        if (const uintptr_t a = Mem::Find("silhouette factor", "83 EC 3C D9 44 24 48 C7 04 24 00 00 80 3F D8 0D ? ? ? ? D8 5C 24 44"))
            if (Mem::Detour("silhouette factor", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkSilhouette), reinterpret_cast<void**>(&oSilhouette)))
                ++ok;
    if (g_log || Reaction::WantsEvents() || Alert::WantsEvents())
        if (const uintptr_t a = Mem::Find("ai event dispatch", "55 8B EC 83 E4 F8 81 EC AC 00 00 00 53 8B 5D 08 85 DB 56 8B 71 78 57"))
            if (Mem::Detour("ai event dispatch", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkDispatch), reinterpret_cast<void**>(&oDispatch)))
                ++ok;
    if (g_backlight || g_directional || g_cover || g_beam)
    {
        // Sam's light level updater (0x10B7F0D0): the bone list offsets and the bone-location call, read from its
        // code and checked byte by byte.
        if (const uintptr_t f = Mem::Find("sam light bones", "83 EC 10 53 56 8B F1 8B 86 18 15 00 00 8B 0D ? ? ? ? 57 99"))
        {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(f);
            auto u32 = [&](size_t o) { return *reinterpret_cast<const uint32_t*>(b + o); };
            if (b[0x07] == 0x8B && b[0x08] == 0x86 && b[0x2A] == 0x8B && b[0x2B] == 0x86 && b[0x3D] == 0xE8)
            {
                g_boneCountOff = u32(0x09);
                g_boneListOff = u32(0x2C);
                g_boneLoc = reinterpret_cast<BoneLoc_t>(f + 0x3D + 5 + static_cast<int32_t>(u32(0x3E)));
            }
            else
                LOG("Perception: Sam's light updater doesn't match; measuring his middle and chest instead");
        }

        // The renderer's RelevantLights filter: movzx ecx, [light+2FDh] (LightSet); test [ecx*4 + masks], 1 << LightingSet
        // (two identical copies of the loop, same table)
        if (const uintptr_t a = Mem::Find("light set masks", "0F B6 8B FD 02 00 00 85 14 8D ? ? ? ?", 0, 2))
            g_lightSetMasks = *reinterpret_cast<const uint32_t* const*>(a + 10);
        if (const uintptr_t a = Mem::Find("target light level", "8B 07 6A 00 8B CF FF 90 E8 01 00 00 D9 5C 24 14 8B 8E 1C 04 00 00"))
            if (Mem::MidHook("target light level", a + 0x10, 6, &OnTargetLuminosity, Mem::MidMode::OriginalAfter))
                ++ok;
    }
    char shadowDesc[96];
    if (shadows) snprintf(shadowDesc, sizeof(shadowDesc), "from every shadow-casting light (extra lights: within %.0f m, light %.0f+)",
                          g_shadowRange / 100.0f, g_shadowMinLight);
    else snprintf(shadowDesc, sizeof(shadowDesc), "original");
    LOG("Perception: shadow detection %s, peripheral vision %s (edge x%.2f from %.0f%% of the half-cone), crouch silhouette %s, "
        "directional lighting %s (dark side x%.2f), partial cover %s (%s, %s), spotlight beam %s, backlighting %s (strength %.2f, sky %.0f) (%d hooks)",
        shadowDesc, g_peripheral ? "on" : "off", g_edge, g_start * 100.0f,
        g_crouchSilhouette ? "on" : "off", g_directional ? "on" : "off", g_darkSide, g_cover ? "on" : "off",
        g_boneLoc ? "Sam's light bones" : "middle and chest only", "guards' sight trace", g_beam ? "on" : "off",
        g_backlight ? "on" : "off", g_blStrength, g_sky, ok);
}
