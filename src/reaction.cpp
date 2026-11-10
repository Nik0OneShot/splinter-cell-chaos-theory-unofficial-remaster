// Guard reaction times ([Perception] Reaction in SCCTRemaster_Guards.ini).
//
// Every AI tick a guard rates each enemy he knows of (EAIController, 0x10BB3EF0, thiscall (enemy index)): a line
// check (0x10BB3B40) traces to him and sets bit 1 of the entry's flags (bLineTrace), then the rating (0x10BB2210)
// gives 3 (not seen) or AI_SEE_ENEMY_BARELY 48h / PARTIALLY 5Ah / MOSTLY 65h / ALERT 7Ah / SURPRISED 7Bh.
//  - An enemy he isn't fighting yet: a new rating is sent to him as that event. ALERT also makes Sam his enemy
//    (bit 0 of the flags) and the soldier script attacks in the same moment (bark, cover or open fire); the
//    weaker ratings make him investigate.
//  - An enemy he's fighting (bit 0): the rating only updates what he knows, and his fire state shoots whenever
//    bLineTrace is set.
// EnemyInformation entries: TArray at controller +4F0h (count +4F4h), 64h bytes each: pawn +0,
// LastKnownEnemyTime +10h, LastEnemyVisualEvent +34h, LastTimeVisualEvent +38h, flags +40h.
//
// Here the rating is held at "not seen" (and bLineTrace cleared) until the guard has had Sam in view for his
// reaction time: ReactionMin (the human minimum) plus his personality's Reaction, longer the harder Sam is to make
// out. Once he knows Sam is there (fighting him, or saw/heard him in the last PeekMemory seconds), Sam coming back
// into view after ReactionForget seconds out of it needs PeekReactionFactor x that time, at least PeekReactionMin.
// A first sighting cut short at least half way makes him check where he saw Sam, as a weaker sighting (Glimpse).
//
// Focus ([Perception] Focus): a guard in a fight with another NPC (an enemy on his own list that isn't Sam: the
// Bathhouse fight, or any map or mod where factions fight) puts it first. While an NPC enemy he's engaged with
// was in his line of sight or hurt him in the last FocusMemory seconds, and Sam isn't his current enemy, Sam's
// sightings (any rating, shadows, mirrors) and everything he hears from Sam are dropped. Sam still counts if he's
// within FocusRange metres and within FocusCone degrees of where the guard faces, and for FocusBreak seconds after
// Sam hurts him or shoots near him (damage, a near miss, a ricochet, an explosion or grenade).
#include "pch.h"
#include "features.h"
#include "aievents.h"

extern HMODULE g_self;

