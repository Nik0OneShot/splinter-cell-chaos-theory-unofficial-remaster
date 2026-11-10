// Noise pollution ([Perception] NoisePollution in SCCTRemaster_Guards.ini) and footstep sync ([Perception] FootstepSync).
//
// How the game masks Sam's noises (levels in dB, 0..120; the HUD sound meter, 0x10B66BC0, draws 23 bars over 120 dB):
//  - The audio subsystem's meter update (0x10D17980, thiscall, no arguments, every frame) sets the PlayerController's
//    ambient level +438h (the zone's ambient, timed masking sounds and placed masking sources at Sam's spot, summed as
//    powers: 10 log10(sum of 10^(L/10))) and Sam's own noise level +43Ch. The HUD meter shows both.
//  - AActor::MakeNoise (0x10A84860, thiscall (radius, type, FVector* location, instigator, 3 more), ret 1Ch) drops a
//    noise of Sam's outright while ambient +438h > his level +43Ch, and then, for every listener, compares the masking
//    level at the listener's spot (audio subsystem vtable +164h = 0x10D17FF0, thiscall (pawn) -> float) with +43Ch:
//    louder there and that guard doesn't hear it (single player).
// Originally only placed sources (machinery, sound triggers, thunder) mask. Here what guards do counts too:
//  - Gunfire and other loud noises not made by Sam (MakeNoise: gunfire at the weapon's FireNoiseLevel, recovered from
//    the radius GetSoundRad gave, explosions, grenades, shattering, falling objects, alarms), fading over GunfireFade s.
//  - The footsteps of every walking (GuardWalkNoise dB) or running (GuardRunNoise dB) NPC.
// Each falls off 6 dB per doubling of distance from 1 m, out to the distance the AI hears it from (footsteps:
// FootstepMaskRange). A guard's own noise counts in full for himself: a guard who is shooting or walking hears less.
// The per-listener level is raised by a detour of 0x10D17FF0; Sam's ambient (and so the HUD meter and the
// drop-it-outright check) right after each meter update.
//
// Footstep sync: walk at a guard's pace (speed within SyncSpeed %, heading within SyncAngle degrees, within SyncRange
// metres) for SyncTime seconds and your footsteps are lost in his: they make no AI noise at all, and the meter's
// ambient level sits just above them. It lasts 0.5 s past the end of the match (corners, a pause).
#include "pch.h"
#include "features.h"
#include "aievents.h"

extern HMODULE g_self;