namespace
{
    struct Vec { float x, y, z; };
    template <class T> T& At(void* p, uintptr_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p) + off); }

    constexpr uint8_t kNotSeen = 3, kBarely = 0x48, kPartially = 0x5A, kMostly = 0x65, kAlert = 0x7A, kSurprised = 0x7B;
    constexpr uint8_t kSeeOtherEnemy = 0x94;
    constexpr float kContinuity = 0.3f;   // seconds between ratings that still count as the same look
    constexpr float kDecay = 1.0f;        // how fast a half-finished reaction fades while Sam is out of view (per s)

    bool g_on = true, g_glimpse = true, g_log = false;
    float g_min = 0.25f, g_peekFactor = 0.6f, g_peekMin = 0.15f, g_forget = 0.5f, g_peekMemory = 10.0f;
    bool g_focus = true, g_focusReady = false;
    float g_focusMemory = 3.0f, g_focusRange = 400.0f, g_focusCone = 30.0f, g_focusBreak = 5.0f;
    float g_defaultExtra = 0.25f;   // NPCs without a personality: an average guard

    using Dispatch_t = void(__fastcall*)(void* mgr, void* edx, void* ctrl, void* ev);
    Dispatch_t g_dispatch = nullptr;
    void* g_sam = nullptr;

    struct State
    {
        void* level = nullptr;
        float progress = 0;      // 0..1 towards reacting
        float rt = 0;            // this look's reaction time
        float lastSeen = -100;   // level time of the last rating that saw Sam
        float start = 0;
        float lastGlimpse = -100;
        uint8_t best = 0;        // strongest rating during this look
        bool reacted = false, peek = false, ended = false;
        Vec seenAt{};            // where Sam's eyes were when last seen
        int idx = -1;
        DWORD aiTick = 0;        // last time his AI ran its sight checks (an unconscious guard's doesn't)
    };
    std::unordered_map<void*, State> g_state;

    // The line check runs every AI tick for the guard's current enemy, whether or not Sam is in view.
    using LineCheck_t = int(__fastcall*)(void* ctrl, void* edx, int idx);
    LineCheck_t oLineCheck = nullptr;
    struct Focus
    {
        void* level = nullptr;
        float npcContact = -100;   // last time an NPC enemy he's engaged with was in his sights or hurt him
        float samBreak = -100;     // last time Sam hurt him or shot near him
        void* npc = nullptr;       // that enemy
        bool on = false;           // focused at the last check (for the log)
    };
    std::unordered_map<void*, Focus> g_focusState;
    void LineContact(void* ctrl, int idx);

    int __fastcall hkLineCheck(void* ctrl, void* edx, int idx)
    {
        if (const auto it = g_state.find(ctrl); it != g_state.end()) it->second.aiTick = GetTickCount();
        const int r = oLineCheck(ctrl, edx, idx);
        if (g_focusReady)
        {
            __try { LineContact(ctrl, idx); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return r;
    }

    uint32_t g_rng = 0x9E3779B9u;
    float Rand()
    {
        g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
        return float(g_rng & 0xFFFFFF) / float(0x1000000);
    }

    // How much longer it takes to make Sam out, by how well the guard sees him.
    float VisScale(uint32_t vis)
    {
        if (vis >= kAlert) return 1.0f;
        if (vis >= kMostly) return 1.25f;
        if (vis >= kPartially) return 1.6f;
        return 2.2f;
    }
    const char* VisName(uint32_t vis)
    {
        switch (vis)
        {
        case kBarely: return "barely visible";
        case kPartially: return "partly visible";
        case kMostly: return "mostly visible";
        case kAlert: return "fully visible";
        case kSurprised: return "right in front of him";
        default: return "?";
        }
    }

    // Where the sight check puts a seen enemy: his location, raised to eye height (+224h).
    Vec EyePos(void* pawn)
    {
        const Vec& p = At<Vec>(pawn, 0xE8);
        return { p.x, p.y, p.z + At<float>(pawn, 0x224) };
    }

    float Dist(void* a, void* b)
    {
        const Vec& p = At<Vec>(a, 0xE8);
        const Vec& q = At<Vec>(b, 0xE8);
        return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z));
    }

    const wchar_t* Label(void* pawn)
    {
        const wchar_t* l = Guards::Label(pawn);
        return l ? l : L"no personality";
    }

    uint8_t* Entry(void* ctrl, int idx)
    {
        uint8_t* arr = At<uint8_t*>(ctrl, 0x4F0);
        const int n = At<int>(ctrl, 0x4F4);
        if (!arr || idx < 0 || idx >= n || n > 64) return nullptr;
        return arr + idx * 0x64;
    }

    bool IsSam(void* pawn) { return pawn && (At<uint8_t>(pawn, 0x2C) & 0x20) != 0; }
    float LevelTime(void* ctrl)
    {
        void* level = At<void*>(ctrl, 0x78);
        return level ? At<float>(level, 0x430) : 0.0f;
    }
    Focus& FocusOf(void* ctrl)
    {
        Focus& f = g_focusState[ctrl];
        void* level = At<void*>(ctrl, 0x78);
        if (f.level != level) { f = Focus(); f.level = level; }
        return f;
    }
    // An NPC on the guard's enemy list (not Sam), alive.
    bool NpcEnemy(void* ctrl, void* pawn)
    {
        if (!pawn || IsSam(pawn) || At<int>(pawn, 0x420) <= 0) return false;
        uint8_t* arr = At<uint8_t*>(ctrl, 0x4F0);
        const int n = At<int>(ctrl, 0x4F4);
        for (int i = 0; arr && i < n && i < 64; ++i)
            if (At<void*>(arr + i * 0x64, 0) == pawn) return true;
        return false;
    }
    void LineContact(void* ctrl, int idx)
    {
        uint8_t* e = Entry(ctrl, idx);
        if (!e) return;
        void* pawn = At<void*>(e, 0);
        const uint32_t flags = At<uint32_t>(e, 0x40);
        if (!pawn || IsSam(pawn) || At<int>(pawn, 0x420) <= 0 || (flags & 3) != 3) return;   // engaged and in his sights
        Focus& f = FocusOf(ctrl);
        f.npcContact = LevelTime(ctrl);
        f.npc = pawn;
    }

    bool SafeName(void* obj, wchar_t* buf)
    {
        if (!obj || !Game::GetFullName) return false;
        __try { Game::GetFullName(obj, nullptr, buf); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool Focused(void* ctrl, float t)
    {
        if (!g_focusReady) return false;
        const auto it = g_focusState.find(ctrl);
        if (it == g_focusState.end()) return false;
        Focus& f = it->second;
        void* enemy = At<void*>(ctrl, 0x320);
        const bool on = t - f.npcContact <= g_focusMemory && t - f.samBreak > g_focusBreak && !IsSam(enemy) &&
                        f.npc && At<int>(f.npc, 0x420) > 0;
        if (on != f.on)
        {
            f.on = on;
            void* pawn = At<void*>(ctrl, 0x41C);
            if (g_log && pawn)
            {
                const wchar_t* l = Guards::Label(pawn);
                if (on)
                {
                    wchar_t name[256] = L"";
                    SafeName(f.npc, name);
                    LOG("[FOCUS] guard (%ls) is fighting %ls: he ignores Sam unless Sam is within %.0f m in front of him or shoots at him",
                        l ? l : L"no personality", name[0] ? name : L"another NPC", g_focusRange / 100.0f);
                }
                else
                    LOG("[FOCUS] guard (%ls) no longer focused on his fight%s", l ? l : L"no personality",
                        t - f.samBreak <= g_focusBreak ? " (Sam hurt him or shot near him)" : "");
            }
        }
        return on;
    }

    // Sam right in front of a focused guard: within FocusRange, within FocusCone of where he faces.
    bool InHisFace(void* pawn, void* sam)
    {
        const Vec& g = At<Vec>(pawn, 0xE8);
        const Vec& s = At<Vec>(sam, 0xE8);
        const float dx = s.x - g.x, dy = s.y - g.y, dz = s.z - g.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > g_focusRange) return false;
        const float flat = std::sqrt(dx * dx + dy * dy);
        if (flat < 1.0f) return true;
        const float yaw = static_cast<float>(At<int>(pawn, 0xF8) & 0xFFFF) * (3.14159265f / 32768.0f);
        const float c = (dx * std::cos(yaw) + dy * std::sin(yaw)) / flat;
        return c >= std::cos(g_focusCone * 3.14159265f / 180.0f);
    }

    void NewLook(State& s, void* ctrl, uint8_t* e, float t, bool engaged)
    {
        void* pawn = At<void*>(ctrl, 0x41C);
        float extra = pawn ? Guards::ReactionExtra(pawn) : -1.0f;
        if (extra < 0) extra = g_defaultExtra;
        extra *= 0.9f + 0.2f * Rand();                 // a little different every time
        const float lket = At<float>(e, 0x10);         // LastKnownEnemyTime
        const bool recent = lket > 0 && t - lket < g_peekMemory;
        const bool onEdge = lket > 0 && t - lket < 120.0f;
        s.peek = engaged || recent;
        if (s.peek)
            s.rt = std::max(g_peekMin, (g_min + extra) * g_peekFactor);
        else
            s.rt = g_min + extra * (onEdge ? 0.7f : 1.0f);   // he knows someone's around: a bit quicker, never below the minimum
        s.rt = std::max(s.rt, 0.05f);
        s.progress = 0;
        s.best = 0;
        s.start = t;
        s.ended = false;
    }

    // At 0x10BB3F2D, right after the rating (ebx = rating, esi = controller, ebp = enemy index).
    void Rate(Mem::Regs* r)
    {
        void* ctrl = reinterpret_cast<void*>(r->esi);
        const int idx = static_cast<int>(r->ebp);
        if (!ctrl) return;
        uint8_t* e = Entry(ctrl, idx);
        if (!e) return;
        void* target = At<void*>(e, 0);
        if (!target || !(At<uint8_t>(target, 0x2C) & 0x20)) return;   // Sam only
        g_sam = target;
        void* level = At<void*>(ctrl, 0x78);
        if (!level) return;
        const float t = At<float>(level, 0x430);
        const uint32_t vis = r->ebx;
        if (vis < kBarely || vis > kSurprised) return;   // not seen: nothing to hold

        if (Focused(ctrl, t))
        {
            void* pawn = At<void*>(ctrl, 0x41C);
            if (pawn && !InHisFace(pawn, target))
            {
                r->ebx = kNotSeen;
                At<uint32_t>(e, 0x40) &= ~2u;
                static DWORD lastLog = 0;
                if (g_log && GetTickCount() - lastLog > 3000)
                {
                    lastLog = GetTickCount();
                    LOG("[FOCUS] guard %.1f m from Sam (%ls) busy fighting: didn't notice Sam (%s)", Dist(pawn, target) / 100.0f,
                        Label(pawn), VisName(vis));
                }
                return;
            }
        }
        if (!g_on) return;

        State& s = g_state[ctrl];
        if (s.level != level) { s = State(); s.level = level; }
        const bool engaged = (At<uint32_t>(e, 0x40) & 1) != 0;
        const float gap = t - s.lastSeen;

        if (s.reacted)
        {
            if (gap <= g_forget)
            {
                s.lastSeen = t;
                s.seenAt = EyePos(target);
                return;   // still has him: the rating goes through
            }
            s.reacted = false;   // out of view long enough: he has to pick Sam up again
            s.progress = 0;
        }

        float dt = 0;
        if (gap > kContinuity)
        {
            s.progress = std::max(0.0f, s.progress - gap * kDecay);   // what's left of an earlier look
            if (s.progress <= 0) NewLook(s, ctrl, e, t, engaged);
        }
        else
            dt = gap;
        s.progress += dt / (s.rt * VisScale(vis));
        s.best = std::max<uint8_t>(s.best, static_cast<uint8_t>(vis));
        s.ended = false;
        s.lastSeen = t;
        s.idx = idx;
        s.seenAt = EyePos(target);

        if (s.progress >= 1.0f)
        {
            s.reacted = true;
            if (g_log)
            {
                void* pawn = At<void*>(ctrl, 0x41C);
                if (pawn)
                    LOG("[REACT] guard %.1f m from Sam (%ls)%s: reacted after %.2f s (his time %.2f s; Sam %s, x%.2f)",
                        Dist(pawn, target) / 100.0f, Label(pawn), s.peek ? " picked Sam up again" : " saw Sam", t - s.start, s.rt,
                        VisName(vis), VisScale(vis));
            }
            return;
        }
        // Not yet: as if he hasn't seen Sam.
        r->ebx = kNotSeen;
        At<uint32_t>(e, 0x40) &= ~2u;   // bLineTrace: his fire state doesn't shoot either
    }

    void __cdecl OnSightRated(Mem::Regs* r)
    {
        if (!g_on && !g_focusReady) return;
        __try { Rate(r); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // A first look cut short: he checks where he saw Sam, as the event the game sends for a weaker sighting (the
    // same fields the sight check fills in before sending one).
    bool SendGlimpse(void* ctrl, State& s, float t)
    {
        void* sam = g_sam;
        void* pawn = At<void*>(ctrl, 0x41C);
        if (!g_dispatch || !sam || !pawn) return false;
        if (At<int>(pawn, 0x420) <= 0 || (At<uint32_t>(ctrl, 0x514) & 2)) return false;   // dead, or not responding
        if (!oLineCheck || GetTickCount() - s.aiTick > 500) return false;                   // his AI isn't looking (knocked out...)
        uint8_t* e = Entry(ctrl, s.idx);
        if (!e || At<void*>(e, 0) != sam || (At<uint32_t>(e, 0x40) & 1)) return false;
        const uint8_t type = s.best >= kMostly ? kMostly : s.best;
        At<uint8_t>(e, 0x34) = type;
        At<float>(e, 0x38) = t;
        At<void*>(ctrl, 0x468) = sam;
        At<Vec>(ctrl, 0x438) = s.seenAt;
        uint8_t* ev = reinterpret_cast<uint8_t*>(ctrl) + 0x430;
        if (At<void*>(ctrl, 0x320) == sam) ev[0] = type;
        else { At<uint8_t>(ctrl, 0x4B4) = type; ev[0] = kSeeOtherEnemy; }
        g_dispatch(At<void*>(ctrl, 0x424), nullptr, ctrl, ev);
        return true;
    }

    void Tick()
    {
        // Looks that just ended (Sam out of view before the guard reacted). Handled after the loop: sending a
        // glimpse runs the guard's event handling, which may rate Sam again and add to g_state.
        void* ended[16];
        int n = 0;
        for (auto& [ctrl, s] : g_state)
        {
            if (s.reacted || s.ended || s.progress <= 0) continue;
            void* level = s.level;
            if (!level || At<void*>(ctrl, 0x78) != level) { s.ended = true; continue; }
            const float gone = At<float>(level, 0x430) - s.lastSeen;
            if (gone < kContinuity) continue;   // still in view
            s.ended = true;
            if (gone <= 2.0f && n < 16) ended[n++] = ctrl;
        }
        for (int i = 0; i < n; ++i)
        {
            void* ctrl = ended[i];
            const auto it = g_state.find(ctrl);
            if (it == g_state.end()) continue;
            State& s = it->second;
            const float t = At<float>(s.level, 0x430);
            const bool may = g_glimpse && !s.peek && s.progress >= 0.5f && t - s.lastGlimpse > 2.0f;
            const bool glimpse = may && SendGlimpse(ctrl, s, t);
            if (glimpse) s.lastGlimpse = t;
            void* pawn = At<void*>(ctrl, 0x41C);
            if (g_log && pawn && g_sam)
                LOG("[REACT] guard %.1f m from Sam (%ls): Sam out of view after %.2f s of his %.2f s (%.0f%% there)%s",
                    Dist(pawn, g_sam) / 100.0f, Label(pawn), s.lastSeen - s.start, s.rt, s.progress * 100.0f,
                    glimpse ? ", a glimpse: he checks where he saw him" : (s.peek ? ", didn't pick him up again" : ", not noticed"));
        }
    }

    // Events that mean "someone is shooting at me / hurting me".
    bool ShotAt(uint8_t type)
    {
        return type == 126 /*TAKE_DAMAGE*/ || type == 150 /*TAKE_DAMAGE_OTHER_ENEMY*/ || type == 120 /*SHOT_JUST_MISSED*/ ||
               type == 121 /*HEAR_RICOCHET*/ || type == 114 /*HEAR_EXPLOSION*/ || type == 119 /*HEAR_GRENADE*/ ||
               type == 124 /*CHANGED_GRENADE*/ || type == 104 /*HIT_BY_OBJECT*/;
    }

    bool Allow(void* ctrl, const uint8_t* ev)
    {
        const uint8_t type = ev[0];
        void* about = *reinterpret_cast<void* const*>(ev + 0x38);
        if (!about) return true;
        const float t = LevelTime(ctrl);
        if (IsSam(about))
        {
            if (ShotAt(type)) { FocusOf(ctrl).samBreak = t; Focused(ctrl, t); return true; }
            if (!Focused(ctrl, t)) return true;
            void* pawn = At<void*>(ctrl, 0x41C);
            if (pawn && type != 58 /*SEE_ENEMY_SHADOW*/ && InHisFace(pawn, about)) return true;
            static std::unordered_map<uint64_t, DWORD> last;
            if (last.size() > 2048) last.clear();
            DWORD& l = last[(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ctrl)) << 8) | type];
            if (g_log && pawn && GetTickCount() - l > 3000)
            {
                l = GetTickCount();
                const char* name = type < sizeof(kAIEventNames) / sizeof(kAIEventNames[0]) ? kAIEventNames[type] : "?";
                LOG("[FOCUS] guard %.1f m from Sam (%ls) busy fighting: ignored %s (%u) about Sam", Dist(pawn, about) / 100.0f,
                    Label(pawn), name, unsigned(type));
            }
            return false;
        }
        // Hurt or shot at by an NPC enemy: he's in a fight.
        if (ShotAt(type) && NpcEnemy(ctrl, about))
        {
            Focus& f = FocusOf(ctrl);
            f.npcContact = t;
            f.npc = about;
        }
        return true;
    }

    float IniFloat(const std::wstring& ini, const wchar_t* key, float def)
    {
        wchar_t buf[64];
        GetPrivateProfileStringW(L"Perception", key, L"", buf, 64, ini.c_str());
        for (wchar_t* p = buf; *p; ++p) if (*p == L';') { *p = 0; break; }
        return buf[0] ? static_cast<float>(_wtof(buf)) : def;
    }
}