namespace
{
    struct Vec { float x, y, z; };
    template <class T> T& At(void* p, uintptr_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p) + off); }

    constexpr uintptr_t kLocation = 0xE8, kVelocity = 0x100, kController = 0x2B4, kHealth = 0x420, kPhysics = 0x4C;
    constexpr uintptr_t kAmbient = 0x438, kSamLevel = 0x43C;
    constexpr uint8_t kQuietStep = 57, kHeavyStep = 89;

    bool g_on = true, g_sync = true, g_log = false;
    float g_walkDb = 20.0f, g_runDb = 35.0f, g_stepRange = 800.0f, g_fade = 1.0f;
    float g_syncRange = 600.0f, g_syncSpeed = 0.2f, g_syncAngle = 25.0f, g_syncTime = 0.75f;
    constexpr float kRunSpeed = 250.0f, kMoving = 60.0f, kSyncGrace = 0.5f;

    using MakeNoise_t = void(__fastcall*)(void* actor, void* edx, float radius, int type, const Vec* loc, void* inst, int a5, int a6, int a7);
    using MaskAt_t = float(__fastcall*)(void* audio, void* edx, void* pawn);
    using Update_t = void(__fastcall*)(void* audio, void* edx);
    MakeNoise_t oMakeNoise = nullptr;
    MaskAt_t oMaskAt = nullptr;
    Update_t oUpdate = nullptr;

    void* g_sam = nullptr;
    void* g_level = nullptr;

    // Sam's lingering noise level (PlayerController +440h: what his last noise left, part of his meter level +43Ch)
    // fades in the engine's frame tick (0x10964490, at 0x109649A2): x0.9 every frame, 0 once at 3 dB or less. That
    // is tuned for 30 fps (halved every 0.22 s); at 160 fps it halves every 0.04 s, so a noise stopped counting
    // almost at once. The fmul's constant (0.1, 0x110B8AA0) is swapped for g_decayFrac, set every frame to
    // 1 - 0.9^(dt x NoiseDecayRate).
    float g_decayFrac = 0.1f, g_decayRate = 30.0f, g_decayLast = -1.0f;
    bool g_decayFixed = false;

    float LevelTime(void* actor)
    {
        void* level = At<void*>(actor, 0x78);
        return level ? At<float>(level, 0x430) : 0.0f;
    }
    float Dist(const Vec& a, const Vec& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
    float Speed2D(const Vec& v) { return std::sqrt(v.x * v.x + v.y * v.y); }
    bool IsSam(void* pawn) { return pawn && (At<uint8_t>(pawn, 0x2C) & 0x20) != 0; }
    bool IsPawn(void* actor) { return actor && (At<uint8_t>(actor, 0x2C) & 0x10) != 0; }

    float DbSum(float a, float b)
    {
        if (a <= 0.0f) return b > 0.0f ? b : 0.0f;
        if (b <= 0.0f) return a;
        return 10.0f * std::log10(std::pow(10.0f, a / 10.0f) + std::pow(10.0f, b / 10.0f));
    }
    // The level a sound of 'db' at 1 m has 'd' units away (6 dB less per doubling of distance).
    float AtDistance(float db, float d) { return db - 20.0f * std::log10(std::max(d, 100.0f) / 100.0f); }
    // Actor.GetSoundRad backwards: <= 40 dB x5, <= 80 dB x10, else x20 units.
    float DbFromRadius(float r) { return r <= 200.0f ? r / 5.0f : r < 1620.0f ? r / 10.0f : r / 20.0f; }

    const char* NoiseName(int type)
    {
        switch (type)
        {
        case kQuietStep: return "quiet footstep";
        case kHeavyStep: return "footstep";
        case 116: return "gunfire (friendly)";
        case 117: return "gunfire";
        case 114: return "explosion";
        case 119: return "grenade";
        case 115: return "shattering";
        case 98: return "glass breaking";
        case 121: return "ricochet";
        case 78: return "object";
        case 63: case 97: return "alarm";
        default: return type >= 0 && type < int(sizeof(kAIEventNames) / sizeof(kAIEventNames[0])) ? kAIEventNames[type] : "noise";
        }
    }
    // Sam's own body noise (footsteps, landings, whistling, his voice, his gunshots): compared with his meter level,
    // which these raise. Guard noise only covers these: a bottle he throws is judged at Sam's spot by the original
    // check, and guards walking around him shouldn't swallow a distraction that lands next to someone else.
    bool BodyNoise(int type)
    {
        switch (type)
        {
        case kQuietStep: case kHeavyStep: case 87: case 59: case 91: case 108: case 116: case 117: return true;
        default: return false;
        }
    }
    bool Masks(int type)
    {
        switch (type)
        {
        case 116: case 117: case 114: case 119: case 115: case 98: case 121: case 78: case 63: case 97: return true;
        default: return false;
        }
    }

    // Loud noises (not Sam's), kept for g_fade seconds.
    struct Event { float t, db, range; Vec at; void* maker; int type; };
    Event g_events[64];
    int g_eventHead = 0, g_eventCount = 0;

    // NPCs, sampled every tick: their footsteps.
    struct Npc { Vec at, vel; float t; bool alive; };
    std::unordered_map<void*, Npc> g_npcs;

    struct Source { float db = 0; void* who = nullptr; int type = -1; float dist = 0; };   // the loudest single source

    // Guard-made noise at a spot (dB), 'self' being the listener (his own noise counts as 1 m away).
    float PollutionAt(const Vec& p, void* self, float now, Source* loudest)
    {
        double sum = 0.0;
        Source best;
        for (int i = 0; i < g_eventCount; ++i)
        {
            const Event& e = g_events[(g_eventHead + 64 - 1 - i) % 64];
            const float age = now - e.t;
            if (age < -0.1f || age > g_fade) continue;
            const float d = e.maker == self ? 0.0f : Dist(p, e.at);
            if (d > e.range) continue;
            // full for the first quarter, then fading to nothing
            const float fade = age <= g_fade * 0.25f ? 1.0f : 1.0f - (age - g_fade * 0.25f) / (g_fade * 0.75f);
            const float l = AtDistance(e.db, d) * fade;
            if (l <= 0.0f) continue;
            sum += std::pow(10.0, l / 10.0);
            if (l > best.db) best = { l, e.maker, e.type, d };
        }
        for (const auto& [pawn, n] : g_npcs)
        {
            if (!n.alive || now - n.t > 0.25f) continue;
            const float speed = Speed2D(n.vel);
            if (speed < kMoving) continue;
            const float d = pawn == self ? 0.0f : Dist(p, n.at);
            const float range = speed > kRunSpeed ? g_stepRange * 1.5f : g_stepRange;
            if (d > range) continue;
            const float l = AtDistance(speed > kRunSpeed ? g_runDb : g_walkDb, d);
            if (l <= 0.0f) continue;
            sum += std::pow(10.0, l / 10.0);
            if (l > best.db) best = { l, pawn, speed > kRunSpeed ? -2 : -1, d };
        }
        if (loudest) *loudest = best;
        return sum > 0.0 ? static_cast<float>(10.0 * std::log10(sum)) : 0.0f;
    }

    void Describe(const Source& s, char* out, size_t size)
    {
        const wchar_t* label = s.who && IsPawn(s.who) ? Guards::Label(s.who) : nullptr;
        const char* what = s.type == -1 ? "footsteps" : s.type == -2 ? "running" : NoiseName(s.type);
        if (s.who && IsPawn(s.who))
            sprintf_s(out, size, "%s of a guard %.1f m away (%ls), %.0f dB", what, s.dist / 100.0f, label ? label : L"no personality", s.db);
        else
            sprintf_s(out, size, "%s %.1f m away, %.0f dB", what, s.dist / 100.0f, s.db);
    }

    // ---- Footstep sync --------------------------------------------------------------------------------------
    struct Sync { void* npc = nullptr; float time = 0; bool active = false; int hidden = 0; float since = 0; };
    Sync g_syncState;
    float g_lastTick = 0;
    float g_stepDb[8]{}; float g_stepT[8]{}; int g_stepHead = 0;   // Sam's recent footstep levels (for the meter)

    float RecentStepPeak(float now)
    {
        float peak = 0.0f;
        for (int i = 0; i < 8; ++i)
            if (now - g_stepT[i] < 1.5f && g_stepDb[i] > peak) peak = g_stepDb[i];
        return peak;
    }

    void UpdateSync(void* sam, float now)
    {
        float dt = now - g_lastTick;
        g_lastTick = now;
        if (dt < 0.0f || dt > 0.25f) dt = 0.0f;
        const Vec& sv = At<Vec>(sam, kVelocity);
        const Vec& sp = At<Vec>(sam, kLocation);
        const float ss = Speed2D(sv);
        void* best = nullptr;
        float bestDist = g_syncRange;
        if (ss > kMoving && At<uint8_t>(sam, kPhysics) == 1)
        {
            const float cosMax = std::cos(g_syncAngle * 3.14159265f / 180.0f);
            for (const auto& [pawn, n] : g_npcs)
            {
                if (!n.alive || now - n.t > 0.25f) continue;
                const float gs = Speed2D(n.vel);
                if (gs < kMoving) continue;
                const float dx = n.at.x - sp.x, dy = n.at.y - sp.y;
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d > bestDist || std::fabs(n.at.z - sp.z) > 150.0f) continue;
                if (std::fabs(ss / gs - 1.0f) > g_syncSpeed) continue;
                if ((sv.x * n.vel.x + sv.y * n.vel.y) / (ss * gs) < cosMax) continue;
                best = pawn;
                bestDist = d;
            }
        }
        // In step after g_syncTime of matching the same guard; out of step kSyncGrace after the match ends.
        Sync& s = g_syncState;
        const bool was = s.active;
        if (best && best == s.npc) s.time = std::min(s.time + dt, g_syncTime);
        else if (best && !s.active) { s.npc = best; s.time = dt; }
        else s.time -= dt;
        if (!s.active)
        {
            if (s.npc && s.time >= g_syncTime) s.active = true;
            else if (s.time < 0.0f) { s.time = 0; s.npc = best; }
        }
        else if (s.time <= g_syncTime - kSyncGrace)
            s.active = false;
        if (s.active != was && g_log && s.npc)
        {
            const wchar_t* label = Guards::Label(s.npc);
            if (s.active)
            {
                s.hidden = 0; s.since = now;
                LOG("[NOISE] Sam fell into step with a guard %.1f m away (%ls, %.0f units/s): his footsteps hide Sam's",
                    Dist(At<Vec>(s.npc, kLocation), sp) / 100.0f, label ? label : L"no personality", Speed2D(At<Vec>(s.npc, kVelocity)));
            }
            else
                LOG("[NOISE] Sam out of step with the guard (%ls) after %.1f s: %d of his footsteps went unheard",
                    label ? label : L"no personality", now - s.since, s.hidden);
        }
        if (!s.active && s.active != was) { s.time = 0; s.npc = best; }
    }

    // ---- Hooks ----------------------------------------------------------------------------------------------
    struct Current { bool active = false; int type = 0; float samLevel = 0; } g_cur;   // Sam's noise being checked

    // Records a loud noise (not Sam's) as a masking source; returns true to drop a footstep of Sam's (in step).
    bool OnNoise(void* actor, void* maker, float radius, int type, const Vec* loc, bool fromSam)
    {
        const float now = LevelTime(actor);
        if (!fromSam && g_on && loc && Masks(type) && radius > 0.0f && radius < 100000.0f)
        {
            Event& e = g_events[g_eventHead];
            g_eventHead = (g_eventHead + 1) % 64;
            if (g_eventCount < 64) ++g_eventCount;
            e = { now, DbFromRadius(radius), radius + 500.0f, *loc, maker, type };
        }
        if (fromSam && (type == kQuietStep || type == kHeavyStep))
        {
            g_stepDb[g_stepHead] = DbFromRadius(radius);
            g_stepT[g_stepHead] = now;
            g_stepHead = (g_stepHead + 1) % 8;
            if (g_sync && !Clusterfuck::Active() && g_syncState.active) { ++g_syncState.hidden; return true; }
        }
        return false;
    }

    float g_origAmbient = 0, g_samPollution = 0;   // Sam's ambient before and the guard noise added at the last meter update
    float g_written = -1;                          // the ambient the last meter update was left with
    Source g_samLoudest;

    void __fastcall hkMakeNoise(void* actor, void* edx, float radius, int type, const Vec* loc, void* inst, int a5, int a6, int a7)
    {
        void* maker = inst ? inst : actor;
        bool fromSam = false, skip = false;
        float samLevel = 0.0f, keptAmbient = 0.0f;
        float* ambient = nullptr;   // put back after a noise that guard noise doesn't cover
        __try
        {
            fromSam = IsSam(maker);
            skip = OnNoise(actor, maker, radius, type & 0xFF, loc, fromSam);
            if (fromSam)
                if (void* pc = At<void*>(maker, kController))
                {
                    samLevel = At<float>(pc, kSamLevel);
                    float& amb = At<float>(pc, kAmbient);
                    if (!BodyNoise(type & 0xFF) && amb == g_written && g_written != g_origAmbient)
                    {
                        ambient = &amb;
                        keptAmbient = amb;
                        amb = g_origAmbient;
                    }
                }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (skip) return;
        const Current saved = g_cur;   // a guard reacting can make a noise of his own inside this one
        g_cur.active = fromSam && BodyNoise(type & 0xFF); g_cur.type = type & 0xFF; g_cur.samLevel = samLevel;
        oMakeNoise(actor, edx, radius, type, loc, inst, a5, a6, a7);
        g_cur = saved;
        if (ambient)
        {
            __try { *ambient = keptAmbient; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    void LogUnheard(void* pawn, const Source& src, float orig, float out)
    {
        static std::unordered_map<void*, float> last;
        if (last.size() > 512) last.clear();
        const float now = LevelTime(pawn);
        float& t = last[pawn];
        if (t != 0.0f && now - t < 2.0f && now >= t) return;
        t = now;
        char what[160];
        Describe(src, what, sizeof(what));
        const wchar_t* label = Guards::Label(pawn);
        LOG("[NOISE] guard %.1f m from Sam (%ls) didn't hear Sam's %s (%.0f dB): drowned out at his spot by %s (masking %.0f -> %.0f dB)",
            Dist(At<Vec>(pawn, kLocation), At<Vec>(g_sam, kLocation)) / 100.0f, label ? label : L"no personality",
            NoiseName(g_cur.type), g_cur.samLevel, what, orig, out);
    }

    float MaskAt(void* pawn, float orig, Source* src)
    {
        if (IsSam(pawn)) return orig;
        return DbSum(orig, PollutionAt(At<Vec>(pawn, kLocation), pawn, LevelTime(pawn), src));
    }

    float __fastcall hkMaskAt(void* audio, void* edx, void* pawn)
    {
        const float orig = oMaskAt(audio, edx, pawn);
        // only while one of Sam's body noises is being checked; CLUSTERFUCK: no cover from guard noise
        if (!g_on || Clusterfuck::Active() || !pawn || !g_cur.active) return orig;
        float out = orig;
        Source src;
        __try { out = MaskAt(pawn, orig, &src); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return orig; }
        if (g_log && g_cur.active && g_sam && out > g_cur.samLevel && orig <= g_cur.samLevel)
        {
            __try { LogUnheard(pawn, src, orig, out); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return out;
    }

    void __fastcall hkUpdate(void* audio, void* edx)
    {
        oUpdate(audio, edx);
        if (!g_on && !g_sync) return;
        __try
        {
            // Single player: the update's local PlayerController is audio +C8h (used when +F0h is 0).
            if (At<void*>(audio, 0xF0)) return;
            void* pc = At<void*>(audio, 0xC8);
            void* sam = pc ? At<void*>(pc, kController) : nullptr;   // Controller.Pawn, same offset
            if (!sam || !IsSam(sam) || sam != g_sam) return;
            float& amb = At<float>(pc, kAmbient);
            if (!(amb >= 0.0f && amb < 200.0f)) return;
            const float now = LevelTime(sam);
            g_origAmbient = amb;
            float v = amb;
            if (g_on && !Clusterfuck::Active())
            {
                g_samPollution = PollutionAt(At<Vec>(sam, kLocation), sam, now, &g_samLoudest);
                v = DbSum(v, g_samPollution);
            }
            if (g_sync && !Clusterfuck::Active() && g_syncState.active)
                v = std::max(v, RecentStepPeak(now) + 1.0f);
            amb = std::min(v, 120.0f);
            g_written = amb;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Logged from the player tick: guard noise around Sam drowning out his noises for everyone (the meter shows it).
    void LogAmbient(void* sam, float now)
    {
        static float lastLog = -100;
        if (now < lastLog) lastLog = -100;
        void* pc = At<void*>(sam, kController);
        if (!pc) return;
        const float samLevel = At<float>(pc, kSamLevel), amb = At<float>(pc, kAmbient);
        const bool drowned = samLevel > 0.0f && amb > samLevel && g_origAmbient <= samLevel && g_samPollution > 0.0f;
        if (!drowned || now - lastLog < 3.0f) return;
        lastLog = now;
        char what[160];
        Describe(g_samLoudest, what, sizeof(what));
        LOG("[NOISE] guard noise around Sam drowns out his %.0f dB: ambient %.0f -> %.0f dB on the meter (loudest: %s)",
            samLevel, g_origAmbient, amb, what);
    }

    float IniFloat(const std::wstring& ini, const wchar_t* key, float def)
    {
        wchar_t buf[64];
        GetPrivateProfileStringW(L"Perception", key, L"", buf, 64, ini.c_str());
        for (wchar_t* p = buf; *p; ++p) if (*p == L';') { *p = 0; break; }
        return buf[0] ? static_cast<float>(_wtof(buf)) : def;
    }
}

void Noise::OnNpcTick(void* pawn)
{
    if ((!g_on && !g_sync) || !pawn) return;
    __try
    {
        if (g_npcs.size() > 1024 && !g_npcs.count(pawn)) return;
        Npc& n = g_npcs[pawn];
        n.at = At<Vec>(pawn, kLocation);
        n.vel = At<Vec>(pawn, kVelocity);
        n.t = LevelTime(pawn);
        n.alive = At<int>(pawn, kHealth) > 0 && At<uint8_t>(pawn, kPhysics) == 1 && At<void*>(pawn, kController) != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Noise::OnPlayerTick(void* pawn)
{
    if (g_decayFixed && pawn)
    {
        __try
        {
            const float now = LevelTime(pawn);
            float dt = now - g_decayLast;
            g_decayLast = now;
            if (dt < 0.0f || dt > 0.25f) dt = 1.0f / g_decayRate;   // a level change, a load, a hitch
            g_decayFrac = 1.0f - std::pow(0.9f, dt * g_decayRate);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if ((!g_on && !g_sync) || !pawn) return;
    g_sam = pawn;
    __try
    {
        void* level = At<void*>(pawn, 0x78);
        const float now = LevelTime(pawn);
        if (level != g_level)
        {
            g_level = level;
            g_npcs.clear();
            g_eventCount = g_eventHead = 0;
            g_syncState = Sync{};
            for (float& t : g_stepT) t = -100.0f;
            g_lastTick = now;
        }
        static float lastPrune = 0;
        if (now - lastPrune > 1.0f || now < lastPrune)
        {
            lastPrune = now;
            for (auto it = g_npcs.begin(); it != g_npcs.end();)
                it = now - it->second.t > 2.0f || now < it->second.t ? g_npcs.erase(it) : std::next(it);
            if (g_syncState.npc && !g_npcs.count(g_syncState.npc)) g_syncState = Sync{};
        }
        if (g_sync) UpdateSync(pawn, now);
        if (g_on && g_log) LogAmbient(pawn, now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Noise::Init()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring ini = path;
    ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"SCCTRemaster_Guards.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) { g_on = g_sync = false; return; }
    g_on = IniFloat(ini, L"NoisePollution", 1) != 0;
    g_sync = IniFloat(ini, L"FootstepSync", 1) != 0;
    g_log = IniFloat(ini, L"LogPerception", 0) != 0;
    g_walkDb = std::clamp(IniFloat(ini, L"GuardWalkNoise", 20.0f), 0.0f, 120.0f);
    g_runDb = std::clamp(IniFloat(ini, L"GuardRunNoise", 35.0f), 0.0f, 120.0f);
    g_stepRange = std::clamp(IniFloat(ini, L"FootstepMaskRange", 8.0f), 0.0f, 100.0f) * 100.0f;
    g_fade = std::clamp(IniFloat(ini, L"GunfireFade", 1.0f), 0.05f, 10.0f);
    g_syncRange = std::clamp(IniFloat(ini, L"SyncRange", 6.0f), 0.5f, 50.0f) * 100.0f;
    g_syncSpeed = std::clamp(IniFloat(ini, L"SyncSpeed", 20.0f), 1.0f, 100.0f) / 100.0f;
    g_syncAngle = std::clamp(IniFloat(ini, L"SyncAngle", 25.0f), 1.0f, 90.0f);
    g_syncTime = std::clamp(IniFloat(ini, L"SyncTime", 0.75f), 0.0f, 10.0f);
    g_decayRate = std::clamp(IniFloat(ini, L"NoiseDecayRate", 30.0f), 0.0f, 1000.0f);
    if (g_decayRate > 0)
    {
        if (const uintptr_t a = Mem::Find("noise level fade", "D9 81 40 04 00 00 D8 0D ?? ?? ?? ?? D8 A9 40 04 00 00 D9 99 40 04 00 00"))
        {
            Mem::Write<uint32_t>(a + 8, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_decayFrac)));
            g_decayFixed = true;
        }
        if (g_decayFixed)
            LOG("Noise: Sam's noise level fades by time whatever the frame rate: %.0f original steps a second (halves every %.2f s)",
                g_decayRate, std::log(0.5f) / std::log(0.9f) / g_decayRate);
        else
            LOG("Noise: Sam's noise level fades per frame as in the original (fade code not found)");
    }
    else
        LOG("Noise: Sam's noise level fades per frame as in the original (faster the higher the frame rate)");
    if (!g_on && !g_sync)
    {
        LOG("Noise: noise pollution and footstep sync off (as in the original)");
        return;
    }

    bool noise = false, mask = false, meter = false;
    if (const uintptr_t a = Mem::Find("make noise", "83 EC 14 8B 44 24 30 85 C0 53 55 56 57 89 4C 24 10 74 5A 8B 41 7C"))
        noise = Mem::Detour("make noise", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkMakeNoise), reinterpret_cast<void**>(&oMakeNoise));
    if (g_on)
        if (const uintptr_t a = Mem::Find("masking at listener", "55 8B EC 83 E4 F8 83 EC 0C 53 8B 5D 08 56 8B F1 8B 8B AC 00 00 00 57 E8"))
            mask = Mem::Detour("masking at listener", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkMaskAt), reinterpret_cast<void**>(&oMaskAt));
    if (const uintptr_t a = Mem::Find("noise meter update", "55 8B EC 83 E4 F8 83 EC 38 53 55 56 8B D9 8B 83 F0 00 00 00 57 33 FF 3B C7 0F 84"))
        meter = Mem::Detour("noise meter update", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkUpdate), reinterpret_cast<void**>(&oUpdate));
    if (!noise)
    {
        g_on = g_sync = false;
        LOG("Noise: MakeNoise not found; noise pollution and footstep sync off");
        return;
    }
    if (g_on)
        LOG("Noise: noise pollution %s: guards' gunfire and loud noises (fading over %.1f s) and footsteps (walking %.0f dB, running %.0f dB, "
            "out to %.0f m) mask Sam's noises%s",
            mask ? "on" : "PARTLY (listener masking not found)", g_fade, g_walkDb, g_runDb, g_stepRange / 100.0f,
            meter ? ", shown on the sound meter" : " (meter update not found: not on the meter, no outright masking)");
    if (g_sync)
        LOG("Noise: footstep sync on: walking at a guard's pace (within %.0f%% speed and %.0f deg, %.0f m) for %.2f s hides Sam's footsteps",
            g_syncSpeed * 100.0f, g_syncAngle, g_syncRange / 100.0f, g_syncTime);
}