bool Reaction::WantsEvents() { return g_focusReady; }

bool Reaction::AllowEvent(void* ctrl, const uint8_t* ev)
{
    if (!g_focusReady || !ctrl || !ev) return true;
    __try { return Allow(ctrl, ev); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

void Reaction::OnPlayerTick(void* pawn)
{
    if ((!g_on && !g_focusReady) || !pawn) return;
    g_sam = pawn;
    static void* level = nullptr;
    void* lv = At<void*>(pawn, 0x78);
    if (lv != level) { level = lv; g_state.clear(); g_focusState.clear(); }
    if (!g_on) return;
    __try { Tick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Reaction::Init()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring ini = path;
    ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"SCCTRemaster_Guards.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) { g_on = g_focus = false; return; }
    g_on = IniFloat(ini, L"Reaction", 1) != 0;
    g_focus = IniFloat(ini, L"Focus", 1) != 0;
    g_log = IniFloat(ini, L"LogPerception", 0) != 0;
    g_min = std::clamp(IniFloat(ini, L"ReactionMin", 0.25f), 0.0f, 2.0f);
    g_peekFactor = std::clamp(IniFloat(ini, L"PeekReactionFactor", 0.6f), 0.0f, 2.0f);
    g_peekMin = std::clamp(IniFloat(ini, L"PeekReactionMin", 0.15f), 0.0f, 2.0f);
    g_forget = std::clamp(IniFloat(ini, L"ReactionForget", 0.5f), 0.05f, 10.0f);
    g_peekMemory = std::clamp(IniFloat(ini, L"PeekMemory", 10.0f), 0.0f, 600.0f);
    g_glimpse = g_on && IniFloat(ini, L"Glimpse", 1) != 0;
    g_focusMemory = std::clamp(IniFloat(ini, L"FocusMemory", 3.0f), 0.1f, 60.0f);
    g_focusRange = std::clamp(IniFloat(ini, L"FocusRange", 4.0f), 0.0f, 100.0f) * 100.0f;
    g_focusCone = std::clamp(IniFloat(ini, L"FocusCone", 30.0f), 0.0f, 180.0f);
    g_focusBreak = std::clamp(IniFloat(ini, L"FocusBreak", 5.0f), 0.0f, 120.0f);
    g_rng ^= GetTickCount();
    if (!g_on && !g_focus)
    {
        LOG("Reaction: reaction times and focus off (as in the original)");
        return;
    }

    if (const uintptr_t a = Mem::Find("ai event dispatch", "55 8B EC 83 E4 F8 81 EC AC 00 00 00 53 8B 5D 08 85 DB 56 8B 71 78 57"))
        g_dispatch = reinterpret_cast<Dispatch_t>(a);
    // The rating call in the sight check, then 'mov ebx,eax; mov eax,[esi+41Ch]' (8 bytes, replayed first).
    const uintptr_t c = Mem::Find("sight rating", "E8 ? ? ? ? 8B D8 8B 86 1C 04 00 00 F6 80 BC 09 00 00 01 75 ? 83 FB 7A");
    if (!c || !Mem::MidHook("sight rating", c + 5, 8, &OnSightRated, Mem::MidMode::OriginalBefore))
    {
        g_on = g_focus = false;
        LOG("Reaction: sight check not found, instant reactions and no focus, as in the original");
        return;
    }
    bool lineCheck = false;
    if (g_glimpse || g_focus)
        if (const uintptr_t a = Mem::Find("enemy line check", "83 EC 70 53 55 56 57 8B F1 33 C0 33 D2 0F 31 89 44 24 14 89 54 24 10 DB 44 24 10"))
            lineCheck = Mem::Detour("enemy line check", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkLineCheck), reinterpret_cast<void**>(&oLineCheck));
    // Focus also needs the AI event dispatcher (hooked by Perception::Init, which asks WantsEvents()).
    g_focusReady = g_focus && lineCheck && g_dispatch;
    if (g_on)
        LOG("Reaction: guards need %.2f s + their personality's reaction time to react to Sam; picking him up again %.0f%% of that "
            "(at least %.2f s) after %.1f s out of view; glimpses %s",
            g_min, g_peekFactor * 100.0f, g_peekMin, g_forget, g_glimpse && lineCheck ? "make them check" : "ignored");
    else
        LOG("Reaction: reaction times off (instant, as in the original)");
    if (g_focus)
        LOG("Reaction: focus %s: a guard fighting another NPC ignores Sam unless he's within %.0f m and %.0f deg in front of him, "
            "and for %.0f s after Sam shoots at him", g_focusReady ? "on" : "FAILED", g_focusRange / 100.0f, g_focusCone, g_focusBreak);
}
