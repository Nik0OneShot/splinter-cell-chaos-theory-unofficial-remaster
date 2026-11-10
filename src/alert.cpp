// Guards draw before they shoot ([Perception] DrawTime) and call it in before the rest of their group knows
// ([Perception] CallOutTime) in SCCTRemaster_Guards.ini.
//
// Shooting: every NPC shot goes through EAIController 0x10BADBE0 (thiscall (target, 5 more), ret 18h). While
// Level.TimeSeconds < NextFireTime (+950h) it only clears bFiring (+93Ch bit 0) and returns 1 (the half second
// between single shots); otherwise it sets the next time, aims (+940h) and calls the weapon's Fire event
// (0x10B9D950). The shoot goal (0x10BB0FD0) calls it every tick once the guard's aim is on (EPawn +744h bit
// 40000000h) and fails the goal if it returns 0. The soldier script attacks the moment a guard rates Sam
// ALERT/SURPRISED (open fire at once within 5 m with a line to him), so the shot came in the same instant.
// Here a guard's first shot at Sam waits until DrawTime seconds (x his reaction, see Scale) after he spotted
// him, and at least DrawAimTime after his gun first came up on Sam: NextFireTime is pushed to then, so the
// native's own between-shots wait holds him, aiming. After DrawReady seconds without trying to shoot Sam, the
// next time needs the draw again.
//
// Group alert: every UpdateEnemy (0x10B9E690) of a guard's, from the moment he rates Sam ALERT and for every look
// after that, calls the AI manager's 0x10BDD810 (thiscall (enemy, controller, shareOnly, a4, a5), ret 14h). For
// every NPC in the same AI zone (EPawn +428h: pawns +2B4h, count +2B8h) and group (EPawn GroupID +430h) it
// copies what this guard knows of Sam (0x10B9EAC0) and sends AI_BACKUP_ATTACK to each one whose pattern isn't
// already attacking (controller +42Ch, bInAttack +410h bit 10h): the whole area knew in the same frame.
// (Other callers: the patterns' deductions 0x1093BE70 (shareOnly, a4 0: position only) and 0x10BE0150, which
// moves a guard into a fighting group and passes on his enemy (shareOnly 1, a4 1, a5 0: backup requests too).)
// Here a guard who finds Sam keeps it to himself for CallOutTime seconds (x his reaction): his call-out. Knock
// him out, kill or grab him before then and nobody hears it from him (gunfire is still heard); a hit holds it
// back until his flinch is over. Still standing, the broadcast goes out with what he knows then, and his
// updates go out at once while he keeps Sam. Once he hasn't had Sam for CallOutAgain seconds, finding him again
// is a new call-out, even for a guard who is in a fight (dev.88 and before let a fighting guard's re-sighting
// out at once: kill him in the next second and his mates already knew).
#include "pch.h"
#include "features.h"

extern HMODULE g_self;

namespace
{
    struct Vec { float x, y, z; };
    template <class T> T& At(void* p, uintptr_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p) + off); }

    constexpr uintptr_t kLevel = 0x78, kXLevel = 0x7C, kTime = 0x430, kLocation = 0xE8, kController = 0x2B4, kHealth = 0x420;
    constexpr uintptr_t kEPawn = 0x41C, kAIManager = 0x424, kPattern = 0x42C, kEnemyInfo = 0x4F0, kEnemyCount = 0x4F4;
    constexpr uintptr_t kNextFireTime = 0x950, kPatternFlags = 0x410, kZone = 0x428, kGroupID = 0x430;
    constexpr uintptr_t kZonePawns = 0x2B4, kZoneCount = 0x2B8;
    constexpr uint32_t kInAttack = 0x10;
    constexpr float kReactMin = 0.25f, kDefaultExtra = 0.25f;

    constexpr uintptr_t kFlags744 = 0x744, kFocusWithGun = 0xA28, kWaitAnim = 0xF34, kMeshInstance = 0x64;
    constexpr uint32_t kGunUp = 0x40000000;

    bool g_log = false;
    float g_draw = 1.0f, g_drawAim = 0.35f, g_drawReady = 5.0f;
    float g_callOut = 2.5f, g_callAgain = 3.0f;
    bool g_turnFix = true;
    float g_turnHz = 30.0f, g_drawTurn = 0.6f;
    bool g_turnHooked = false;
    float g_raise = 0.7f, g_raiseDelay = 0.1f, g_drawAnim = 0.6f;
    // Above everything the game blends: native code uses channels 0-18 (11/12 a head layer, 13/14 faces, 15-18
    // upper-body layers such as the relaxed walk arms), scripts 1-7, 10, 15, 18.
    constexpr int kRaiseChannel = 19;
    constexpr uintptr_t kPawnFlags2B8 = 0x2B8, kWaitGD = 0xF38, kWaitCrouchGD = 0xF50, kHolsterGun = 0x149C;
    constexpr uintptr_t kHandItem = 0x6E0, kCurrentWeapon = 0x6EC, kHandedness = 0x6F4, kActorFlags30 = 0x30;
    void* g_blendCode = nullptr;        // EPawn.BlendAnimOverCurrent (native 1538)
    int g_boneSpine = 0;
    bool g_raiseLookup = false;
    // Combat: reaction on re-sighting, hit chance, sprint windup, stance blend.
    float g_combatReact = 1.0f, g_reacquire = 0.3f, g_aimSettle = 1.2f, g_aimTrack = 0.8f, g_aimMoving = 0.6f;
    bool g_firstShotHit = false, g_fairRoll = true;
    float g_minShot = 0.066f;   // NPCShotInterval: least time between an NPC's shots
    // CLUSTERFUCK: guards draw in half the time, react in 0.6x, settle their aim twice as fast, call it in in a
    // quarter of the time (to the whole level) and never fire warning shots.
    inline float CfMul(float m) { return Clusterfuck::Active() ? m : 1.0f; }
    float g_duck = 1.2f, g_duckLevel = 0.6f;   // SuppressionDuck (s down), SuppressionDuckLevel
    float g_lastChance = -1.0f;   // the chance of the shot being decided (OnHitChance), -1 none
    constexpr uintptr_t kHitsInLast10 = 0x98C;
    float g_runAccel = 450.0f, g_stanceBlend = 0.35f;
    // Flinch, suppression, warning shots, darkness and goggles, wounds.
    float g_flinch = 0.6f, g_supAim = 0.6f, g_supHalf = 1.2f, g_supPin = 0.3f;
    float g_warnChance = 0.5f, g_warnRange = 800.0f, g_warnPause = 0.8f;
    float g_darkAim = 0.7f, g_woundAim = 0.5f;
    float g_samHostile = -1000.0f;   // last time Sam hurt a guard or shot near one (level time)

    using Shoot_t = int(__fastcall*)(void* ctrl, void* edx, void* target, int a2, int a3, int a4, int a5, int a6);
    using Broadcast_t = void(__fastcall*)(void* mgr, void* edx, void* enemy, void* ctrl, int shareOnly, int a4, int a5);
    Shoot_t oShoot = nullptr;
    Broadcast_t oBroadcast = nullptr;

    struct Guard
    {
        void* level = nullptr;
        float touched = -1000;                       // last activity (level time)
        float engaged = -1000, lastTrack = -1000;    // his own updates about Sam: first of this run, last
        float lastShot = -1000, holdUntil = 0, holdFrom = 0;
        bool holding = false;
        bool pending = false, released = false;
        float spotted = 0, due = 0, lastShare = -1000;
        void* enemy = nullptr;
        int a5 = 1;
        float lastTurn[3] = { -1000, -1000, -1000 }; // last facing step, per place that turns him
        float sampleNext = 0, sampleFrom = 0;        // [DRAW] animation samples (LogPerception)
        int samples = 0;
        bool gunUpAtSpot = true;                     // his gun was already up when he spotted Sam
        bool handAtSpot = true;                      // he had it in his hands (HandItem) when he spotted Sam
        int raise = 0;                               // gun raise: 0 none, 1 to start, 2 playing
        float raiseStart = 0, drawDur = 0, raiseHold = 0, raiseDur = 0;
        void* raisePawn = nullptr;
        int raiseAnim = 0;
        bool vis = false;                            // line of sight to Sam at his last shot attempt
        float lastVis = -1000, acquired = -1000;     // last attempt with Sam in sight; when he last got him in sight
        float aimFrom = -1000;                       // his gun is up and on Sam from here (end of a raise)
        float lastFired = -1000, cappedLog = -1000;  // his last shot (any target); last fire-rate log line
        float duckUntil = 0;                         // ducking under suppressive fire until then
        int capped = 0;
        float flinchUntil = 0, pinUntil = 0;         // no firing: hit by Sam / shots landing around him
        float supp = 0, suppT = 0;                   // suppression (0..1) at suppT, halving every SuppressionDecay s
        bool warnPending = false; float warnEnd = 0; // a warning burst: its shots miss until warnEnd
        int maxHealth = 0;
    };
    std::unordered_map<void*, Guard> g_guards;   // by controller
    void* g_sam = nullptr;
    void* g_xlevel = nullptr;

    float LevelTime(void* actor)
    {
        void* level = At<void*>(actor, kLevel);
        return level ? At<float>(level, kTime) : 0.0f;
    }
    bool IsSam(void* actor) { return actor && (At<uint8_t>(actor, 0x2C) & 0x20) != 0; }
    float Dist(const Vec& a, const Vec& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

    // His personality's reaction (ReactionMin + Reaction) against an average guard's 0.5 s: quick guards draw
    // and talk sooner.
    float Scale(void* pawn)
    {
        float extra = pawn ? Guards::ReactionExtra(pawn) : -1.0f;
        if (extra < 0) extra = kDefaultExtra;
        return std::clamp((kReactMin + extra) / 0.5f, 0.6f, 1.6f);
    }

    Guard& Get(void* ctrl, float now)
    {
        Guard& g = g_guards[ctrl];
        void* level = At<void*>(ctrl, kLevel);
        if (g.level != level || now + 1.0f < g.touched)   // another level, or time went back (a load)
        {
            g = Guard{};
            g.level = level;
        }
        g.touched = now;
        return g;
    }

    void Who(void* pawn, char* out, size_t len)
    {
        float d = -1;
        if (g_sam && pawn) d = Dist(At<Vec>(pawn, kLocation), At<Vec>(g_sam, kLocation)) / 100.0f;
        const wchar_t* label = pawn ? Guards::Label(pawn) : nullptr;
        _snprintf_s(out, len, _TRUNCATE, "guard %.1f m from Sam (%ls)", d, label ? label : L"no personality");
    }

    bool Fighting(void* ctrl)
    {
        void* pattern = At<void*>(ctrl, kPattern);
        return pattern && (At<uint32_t>(pattern, kPatternFlags) & kInAttack) != 0;
    }

    // ---- turning ----
    //
    // Guards turn to face their focus by covering a quarter of the angle left on every AI tick: a quaternion blend
    // 0x10CFF090 (cdecl (out, keep 0.75, current rotation, wanted rotation)) then MoveActor, in three places: the
    // shoot goal's turn (0x10BA4CE0, controller in ebx), the goal animation/aim step that the shoot goal (and most
    // others) run right after it (0x10BABCB0, esi) and another goal's turn (0x10BAF6D0, edi). Tuned for the
    // consoles' 30 fps (one step: 90% of a turn in 0.27 s; shooting, turn + aim step: 0.13 s), it is per tick, so at
    // 160 fps a guard swings round several times faster. Here each place keeps 0.75^(dt x TurnRate) for the time
    // since its own last step (at most two frames: a goal that starts turning after a pause doesn't jump), and
    // while a guard is drawing on Sam (his first DrawTime seconds after spotting him, or holding fire) x
    // DrawTurnSpeed on top.
    float g_frameDt = 1.0f / 60.0f;   // level time per frame, from Sam's ticks
    // Shooting at a spot rather than a pawn (suppressive fire), the shoot goal re-scatters its aim point (+-30
    // units around it) with a 20% chance every AI tick (0x10BAE615: FRand < [0x110BCAA8] 0.2): five times as
    // often at 160 fps. The compare's constant is swapped for g_aimRoll = 1 - 0.8^(dt x TurnRate).
    float g_aimRoll = 0.2f;

    bool Drawing(const Guard& g, void* pawn, float now)
    {
        if (g.holding && now < g.holdUntil) return true;
        return g_draw > 0 && g.engaged > -999.0f && now - g.lastTrack <= g_drawReady &&
               g.lastShot < g.engaged + 0.001f && now < g.engaged + g_draw * CfMul(0.5f) * Scale(pawn);
    }

    void OnTurn(void* ctrl, Mem::Regs* r, int site)
    {
        float& keep = r->Stack<float>(4);
        if (!ctrl || keep != 0.75f) return;
        void* pawn = At<void*>(ctrl, kEPawn);
        const float now = LevelTime(ctrl);
        Guard& g = Get(ctrl, now);
        float dt = now - g.lastTurn[site];
        g.lastTurn[site] = now;
        if (dt == 0.0f) { keep = 1.0f; return; }   // a second step from the same place in the same tick
        if (dt < 0.0f) dt = g_frameDt;
        dt = std::min(dt, 2.0f * g_frameDt);
        float rate = g_turnHz;
        if (pawn && g_drawTurn < 1.0f && Drawing(g, pawn, now)) rate *= g_drawTurn;
        keep = std::pow(0.75f, dt * rate);
    }

    void __cdecl OnTurnShoot(Mem::Regs* r)
    {
        __try { OnTurn(reinterpret_cast<void*>(r->ebx), r, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    void __cdecl OnTurnAim(Mem::Regs* r)
    {
        __try { OnTurn(reinterpret_cast<void*>(r->esi), r, 1); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    void __cdecl OnTurnGoal(Mem::Regs* r)
    {
        __try { OnTurn(reinterpret_cast<void*>(r->edi), r, 2); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // UAV camera head: EUAVDrone's look update (0x10C6FFD0, the drone in esi) turns PrevLookRot (+16ECh) toward its
    // focus with fixedTurn (0x10CF8490) by a step per tick that never sees the frame time: 1000 units idle, 2000
    // searching, 5000 attacking (the pattern's bInSearch/bInAttack), 100000 once on target (m_bLockedOnCurrentFocus
    // +170Ch bit 0), then sets the arm and camera bones. At 160 fps the attacking head swings 90 degrees in four
    // frames (0.025 s). Here the step (a float at [esp+7Ch] where it is loaded) is scaled by dt x TurnRate
    // (the original at 30 fps), x UAVTurnSpeed, and x DrawTurnSpeed while the drone is drawing on Sam.
    float g_uavTurn = 0.6f;   // idle 1000 x 30 x 0.6 = 99 deg/s: still keeps up with the 90 deg/s scanning sweep

    float UAVLookScale(void* pawn)
    {
        float scale = std::clamp(g_frameDt * g_turnHz, 0.0f, 2.0f) * g_uavTurn;
        void* ctrl = pawn ? At<void*>(pawn, kController) : nullptr;
        if (ctrl && g_drawTurn < 1.0f)
        {
            const auto it = g_guards.find(ctrl);
            if (it != g_guards.end() && Drawing(it->second, pawn, LevelTime(pawn))) scale *= g_drawTurn;
        }
        return scale;
    }

    void __cdecl OnUAVLook(Mem::Regs* r)
    {
        __try
        {
            float& step = r->Stack<float>(0x7C);
            step = std::max(step * UAVLookScale(reinterpret_cast<void*>(r->esi)), 1.0f);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // [DRAW] how a guard's gun comes up: bGunUp (EPawn +744h bit 30, set from the controller's bFocusWithGun
    // +A28h bit 0 by the shoot goal; picks the GU animation sets in 0x10B7C450), his wait animation (+F34h) and the
    // animation channels (mesh instance +64h, channels +94h / count +98h, 80h each: sequence +0, rate +8, frame +0Ch,
    // alpha +30h, blend time +3Ch, target alpha +40h).
    void LogAnims(void* ctrl, void* pawn, const char* when)
    {
        char line[1200];
        int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "[DRAW] %s: gunUp %d focusWithGun %d", when,
                              (At<uint32_t>(pawn, kFlags744) & kGunUp) ? 1 : 0, (At<uint32_t>(ctrl, kFocusWithGun) & 1) ? 1 : 0);
        wchar_t name[48];
        if (len > 0 && Script::NameText(At<int>(pawn, kWaitAnim), name, 48))
            len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " wait %ls", name);
        // His weapon: in his hands (HandItem) or put away, hidden (bHidden +30h bit 80h), the bone it's on (+140h).
        void* hand = At<void*>(pawn, kHandItem);
        void* weapon = At<void*>(pawn, kCurrentWeapon);
        if (len > 0)
        {
            wchar_t bone[48] = L"-";
            if (weapon && !Script::NameText(At<int>(weapon, 0x140), bone, 48)) wcscpy_s(bone, L"?");
            len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " | weapon %s%s on %ls, handed %d, holster %d |",
                               !weapon ? "none" : hand == weapon ? "in hand" : hand ? "other in hand" : "away",
                               weapon && (At<uint32_t>(weapon, kActorFlags30) & 0x80) ? " hidden" : "", bone,
                               At<int>(pawn, kHandedness), (At<uint32_t>(pawn, kFlags744) & 1) ? 1 : 0);
        }
        void* inst = At<void*>(pawn, kMeshInstance);
        const int num = inst ? std::min(At<int>(inst, 0x98), 20) : 0;
        const uint8_t* data = inst ? At<uint8_t*>(inst, 0x94) : nullptr;
        for (int ch = 0; data && ch < num && len > 0 && len < int(sizeof(line)) - 100; ++ch)
        {
            const uint8_t* c = data + ch * 0x80;
            const int seq = *reinterpret_cast<const int*>(c);
            if (!seq) continue;
            if (!Script::NameText(seq, name, 48)) swprintf_s(name, L"#%d", seq);
            len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " ch%d %ls a%.2f->%.2f/%.2fs f%.2f r%.2f", ch, name,
                               *reinterpret_cast<const float*>(c + 0x30), *reinterpret_cast<const float*>(c + 0x40),
                               *reinterpret_cast<const float*>(c + 0x3C), *reinterpret_cast<const float*>(c + 0x0C),
                               *reinterpret_cast<const float*>(c + 0x08));
        }
        LOG("%s", line);
    }

    // Channel 'ch' of the pawn's animation (nullptr if there's no such channel).
    uint8_t* Channel(void* pawn, int ch)
    {
        void* inst = At<void*>(pawn, kMeshInstance);
        if (!inst || At<int>(inst, 0x98) <= ch) return nullptr;
        uint8_t* data = At<uint8_t*>(inst, 0x94);
        return data ? data + ch * 0x80 : nullptr;
    }

    // His main animation is tweening into a gun-up one (a "...GU..." sequence at a frame below 0): his gun is only
    // now coming up.
    bool JustRaised(void* pawn)
    {
        const uint8_t* c = Channel(pawn, 0);
        if (!c || *reinterpret_cast<const float*>(c + 0x0C) >= 0.0f) return false;
        wchar_t name[48];
        return Script::NameText(*reinterpret_cast<const int*>(c), name, 48) && wcsstr(name, L"GU") != nullptr;
    }

    // ---- first shot ----

    // His line of sight to the enemy right now: bLineTrace (bit 1 of the EnemyInformation flags +40h).
    bool LineTo(void* ctrl, void* enemy)
    {
        uint8_t* info = At<uint8_t*>(ctrl, kEnemyInfo);
        const int n = At<int>(ctrl, kEnemyCount);
        if (!info || n <= 0 || n > 64) return false;
        for (int i = 0; i < n; ++i)
            if (At<void*>(info + i * 0x64, 0) == enemy) return (At<uint32_t>(info + i * 0x64, 0x40) & 2) != 0;
        return false;
    }

    void HoldFire(void* ctrl, void* target)
    {
        void* pawn = At<void*>(ctrl, kEPawn);
        if (!pawn) return;
        const float now = LevelTime(ctrl);
        Guard& g = Get(ctrl, now);
        const bool fresh = now - g.lastShot > g_drawReady;
        // Combat reaction: Sam back in his sights after ReacquireAfter seconds out of them (a peek, coming round a
        // corner) costs his reaction time before he can fire again. Also his aim settles from here (ShouldHit).
        const bool vis = LineTo(ctrl, target);
        if (vis)
        {
            if (fresh || g.acquired < -999.0f) g.acquired = now;
            else if (now - g.lastVis > g_reacquire)   // he had no line to Sam for that long (or wasn't shooting)
            {
                g.acquired = now;
                if (g_combatReact > 0)
                {
                    float extra = Guards::ReactionExtra(pawn);
                    if (extra < 0) extra = kDefaultExtra;
                    const float react = (kReactMin + extra) * (0.9f + 0.2f * float(rand()) / RAND_MAX) * g_combatReact * CfMul(0.6f);
                    if (now + react > g.holdUntil || !g.holding)
                    {
                        g.holdUntil = std::max(g.holding ? g.holdUntil : now, now + react);
                        g.holdFrom = now;
                        g.holding = true;
                    }
                    if (g_log)
                    {
                        char who[160]; Who(pawn, who, sizeof(who));
                        LOG("[COMBAT] %s has Sam back in his sights after %.2f s: holds fire %.2f s (his reaction)", who,
                            now - std::max(g.lastVis, -100.0f), react);
                    }
                }
            }
            g.lastVis = now;
        }
        g.vis = vis;
        // Hit by Sam (flinch) or pinned by his shots landing around him: no firing until that's over.
        const float until = std::max(g.flinchUntil, g.pinUntil);
        if (now < until && (until > g.holdUntil || !g.holding))
        {
            g.holdUntil = std::max(g.holding ? g.holdUntil : now, until);
            if (!g.holding) g.holdFrom = now;
            g.holding = true;
        }
        if (fresh)
        {
            // A warning burst first: Sam at range, and he hasn't fired on anyone lately.
            g.warnPending = false;
            if (g_warnChance > 0 && !Clusterfuck::Active() && now - g_samHostile > 10.0f && float(rand()) / RAND_MAX < g_warnChance)
            {
                const Vec& a = At<Vec>(pawn, kLocation);
                const Vec& b = At<Vec>(target, kLocation);
                g.warnPending = Dist(a, b) >= g_warnRange;
            }
            // A new engagement. Counted from when he spotted Sam if he's been tracking him (his gun may have been
            // coming up meanwhile), but with at least DrawAimTime of aiming; from now if we never saw him spot Sam.
            const float draw = g_draw * CfMul(0.5f) * Scale(pawn);
            const bool tracked = g.engaged > -999.0f && now - g.lastTrack <= g_drawReady;
            g.holdUntil = tracked ? std::max(g.engaged + draw, now + g_drawAim) : now + draw;
            g.holdFrom = now;
            g.holding = g.holdUntil > now;
            // His gun was down (when he spotted Sam, or it's tweening into a gun-up animation right now): it comes
            // up over the raise time (driven from Alert::OnPlayerTick), and the first shot waits for it. Put away
            // (no HandItem when he spotted Sam, standing): he draws it first.
            if (g_raise > 0 && g.raise == 0 && ((tracked && !g.gunUpAtSpot) || JustRaised(pawn)))
            {
                const float s = std::clamp(Scale(pawn), 0.8f, 1.4f);
                const bool away = tracked && !g.handAtSpot && g_drawAnim > 0 && !(At<uint32_t>(pawn, kPawnFlags2B8) & 2);
                g.raise = 1;
                g.raiseStart = now;
                g.drawDur = away ? g_drawAnim * s : 0.0f;
                g.raiseHold = away ? 0.0f : g_raiseDelay * s;
                g.raiseDur = g_raise * s;
                g.holdUntil = std::max(g.holdUntil, now + g.drawDur + g.raiseHold + g.raiseDur + 0.1f);
                g.holding = true;
                g.aimFrom = now + g.drawDur + g.raiseHold + g.raiseDur;   // his aim settles once the gun is up
            }
            if (g_log)
            {
                char who[160]; Who(pawn, who, sizeof(who));
                char raise[80] = "";
                if (g.raise == 1 && g.drawDur > 0)
                    _snprintf_s(raise, sizeof(raise), _TRUNCATE, " (drawing it over %.2f s, then raising it over %.2f s)", g.drawDur, g.raiseDur);
                else if (g.raise == 1)
                    _snprintf_s(raise, sizeof(raise), _TRUNCATE, " (raising his gun over %.2f s)", g.raiseHold + g.raiseDur);
                if (tracked)
                    LOG("[DRAW] %s has his gun on Sam %.2f s after spotting him: first shot in %.2f s%s", who, now - g.engaged, g.holdUntil - now, raise);
                else
                    LOG("[DRAW] %s has his gun on Sam: first shot in %.2f s%s", who, g.holdUntil - now, raise);
                LogAnims(ctrl, pawn, "  gun on Sam");
                g.sampleFrom = now;
                g.sampleNext = now + 0.1f;
                g.samples = 0;
            }
        }
        g.lastShot = now;
        if (g.holding)
        {
            if (now < g.holdUntil)
            {
                float& next = At<float>(ctrl, kNextFireTime);
                if (next < g.holdUntil) next = g.holdUntil;
            }
            else
            {
                g.holding = false;
                if (g_log)
                {
                    char who[160]; Who(pawn, who, sizeof(who));
                    LOG("[DRAW] %s opens fire (held %.2f s)", who, now - g.holdFrom);
                }
            }
        }
    }

    // ---- fire rate ----
    //
    // A guard firing at a spot he can't see into (Sam behind a closed door) has his burst set up again on every
    // call (0x10BAE168: NextFireTime = now, a 10 s burst), and every set-up fires at once: one shot per AI tick,
    // 30 a second at the consoles' frame rate and ~150 at 160 fps (thousands of shots into a door). Here an NPC's
    // shots are at least NPCShotInterval apart (his normal automatic fire is ~0.07 s): when the last one was too
    // recent, NextFireTime is pushed to then, so the native's own between-shots wait holds him. A shot is a call
    // that reached the hit check (hkShouldHit sets g_shotCtrl).
    void* g_shotCtrl = nullptr;

    void CapFireRate(void* ctrl)
    {
        const auto it = g_guards.find(ctrl);
        if (it == g_guards.end() || it->second.lastFired < -999.0f) return;
        Guard& g = it->second;
        const float now = LevelTime(ctrl);
        if (now < g.lastFired || now - g.lastFired >= g_minShot) return;
        float& next = At<float>(ctrl, kNextFireTime);
        const float want = g.lastFired + g_minShot;
        if (next >= want) return;
        next = want;
        ++g.capped;
        if (g_log && now - g.cappedLog > 10.0f)
        {
            g.cappedLog = now;
            char who[160]; Who(At<void*>(ctrl, kEPawn), who, sizeof(who));
            LOG("[COMBAT] %s tried to fire again %.3f s after his last shot: held to one shot every %.3f s", who,
                now - g.lastFired, g_minShot);
        }
    }

    void NoteShot(void* ctrl)
    {
        const float now = LevelTime(ctrl);
        Get(ctrl, now).lastFired = now;
    }

    int __fastcall hkShoot(void* ctrl, void* edx, void* target, int a2, int a3, int a4, int a5, int a6)
    {
        if (g_minShot > 0 && ctrl)
        {
            __try { CapFireRate(ctrl); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if ((g_draw > 0 || g_combatReact > 0) && ctrl && IsSam(target))
        {
            __try { HoldFire(ctrl, target); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        g_shotCtrl = nullptr;
        const int r = oShoot(ctrl, edx, target, a2, a3, a4, a5, a6);
        if (g_minShot > 0 && ctrl && g_shotCtrl == ctrl)
        {
            __try { NoteShot(ctrl); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return r;
    }

    // ---- hit chance ----
    //
    // Whether a guard's shot should hit is decided before it is traced, by EAIController 0x10BA3130 (thiscall
    // (target, a2), ret 8): bFirstHit (+99Ch bit 0) makes it a hit outright (and is cleared); else the chance is
    // GunAccuracy (pawn +9C8h) x 2.5 within 1 m / x2 within 2 m / x1.5 within 5 m (+0.1 for some patterns), clamped
    // to [MinGunAccuracy +9CCh, MaxGunAccuracy +9D0h] (a dark target beyond 5 m: 0.05-0.15), and the shot hits when
    // chance x 10 beats his hits in the last 10 shots (+98Ch): a steady quota, no aim at all. The shot (0x10BAD630)
    // then traces at a random bone, pushed 40 units out for a miss.
    // That compare is a rubber band: 10 x rand^(log2(10 / hits)) (0x10CEE590 (hits, 10)), so with no hits in his
    // last 10 any chance above 0 hits, with 1 a chance of 0.03 still hits a third of the time, and only at 5 is
    // it a plain roll. A guard who hadn't hit Sam lately (every fresh one) was sure to hit him.
    // Here the guaranteed first hit goes (FirstShotHit 0), the roll is a plain one (FairHitRoll 1: his count is
    // 5 for the compare, his real one kept), and the chance (at [esp+1Ch] where it is loaded for the
    // compare, controller in esi, target in edi) is scaled by: his aim settling since he got Sam in his sights
    // (0.2 rising to 1 over AimSettleTime x his reaction), how fast Sam moves across his view (1 / (1 + w /
    // AimTrackSpeed), w in rad/s) and AimWhileMoving when he is moving himself.
    using ShouldHit_t = int(__fastcall*)(void* ctrl, void* edx, void* target, int a2);
    ShouldHit_t oShouldHit = nullptr;

    // A warning burst: the guard's first shots at Sam after spotting him at range miss on purpose (0.35 s of
    // them), then he waits WarningPause before firing for real.
    bool WarningMiss(void* ctrl)
    {
        const auto it = g_guards.find(ctrl);
        if (it == g_guards.end()) return false;
        Guard& g = it->second;
        const float now = LevelTime(ctrl);
        if (g.warnPending)
        {
            g.warnPending = false;
            g.warnEnd = now + 0.35f;
            g.pinUntil = std::max(g.pinUntil, g.warnEnd + g_warnPause);
            if (g_log)
            {
                char who[160]; Who(At<void*>(ctrl, kEPawn), who, sizeof(who));
                LOG("[COMBAT] %s fires a warning burst, then waits %.2f s", who, g_warnPause);
            }
        }
        return now < g.warnEnd;
    }

    int __fastcall hkShouldHit(void* ctrl, void* edx, void* target, int a2)
    {
        g_shotCtrl = ctrl;   // a shot (the fire-rate cap counts from here)
        bool warn = false, fair = false;
        int hits = 0;
        if (ctrl && IsSam(target))
        {
            __try
            {
                if (!g_firstShotHit) At<uint32_t>(ctrl, 0x99C) &= ~1u;
                warn = g_warnChance > 0 && WarningMiss(ctrl);
                // Fair roll: with 5 hits in his last 10 the compare is chance x 10 > 10 x rand, a plain roll.
                if (g_fairRoll && !warn)
                {
                    hits = At<int>(ctrl, kHitsInLast10);
                    At<int>(ctrl, kHitsInLast10) = 5;
                    fair = true;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (warn) return 0;
        g_lastChance = -1.0f;
        const int r = oShouldHit(ctrl, edx, target, a2);
        if (fair)
        {
            __try { At<int>(ctrl, kHitsInLast10) = hits; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (g_log && ctrl && IsSam(target))
        {
            // At most ~6 lines a second; the shots in between are counted into the next line.
            static float lastLine = -100.0f;
            static int quiet = 0, quietHits = 0;
            __try
            {
                const float now = LevelTime(ctrl);
                if (now - lastLine < 0.15f && now >= lastLine)
                {
                    ++quiet;
                    if (r) ++quietHits;
                }
                else
                {
                    lastLine = now;
                    char who[160]; Who(At<void*>(ctrl, kEPawn), who, sizeof(who));
                    char more[64] = "";
                    if (quiet) _snprintf_s(more, _TRUNCATE, " [+%d more shots, %d hits]", quiet, quietHits);
                    quiet = quietHits = 0;
                    if (g_lastChance >= 0)
                        LOG("[COMBAT] %s shoots at Sam: %s (chance %.2f, %s)%s", who, r ? "HIT" : "miss", g_lastChance,
                            fair ? "plain roll" : "game's own roll", more);
                    else
                        LOG("[COMBAT] %s shoots at Sam: %s (no roll)%s", who, r ? "HIT" : "miss", more);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return r;
    }

    float Suppression(const Guard& g, float now)
    {
        if (g.supp <= 0 || g_supHalf <= 0) return 0.0f;
        return g.supp * std::pow(0.5f, std::max(now - g.suppT, 0.0f) / g_supHalf);
    }

    struct Aim { float settle = 1, track = 1, moving = 1, supp = 1, dark = 1, wound = 1; };

    float AimFactor(void* ctrl, void* target, Aim& f)
    {
        float& settle = f.settle; float& track = f.track; float& moving = f.moving;
        void* pawn = At<void*>(ctrl, kEPawn);
        if (!pawn) return 1.0f;
        const float now = LevelTime(ctrl);
        const auto it = g_guards.find(ctrl);
        if (g_aimSettle > 0 && it != g_guards.end() && it->second.acquired > -999.0f)
        {
            const float s = std::clamp(Scale(pawn), 0.6f, 1.6f);
            const float from = std::max(it->second.acquired, it->second.aimFrom);
            settle = std::clamp(0.2f + 0.8f * (now - from) / (g_aimSettle * CfMul(0.5f) * s), 0.2f, 1.0f);
        }
        const Vec& p = At<Vec>(pawn, kLocation);
        const Vec& t = At<Vec>(target, kLocation);
        const Vec& v = At<Vec>(target, 0x100);
        const float dx = t.x - p.x, dy = t.y - p.y, dz = t.z - p.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (g_aimTrack > 0 && d > 1.0f)
        {
            const float along = (v.x * dx + v.y * dy + v.z * dz) / d;
            const float v2 = v.x * v.x + v.y * v.y + v.z * v.z;
            const float perp = std::sqrt(std::max(v2 - along * along, 0.0f));
            track = 1.0f / (1.0f + (perp / d) / g_aimTrack);
        }
        const Vec& gv = At<Vec>(pawn, 0x100);
        if (std::sqrt(gv.x * gv.x + gv.y * gv.y) > 120.0f) moving = g_aimMoving;
        // Suppressed: shots landing around him throw his aim off.
        if (it != g_guards.end() && g_supAim > 0) f.supp = 1.0f - g_supAim * Suppression(it->second, now);
        // Darkness: Sam's light (LuminosityFactor +264h, the scale the game's own "dark below 10" uses) from 30 down
        // to 5, counting from 3 m to fully at 10 m; night vision (+42Ch bit 2) or thermal (+748h bit 27) ignore it.
        const bool goggles = (At<uint32_t>(pawn, 0x42C) & 4) || (At<uint32_t>(pawn, 0x748) & 0x8000000);
        if (g_darkAim > 0 && !goggles)
        {
            const float lum = At<float>(target, 0x264);
            const float dark = std::clamp((30.0f - lum) / 25.0f, 0.0f, 1.0f);
            const float reach = std::clamp((d - 300.0f) / 700.0f, 0.0f, 1.0f);
            f.dark = 1.0f - g_darkAim * dark * reach;
        }
        // Wounded: health against his starting health (+1530h; the most seen if that's unset).
        if (g_woundAim > 0)
        {
            const int hp = At<int>(pawn, kHealth);
            int full = At<int>(pawn, 0x1530);
            if (it != g_guards.end())
            {
                Guard& g = const_cast<Guard&>(it->second);
                g.maxHealth = std::max(g.maxHealth, hp);
                if (full <= 0) full = g.maxHealth;
            }
            if (full > 0) f.wound = 1.0f - g_woundAim * (1.0f - std::clamp(float(hp) / full, 0.0f, 1.0f));
        }
        return std::max(settle * track * moving * f.supp * f.dark * f.wound, 0.03f);
    }

    void __cdecl OnHitChance(Mem::Regs* r)
    {
        __try
        {
            void* ctrl = reinterpret_cast<void*>(r->esi);
            void* target = reinterpret_cast<void*>(r->edi);
            if (!ctrl || !IsSam(target)) return;
            Aim a;
            const float f = AimFactor(ctrl, target, a);
            float& chance = r->Stack<float>(0x1C);
            const float before = chance;
            chance *= f;
            g_lastChance = chance;
            if (g_log && f < 0.999f)
            {
                static float lastLog = -100.0f;
                const float now = LevelTime(ctrl);
                if (now - lastLog > 0.5f || now < lastLog)
                {
                    lastLog = now;
                    char who[160]; Who(At<void*>(ctrl, kEPawn), who, sizeof(who));
                    LOG("[COMBAT] %s hit chance %.2f -> %.2f (aim settling x%.2f, Sam moving x%.2f, himself moving x%.2f, "
                        "suppressed x%.2f, dark x%.2f, wounded x%.2f)", who, before, chance, a.settle, a.track, a.moving, a.supp, a.dark, a.wound);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ---- call-out ----

    // True to pass the broadcast on now.
    bool OnBroadcast(void* enemy, void* ctrl, int shareOnly, int a4, int a5)
    {
        void* pawn = At<void*>(ctrl, kEPawn);
        const float now = LevelTime(ctrl);
        Guard& g = Get(ctrl, now);
        bool fresh = false;   // he finds Sam after not having had him for CallOutAgain s: news, whatever came before
        if (!shareOnly)   // his own look at Sam (shareOnly: the patterns' "where did he go" deductions, 0x1093BE70)
        {
            fresh = now - g.lastTrack > g_callAgain;
            if (now - g.lastTrack > g_drawReady)
            {
                g.engaged = now;
                g.gunUpAtSpot = pawn && (At<uint32_t>(pawn, kFlags744) & kGunUp) != 0;
                g.handAtSpot = pawn && At<void*>(pawn, kHandItem) != nullptr;
            }
            g.lastTrack = now;
        }
        if (g_callOut <= 0) return true;

        if (g.pending)
        {
            if (!shareOnly && a5 == 0) g.a5 = 0;   // the release sends backup requests if any held update would
            return false;
        }
        if (shareOnly)   // deductions go out as in the original unless he's holding his news
        {
            if (a4 && !a5 && g_log)   // 0x10BE0150: he joins a fighting group, backup requests included
            {
                static float lastLog = -100.0f;
                if (now - lastLog > 2.0f || now < lastLog)
                {
                    lastLog = now;
                    char who[160]; Who(pawn, who, sizeof(who));
                    LOG("[ALERT] %s joins a group that is fighting and passes on what he knows of Sam", who);
                }
            }
            return true;
        }
        const bool fighting = Fighting(ctrl);
        if (!fresh)   // he's had Sam all along
        {
            if (g.released) { g.lastShare = now; return true; }   // he called it in: his updates go out at once
            if (fighting)   // in a fight with Sam in front of him: he's on the radio
            {
                g.released = true;
                g.lastShare = now;
                if (g_log)
                {
                    char who[160]; Who(pawn, who, sizeof(who));
                    LOG("[ALERT] %s is fighting with Sam in sight: the guards around him hear it at once", who);
                }
                return true;
            }
        }
        g.released = false;   // a new sighting (or his first): he has to call it in

        // He wasn't fighting: whatever an earlier fight left (a hold he never fired out of before he was knocked
        // out, say) is over, and he has to draw again. (In a fight his gun is out already.)
        if (g.raise == 0 && !fighting)
        {
            g.engaged = now;
            g.lastShot = -1000;
            g.holding = false;
            g.gunUpAtSpot = pawn && (At<uint32_t>(pawn, kFlags744) & kGunUp) != 0;
            g.handAtSpot = pawn && At<void*>(pawn, kHandItem) != nullptr;
        }
        g.pending = true;
        g.spotted = now;
        g.due = now + g_callOut * CfMul(0.25f) * Scale(pawn);
        g.enemy = enemy;
        g.a5 = a5 ? 1 : 0;
        if (g_log)
        {
            char who[160]; Who(pawn, who, sizeof(who));
            LOG("[ALERT] %s %s: calls it in in %.2f s unless he's stopped", who,
                fighting ? "finds Sam again mid-fight" : "spotted Sam", g.due - now);
            if (pawn) LogAnims(ctrl, pawn, "  spotting him");
        }
        return false;
    }

    void __fastcall hkBroadcast(void* mgr, void* edx, void* enemy, void* ctrl, int shareOnly, int a4, int a5)
    {
        bool pass = true;
        if (ctrl && IsSam(enemy))
        {
            __try { pass = OnBroadcast(enemy, ctrl, shareOnly, a4, a5); }
            __except (EXCEPTION_EXECUTE_HANDLER) { pass = true; }
        }
        if (pass) oBroadcast(mgr, edx, enemy, ctrl, shareOnly, a4, a5);
    }

    // The controller still runs a pawn in this level (it may have been destroyed since he spotted Sam).
    void* LivePawn(void* sam, void* ctrl)
    {
        void* xlevel = At<void*>(sam, kXLevel);
        if (!xlevel) return nullptr;
        void** actors = At<void**>(xlevel, 0x28);
        const int n = At<int>(xlevel, 0x2C);
        if (!actors || n <= 0 || n > 500000) return nullptr;
        for (int i = 0; i < n; ++i)
        {
            void* a = actors[i];
            if (!a || a == sam || (At<uint32_t>(a, 0x28) & 4)) continue;   // bDeleteMe
            if (!(At<uint32_t>(a, 0x2C) & 0x10)) continue;                 // pawns
            if (At<void*>(a, kController) == ctrl) return At<void*>(ctrl, kEPawn) == a ? a : nullptr;
        }
        return nullptr;
    }

    const wchar_t* OutOfAction(void* pawn, void* ctrl)
    {
        static const wchar_t* const kOut[] = { L"Unconscious", L"Dead", L"Dying", L"Grabbed", L"Inert", L"Carried",
                                               L"Stunned", L"Electrocuted" };
        if (At<int>(pawn, kHealth) <= 0) return L"dead";
        static wchar_t st[128];
        for (void* obj : { ctrl, pawn })
        {
            if (!Script::NameText(Script::StateName(obj), st, 128)) continue;
            for (const wchar_t* w : kOut)
                if (wcsstr(st, w)) return st;
        }
        return nullptr;
    }

    bool KnowsOf(void* ctrl, void* enemy)
    {
        uint8_t* info = At<uint8_t*>(ctrl, kEnemyInfo);
        const int n = At<int>(ctrl, kEnemyCount);
        if (!info || n <= 0 || n > 64) return false;
        for (int i = 0; i < n; ++i)
            if (At<void*>(info + i * 0x64, 0) == enemy) return true;
        return false;
    }

    // NPCs who'd hear it: the same AI zone and group, alive, not him.
    int Teammates(void* pawn)
    {
        void* zone = At<void*>(pawn, kZone);
        if (!zone) return 0;
        void** pawns = At<void**>(zone, kZonePawns);
        const int n = At<int>(zone, kZoneCount);
        if (!pawns || n <= 0 || n > 4096) return 0;
        int count = 0;
        for (int i = 0; i < n; ++i)
        {
            void* p = pawns[i];
            if (!p || p == pawn || !At<void*>(p, kController) || At<int>(p, kHealth) <= 0) continue;
            if (At<uint8_t>(p, kGroupID) == At<uint8_t>(pawn, kGroupID)) ++count;
        }
        return count;
    }

    // CLUSTERFUCK: everyone is one party. The broadcast reaches the guards of the caller's AI zone and group
    // (pawn +428h / +430h), so it is sent once per (zone, group) pair found in the level, with the caller's pawn
    // briefly given that zone and group. Returns how many guards it reached.
    int BroadcastLevelWide(void* sam, void* ctrl, void* pawn, int a5)
    {
        void* xlevel = At<void*>(sam, kXLevel);
        void** actors = xlevel ? At<void**>(xlevel, 0x28) : nullptr;
        const int n = xlevel ? At<int>(xlevel, 0x2C) : 0;
        std::vector<std::pair<void*, uint8_t>> pairs;
        int reached = 0;
        for (int i = 0; actors && i < n && i < 500000; ++i)
        {
            void* a = actors[i];
            if (!a || a == pawn || a == sam || (At<uint32_t>(a, 0x28) & 4) || !(At<uint32_t>(a, 0x2C) & 0x10)) continue;
            if (!At<void*>(a, kController) || At<int>(a, kHealth) <= 0) continue;
            ++reached;
            const std::pair<void*, uint8_t> key(At<void*>(a, kZone), At<uint8_t>(a, kGroupID));
            if (key.first && std::find(pairs.begin(), pairs.end(), key) == pairs.end()) pairs.push_back(key);
        }
        void* const zone = At<void*>(pawn, kZone);
        const uint8_t group = At<uint8_t>(pawn, kGroupID);
        for (const auto& [z, gr] : pairs)
        {
            At<void*>(pawn, kZone) = z;
            At<uint8_t>(pawn, kGroupID) = gr;
            oBroadcast(At<void*>(ctrl, kAIManager), nullptr, sam, ctrl, 0, 0, a5);
        }
        At<void*>(pawn, kZone) = zone;
        At<uint8_t>(pawn, kGroupID) = group;
        return reached;
    }

    // A guard whose call-out is due: still able, he tells the others what he knows of Sam now.
    void Release(void* sam, void* ctrl, Guard& g, float now)
    {
        g.pending = false;
        void* pawn = LivePawn(sam, ctrl);
        if (!pawn || g.enemy != sam)
        {
            if (g_log) LOG("[ALERT] a guard who spotted Sam is gone before he could call it in");
            return;
        }
        char who[160]; Who(pawn, who, sizeof(who));
        if (const wchar_t* why = OutOfAction(pawn, ctrl))
        {
            if (g_log) LOG("[ALERT] %s was stopped before he could call it in (%ls): nobody heard it from him", who, why);
            return;
        }
        g.released = true;
        g.lastShare = now;
        if (!KnowsOf(ctrl, sam))
        {
            if (g_log) LOG("[ALERT] %s has nothing to call in any more (forgot Sam)", who);
            return;
        }
        int mates = Teammates(pawn);
        if (Clusterfuck::Active())
            mates = BroadcastLevelWide(sam, ctrl, pawn, g.a5);
        else
            oBroadcast(At<void*>(ctrl, kAIManager), nullptr, sam, ctrl, 0, 0, g.a5);
        if (g_log) LOG("[ALERT] %s calls it in after %.2f s: %d guard(s) around him hear it", who, now - g.spotted, mates);
    }

    void ReleaseSafe(void* sam, void* ctrl, Guard& g, float now)
    {
        __try { Release(sam, ctrl, g, now); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g.pending = false; }
    }

    // ---- gun raise ----
    //
    // The game has no draw animation: when a guard engages, his anim set switches to the gun-up (GU) one (bGunUp)
    // and his main channel tweens from the gun-down pose into the GU wait in under 0.2 s (walking GU right after
    // when he moves). Here his own gun-down wait (AWaitGD +F38h, crouched AWaitCrouchGD +F50h) goes on channel 13
    // from "B Spine1" up (EPawn.BlendAnimOverCurrent, native 1538) at full strength, frozen at frame 0, held for
    // GunRaiseDelay and faded out over GunRaiseTime (both x his reaction) with our own envelope every tick: his
    // upper body brings the gun up while his legs do whatever the game has them do.

    void SetChannel(void* pawn, float alpha, float frame = 0.0f)
    {
        if (uint8_t* c = Channel(pawn, kRaiseChannel))
        {
            *reinterpret_cast<float*>(c + 0x30) = alpha;   // BlendAlpha
            *reinterpret_cast<float*>(c + 0x40) = alpha;   // its target: the engine leaves it there
            *reinterpret_cast<float*>(c + 0x0C) = frame;   // frame
            *reinterpret_cast<float*>(c + 0x08) = 0.0f;    // rate: frozen, we move it
        }
    }

    // Plays 'anim' on the raise channel from the spine up; false (channel cleared) if it doesn't take.
    bool PlayUpper(void* pawn, int anim, const char* who)
    {
        Script::Args a;   // Sequence, BlendStrength, BoneName, Rate, TweenTime, Channel, bBackward, BlendTime, in_loop, pause
        a.Name(anim); a.Float(1.0f); a.Name(g_boneSpine); a.Float(1.0f); a.Float(0.0f); a.Int(kRaiseChannel);
        a.Skip(); a.Float(0.0f); a.Skip(); a.Skip();
        if (!Script::CallNative(pawn, g_blendCode, a.End())) return false;
        const uint8_t* c = Channel(pawn, kRaiseChannel);
        if (c && *reinterpret_cast<const int*>(c) == anim && c[0x46] != 0) return true;
        if (c) SetChannel(pawn, 0.0f);
        if (g_log)
        {
            wchar_t name[48];
            if (!Script::NameText(anim, name, 48)) wcscpy_s(name, L"?");
            LOG("[DRAW] %s: couldn't put %ls on his upper body (channel %s, bone %d)", who, name,
                c ? (*reinterpret_cast<const int*>(c) == anim ? "ok" : "other") : "missing", c ? int(c[0x46]) : -1);
        }
        return false;
    }

    // The draw for a gun that was put away: his holster animation (AHolsterGun +149Ch, TranStGdNt2: gun-down to
    // put away, the weapon moving with his hand) backwards, ending in its first frame, the gun-down pose.
    bool StartDraw(void* pawn, const char* who)
    {
        const int anim = At<int>(pawn, kHolsterGun);
        if (anim <= 0 || !PlayUpper(pawn, anim, who)) return false;
        SetChannel(pawn, 1.0f, 1.0f);
        if (g_log)
        {
            wchar_t name[48];
            if (!Script::NameText(anim, name, 48)) wcscpy_s(name, L"?");
            LOG("[DRAW] %s draws his gun (%ls backwards)", who, name);
        }
        return true;
    }

    bool RaiseReady()
    {
        if (!g_raiseLookup)
        {
            g_raiseLookup = true;
            g_blendCode = Script::Native(1538);
            g_boneSpine = Script::Name(L"B Spine1");
            if (!g_blendCode || !g_boneSpine) LOG("Alert: gun raise unavailable (BlendAnimOverCurrent or the spine bone not found)");
        }
        return g_blendCode && g_boneSpine && Script::Ready();
    }

    bool StartRaise(void* ctrl, void* pawn, const char* who)
    {
        if (!RaiseReady()) return false;
        const bool crouched = (At<uint32_t>(pawn, kPawnFlags2B8) & 2) != 0;
        const int anim = At<int>(pawn, crouched ? kWaitCrouchGD : kWaitGD);
        if (anim <= 0) { if (g_log) LOG("[DRAW] %s: no gun-down wait animation to raise from", who); return false; }
        if (!PlayUpper(pawn, anim, who)) return false;
        SetChannel(pawn, 1.0f);
        if (g_log)
        {
            wchar_t name[48];
            if (!Script::NameText(anim, name, 48)) wcscpy_s(name, L"?");
            LOG("[DRAW] %s raises his gun from %ls", who, name);
        }
        return true;
    }

    // The actor is still in the level (a body stays after its controller lets go of it).
    bool InLevel(void* sam, void* actor)
    {
        void* xlevel = At<void*>(sam, kXLevel);
        if (!xlevel || !actor) return false;
        void** actors = At<void**>(xlevel, 0x28);
        const int n = At<int>(xlevel, 0x2C);
        if (!actors || n <= 0 || n > 500000) return false;
        for (int i = 0; i < n; ++i)
            if (actors[i] == actor) return (At<uint32_t>(actor, 0x28) & 4) == 0;
        return false;
    }

    void RaiseTick(void* sam, void* ctrl, Guard& g, float now)
    {
        void* pawn = LivePawn(sam, ctrl);
        if (!pawn)   // killed or gone: take the pose off his body if it's still there
        {
            if (g.raise == 2 && InLevel(sam, g.raisePawn)) SetChannel(g.raisePawn, 0.0f);
            g.raise = 0;
            return;
        }
        if (OutOfAction(pawn, ctrl))
        {
            if (g.raise == 2) SetChannel(pawn, 0.0f);
            g.raise = 0;
            return;
        }
        if (g.raise == 1)
        {
            char who[160]; Who(pawn, who, sizeof(who));
            bool ok = false;
            if (g.drawDur > 0)
            {
                ok = RaiseReady() && StartDraw(pawn, who);
                if (!ok) g.drawDur = 0.0f;   // no draw animation: raise from gun-down instead
            }
            if (!ok) ok = StartRaise(ctrl, pawn, who);
            if (!ok) { g.raise = 0; return; }
            g.raise = 2;
            g.raisePawn = pawn;
            const uint8_t* c = Channel(pawn, kRaiseChannel);
            g.raiseAnim = c ? *reinterpret_cast<const int*>(c) : 0;
        }
        const uint8_t* c = Channel(pawn, kRaiseChannel);
        if (!c || *reinterpret_cast<const int*>(c) != g.raiseAnim)   // something else took the channel
        {
            if (g_log) { char who[160]; Who(pawn, who, sizeof(who)); LOG("[DRAW] %s: the raise channel was taken over; raise ended", who); }
            g.raise = 0;
            return;
        }
        const float t = now - g.raiseStart;
        if (t >= g.drawDur + g.raiseHold + g.raiseDur || t < 0.0f)
        {
            SetChannel(pawn, 0.0f);
            g.raise = 0;
            return;
        }
        if (t < g.drawDur)   // the holster animation backwards: from put away to gun-down
        {
            const float x = t / g.drawDur;
            SetChannel(pawn, 1.0f, 1.0f - x * x * (3.0f - 2.0f * x));
            return;
        }
        const float u = t - g.drawDur;
        float alpha = 1.0f;
        if (u > g.raiseHold)
        {
            const float x = std::clamp((u - g.raiseHold) / std::max(g.raiseDur, 0.01f), 0.0f, 1.0f);
            alpha = 1.0f - x * x * (3.0f - 2.0f * x);   // smoothstep
        }
        SetChannel(pawn, alpha, 0.0f);
    }

    void RaiseSafe(void* sam, void* ctrl, Guard& g, float now)
    {
        __try { RaiseTick(sam, ctrl, g, now); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g.raise = 0; }
    }

    // A few animation samples after a guard's gun comes up on Sam (LogPerception).
    void Sample(void* sam, void* ctrl, Guard& g, float now)
    {
        ++g.samples;
        g.sampleNext = now + 0.12f;
        void* pawn = LivePawn(sam, ctrl);
        if (!pawn) { g.samples = 99; return; }
        char when[48];
        _snprintf_s(when, sizeof(when), _TRUNCATE, "  +%.2f s", now - g.sampleFrom);
        LogAnims(ctrl, pawn, when);
    }

    void SampleSafe(void* sam, void* ctrl, Guard& g, float now)
    {
        __try { Sample(sam, ctrl, g, now); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g.samples = 99; }
    }

    // ---- guard movement (Alert::OnNpcTick, right before each NPC pawn's own tick) ----
    //
    // Sprint windup: NPC walking speed is GroundSpeed (+2E8h), set each tick from the walk and jog animation sets'
    // speeds by MovementRatio (+304h, eased by time), and physics accelerates toward it at AccelRate (2048 u/s^2):
    // a guard breaking into a run is at full speed in about a quarter of a second. Here, walking (PHYS_Walking),
    // the speed he may reach rises at most NPCRunAccel u/s^2 above his current speed (patrol pace, up to 180 u/s,
    // stays free): GroundSpeed is capped to that right before his physics runs.
    // Stance blend: a guard crouching or standing up (bIsCrouched, +2B8h bit 1) while standing still switches
    // pose in a single frame (the goal animation plays the other stance's pose outright). Here his pose from just
    // before (main channel's sequence and frame, the snapshot whose name matches the old stance: "St" standing, "Cr"
    // crouched) goes on channel 20 over his whole body and fades out over StanceBlendTime.
    constexpr int kStanceChannel = 20;
    constexpr float kFreeSpeed = 180.0f;
    struct Npc
    {
        void* level = nullptr;
        bool init = false, crouched = false;
        int seq[2] = { 0, 0 }; float frame[2] = { 0, 0 };   // main channel, last two ticks (0 newest)
        bool blending = false; float blendStart = 0; int blendSeq = 0; float blendFrame = 0;
        float gsTarget = 0, gsWritten = -1;                 // the speed the game wants, the cap we last wrote
    };
    std::unordered_map<void*, Npc> g_npcs;

    void SetChannelN(void* pawn, int ch, float alpha, float frame)
    {
        if (uint8_t* c = Channel(pawn, ch))
        {
            *reinterpret_cast<float*>(c + 0x30) = alpha;
            *reinterpret_cast<float*>(c + 0x40) = alpha;
            *reinterpret_cast<float*>(c + 0x0C) = frame;
            *reinterpret_cast<float*>(c + 0x08) = 0.0f;
        }
    }

    bool PlayFull(void* pawn, int anim, int ch)
    {
        if (!RaiseReady()) return false;
        Script::Args a;   // Sequence, BlendStrength, BoneName (None: the whole body), Rate, TweenTime, Channel, ...
        a.Name(anim); a.Float(1.0f); a.Name(0); a.Float(1.0f); a.Float(0.0f); a.Int(ch);
        a.Skip(); a.Float(0.0f); a.Skip(); a.Skip();
        if (!Script::CallNative(pawn, g_blendCode, a.End())) return false;
        const uint8_t* c = Channel(pawn, ch);
        return c && *reinterpret_cast<const int*>(c) == anim;
    }

    bool NameHas(int name, const wchar_t* part)
    {
        wchar_t buf[48];
        return name > 0 && Script::NameText(name, buf, 48) && wcsstr(buf, part) != nullptr;
    }

    void NpcTick(void* pawn)
    {
        void* ctrl = At<void*>(pawn, kController);
        if (!ctrl || At<int>(pawn, kHealth) <= 0) return;
        void* level = At<void*>(pawn, kLevel);
        if (!level) return;
        const float now = At<float>(level, kTime);
        const float dt = At<float>(level, 0x75C);
        const uint8_t physics = At<uint8_t>(pawn, 0x4C);
        const Vec& v = At<Vec>(pawn, 0x100);
        const float speed = std::sqrt(v.x * v.x + v.y * v.y);

        static void* lastLevel = nullptr;
        if (level != lastLevel) { g_npcs.clear(); lastLevel = level; }
        if (g_npcs.size() > 512) g_npcs.clear();
        Npc& n = g_npcs[pawn];
        if (n.level != level) { n = Npc{}; n.level = level; }

        if (g_runAccel > 0)
        {
            float& gs = At<float>(pawn, 0x2E8);
            if (gs != n.gsWritten) n.gsTarget = gs;   // the game set a new speed (not our cap)
            if (physics == 1 && dt > 0)
            {
                const float cap = std::max(kFreeSpeed, speed + g_runAccel * dt);
                gs = std::min(n.gsTarget, cap);
                n.gsWritten = gs;
            }
            else if (n.gsWritten >= 0 && gs == n.gsWritten)
            {
                gs = n.gsTarget;                       // not walking: give back what the game wanted
                n.gsWritten = -1;
            }
        }

        if (g_stanceBlend <= 0) return;
        const bool crouched = (At<uint32_t>(pawn, kPawnFlags2B8) & 2) != 0;
        if (n.init && crouched != n.crouched && physics == 1 && speed < 80.0f && !n.blending)
        {
            // the old stance's pose: the newest snapshot that still shows it
            const wchar_t* old = n.crouched ? L"Cr" : L"St";
            int pick = NameHas(n.seq[0], old) ? 0 : NameHas(n.seq[1], old) ? 1 : -1;
            if (pick >= 0 && PlayFull(pawn, n.seq[pick], kStanceChannel))
            {
                n.blending = true; n.blendStart = now; n.blendSeq = n.seq[pick]; n.blendFrame = n.frame[pick];
                SetChannelN(pawn, kStanceChannel, 1.0f, n.blendFrame);
                if (g_log)
                {
                    char who[160]; Who(pawn, who, sizeof(who));
                    wchar_t name[48]; if (!Script::NameText(n.blendSeq, name, 48)) wcscpy_s(name, L"?");
                    LOG("[STANCE] %s %s: blending out of %ls over %.2f s", who, crouched ? "crouches" : "stands up", name, g_stanceBlend);
                }
            }
        }
        n.crouched = crouched;
        n.init = true;
        if (n.blending)
        {
            const uint8_t* c = Channel(pawn, kStanceChannel);
            const float t = now - n.blendStart;
            if (!c || *reinterpret_cast<const int*>(c) != n.blendSeq || t < 0.0f || t >= g_stanceBlend)
            {
                if (c && *reinterpret_cast<const int*>(c) == n.blendSeq) SetChannelN(pawn, kStanceChannel, 0.0f, n.blendFrame);
                n.blending = false;
            }
            else
            {
                const float x = t / g_stanceBlend;
                SetChannelN(pawn, kStanceChannel, 1.0f - x * x * (3.0f - 2.0f * x), n.blendFrame);
            }
        }
        if (const uint8_t* c0 = Channel(pawn, 0))
        {
            n.seq[1] = n.seq[0]; n.frame[1] = n.frame[0];
            n.seq[0] = *reinterpret_cast<const int*>(c0);
            n.frame[0] = std::clamp(*reinterpret_cast<const float*>(c0 + 0x0C), 0.0f, 1.0f);
        }
    }

    float IniFloat(const std::wstring& ini, const wchar_t* key, float def)
    {
        wchar_t buf[64];
        GetPrivateProfileStringW(L"Perception", key, L"", buf, 64, ini.c_str());
        for (wchar_t* p = buf; *p; ++p) if (*p == L';') { *p = 0; break; }
        return buf[0] ? static_cast<float>(_wtof(buf)) : def;
    }
}

namespace
{
    // An AI event a guard receives (the dispatcher hook in perception.cpp; type byte +0, about-actor +38h):
    // AI_TAKE_DAMAGE (126) from Sam makes him flinch: no firing for FlinchTime (x0.8-1.2) and his aim settles
    // again; AI_SHOT_JUST_MISSED (120) and AI_HEAR_RICOCHET (121) from Sam suppress him: +0.35 suppression (a hit
    // +0.5, at most 1, halving every SuppressionDecay s) and no firing for SuppressionPin s.
    void Event(void* ctrl, const uint8_t* ev)
    {
        const uint8_t type = ev[0];
        if (type != 126 && type != 120 && type != 121) return;
        void* about = *reinterpret_cast<void* const*>(ev + 0x38);
        if (!IsSam(about)) return;
        void* pawn = At<void*>(ctrl, kEPawn);
        if (!pawn || At<int>(pawn, kHealth) <= 0) return;
        const float now = LevelTime(ctrl);
        g_samHostile = now;
        Guard& g = Get(ctrl, now);
        const float s = Suppression(g, now);
        if (type == 126)
        {
            if (g_flinch > 0)
            {
                g.flinchUntil = std::max(g.flinchUntil, now + g_flinch * (0.8f + 0.4f * float(rand()) / RAND_MAX));
                g.acquired = now;
            }
            if (g_supAim > 0) { g.supp = std::min(1.0f, s + 0.5f); g.suppT = now; }
            // Reeling from the hit, he can't get his call-out out until he has recovered.
            const float pushed = g.pending ? std::max(g.due, g_flinch > 0 ? g.flinchUntil : now + 0.6f) : 0.0f;
            const float delay = g.pending ? pushed - g.due : 0.0f;
            if (g.pending) g.due = pushed;
            if (g_log)
            {
                char who[160]; Who(pawn, who, sizeof(who));
                LOG("[COMBAT] %s is hit by Sam: flinches, no firing for %.2f s%s", who, std::max(g.flinchUntil - now, 0.0f),
                    delay > 0.005f ? ", call-out held back" : "");
            }
            return;
        }
        if (g_supAim > 0 || g_supPin > 0 || g_duck > 0)
        {
            g.supp = std::min(1.0f, s + 0.35f);
            g.suppT = now;
            if (g_supPin > 0) g.pinUntil = std::max(g.pinUntil, now + g_supPin);
            // Ducking: heavy fire around him (suppression at DuckLevel or more) puts him down for SuppressionDuck s.
            if (g_duck > 0 && g.supp >= g_duckLevel)
            {
                const bool fresh = now >= g.duckUntil;
                g.duckUntil = std::max(g.duckUntil, now + g_duck * (0.8f + 0.4f * float(rand()) / RAND_MAX));
                if (fresh && g_log)
                {
                    char who[160]; Who(pawn, who, sizeof(who));
                    LOG("[COMBAT] %s ducks under Sam's fire (suppression %.2f): stays down %.2f s", who, g.supp, g.duckUntil - now);
                }
            }
            if (g_log)
            {
                static float lastLog = -100.0f;
                if (now - lastLog > 0.5f || now < lastLog)
                {
                    lastLog = now;
                    char who[160]; Who(pawn, who, sizeof(who));
                    LOG("[COMBAT] %s is suppressed by Sam's %s: suppression %.2f, pinned %.2f s", who,
                        type == 120 ? "near miss" : "ricochet", g.supp, g_supPin);
                }
            }
        }
    }
}

namespace
{
    // Crash fix: when two guards block each other's path, the AI swaps their "brains" (0x10BA5130, from the AI
    // tick at 0x10BBBFE6, after AI_REQUEST_SWITCH_BRAIN): it looks in each controller's goal list (+4D0h -> head
    // +28h, next +2Ch) for the node whose next goal's byte +38h is 0 and swaps the two tails. It checks that the
    // second guard has such a node but not the first, and writes through a null pointer when he hasn't (seen in
    // the Bathhouse: a guard on his way to an alarm panel, stuck behind another). Here the swap (mov ecx,[eax+2Ch]
    // / mov [edx+2Ch],ecx / mov [eax+2Ch],ebx at 0x10BA5695) only happens when both nodes exist.
    int g_swapSkips = 0;

    void __cdecl OnBrainSwap(Mem::Regs* r)
    {
        uint8_t* a = reinterpret_cast<uint8_t*>(r->eax);
        uint8_t* d = reinterpret_cast<uint8_t*>(r->edx);
        if (!a || !d)
        {
            if (g_swapSkips++ < 20) LOG("[FIX] two guards' brain switch skipped: one of them had no goal to swap (the original crashes here)");
            return;
        }
        const uint32_t next = *reinterpret_cast<uint32_t*>(a + 0x2C);
        r->ecx = next;
        *reinterpret_cast<uint32_t*>(d + 0x2C) = next;
        *reinterpret_cast<uint32_t*>(a + 0x2C) = r->ebx;
    }
}

namespace
{
    // Ducking: the shoot goal (0x10BB0FD0, goal in esi, controller in edi) sets the pawn's bWantsToCrouch (+2B8h bit 0)
    // every tick from its stance byte (goal +D4h: 9/10 crouched, else standing; read only there and when the goal
    // starts), so a guard firing standing kept standing whatever landed around him. Hooked on that write
    // ('mov [eax+2B8h],ecx', 0x10BB1254): while he's ducking, bit 0 is set, exactly as for a goal told to fire
    // crouched; the pawn crouches (the stance blend covers the change) and his crouched animations follow.
    void __cdecl OnShootStance(Mem::Regs* r)
    {
        __try
        {
            void* ctrl = reinterpret_cast<void*>(r->edi);
            const auto it = g_guards.find(ctrl);
            if (it == g_guards.end()) return;
            const float now = LevelTime(ctrl);
            const float until = it->second.duckUntil;
            if (now < until && until - now < 10.0f) r->ecx |= 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

bool Alert::WantsEvents()
{
    return g_flinch > 0 || g_supAim > 0 || g_supPin > 0 || g_warnChance > 0 || g_duck > 0;
}

void Alert::OnEvent(void* ctrl, const uint8_t* ev)
{
    if (ctrl && ev) Clusterfuck::OnGuardEvent(ctrl, ev);
    if (!ctrl || !ev || !WantsEvents()) return;
    __try { Event(ctrl, ev); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Alert::OnNpcTick(void* pawn)
{
    if (!pawn || (g_runAccel <= 0 && g_stanceBlend <= 0)) return;
    __try { NpcTick(pawn); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Alert::OnPlayerTick(void* pawn)
{
    if (!pawn || (!oShoot && !oBroadcast && !g_turnHooked)) return;
    g_sam = pawn;
    void* xlevel = At<void*>(pawn, kXLevel);
    if (xlevel != g_xlevel) { g_xlevel = xlevel; g_guards.clear(); return; }
    const float now = LevelTime(pawn);
    {
        static float lastFrame = 0;
        const float d = now - lastFrame;
        lastFrame = now;
        if (d > 0.0f && d < 0.25f) g_frameDt = d;
        if (g_turnFix) g_aimRoll = 1.0f - std::pow(0.8f, g_frameDt * g_turnHz);
    }
    if (g_guards.empty()) return;
    // Due call-outs first (collected: the broadcast can reach code that adds guards to the map).
    void* due[16];
    int n = 0;
    for (auto& [ctrl, g] : g_guards)
    {
        if (g.pending && now >= g.due && n < 16) due[n++] = ctrl;
        if (g.raise) RaiseSafe(pawn, ctrl, g, now);
        if (g_log && g.samples < 6 && g.sampleNext > 0 && now >= g.sampleNext) SampleSafe(pawn, ctrl, g, now);
    }
    for (int i = 0; i < n; ++i)
    {
        const auto it = g_guards.find(due[i]);
        if (it != g_guards.end() && it->second.pending) ReleaseSafe(pawn, due[i], it->second, now);
    }
    static float lastPrune = 0;
    if (now - lastPrune <= 5.0f && now >= lastPrune) return;
    lastPrune = now;
    for (auto it = g_guards.begin(); it != g_guards.end();)
    {
        const Guard& g = it->second;
        if (!g.pending && (now - g.touched > 60.0f || now + 1.0f < g.touched))
            it = g_guards.erase(it);
        else
            ++it;
    }
}

void Alert::Init()
{
    if (const uintptr_t a = Mem::Find("guard brain switch", "8B 48 2C 89 4A 2C 89 58 2C 8B 8F 2C 04 00 00 E8"))
        if (Mem::MidHook("guard brain switch", a, 9, &OnBrainSwap, Mem::MidMode::Replace))
            LOG("Alert: guard brain switch null-pointer crash fixed");

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring ini = path;
    ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"SCCTRemaster_Guards.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    g_log = IniFloat(ini, L"LogPerception", 0) != 0;
    g_draw = std::clamp(IniFloat(ini, L"DrawTime", 1.0f), 0.0f, 10.0f);
    g_drawAim = std::clamp(IniFloat(ini, L"DrawAimTime", 0.35f), 0.0f, 5.0f);
    g_drawReady = std::clamp(IniFloat(ini, L"DrawReady", 5.0f), 0.5f, 120.0f);
    g_callOut = std::clamp(IniFloat(ini, L"CallOutTime", 2.5f), 0.0f, 30.0f);
    g_callAgain = std::clamp(IniFloat(ini, L"CallOutAgain", 3.0f), 0.5f, 600.0f);
    g_turnHz = std::clamp(IniFloat(ini, L"TurnRate", 30.0f), 0.0f, 1000.0f);
    g_turnFix = g_turnHz > 0;
    g_drawTurn = std::clamp(IniFloat(ini, L"DrawTurnSpeed", 0.6f), 0.05f, 1.0f);
    g_raise = std::clamp(IniFloat(ini, L"GunRaiseTime", 0.7f), 0.0f, 5.0f);
    g_raiseDelay = std::clamp(IniFloat(ini, L"GunRaiseDelay", 0.1f), 0.0f, 2.0f);
    g_drawAnim = std::clamp(IniFloat(ini, L"GunDrawTime", 0.6f), 0.0f, 5.0f);
    g_uavTurn = std::clamp(IniFloat(ini, L"UAVTurnSpeed", 0.6f), 0.0f, 10.0f);
    g_combatReact = std::clamp(IniFloat(ini, L"CombatReaction", 1.0f), 0.0f, 5.0f);
    g_minShot = std::clamp(IniFloat(ini, L"NPCShotInterval", 0.066f), 0.0f, 1.0f);
    g_reacquire = std::clamp(IniFloat(ini, L"ReacquireAfter", 0.3f), 0.05f, 10.0f);
    g_aimSettle = std::clamp(IniFloat(ini, L"AimSettleTime", 1.2f), 0.0f, 10.0f);
    g_aimTrack = std::clamp(IniFloat(ini, L"AimTrackSpeed", 0.8f), 0.0f, 100.0f);
    g_aimMoving = std::clamp(IniFloat(ini, L"AimWhileMoving", 0.6f), 0.0f, 1.0f);
    g_firstShotHit = IniFloat(ini, L"FirstShotHit", 0.0f) != 0;
    g_fairRoll = IniFloat(ini, L"FairHitRoll", 1.0f) != 0;
    g_runAccel = std::clamp(IniFloat(ini, L"NPCRunAccel", 450.0f), 0.0f, 100000.0f);
    g_stanceBlend = std::clamp(IniFloat(ini, L"StanceBlendTime", 0.35f), 0.0f, 3.0f);
    g_flinch = std::clamp(IniFloat(ini, L"FlinchTime", 0.6f), 0.0f, 5.0f);
    g_supAim = std::clamp(IniFloat(ini, L"SuppressionAim", 0.6f), 0.0f, 1.0f);
    g_supHalf = std::clamp(IniFloat(ini, L"SuppressionDecay", 1.2f), 0.05f, 30.0f);
    g_supPin = std::clamp(IniFloat(ini, L"SuppressionPin", 0.3f), 0.0f, 5.0f);
    g_duck = std::clamp(IniFloat(ini, L"SuppressionDuck", 1.2f), 0.0f, 10.0f);
    g_duckLevel = std::clamp(IniFloat(ini, L"SuppressionDuckLevel", 0.6f), 0.05f, 1.0f);
    g_warnChance = std::clamp(IniFloat(ini, L"WarningShotChance", 0.5f), 0.0f, 1.0f);
    g_warnRange = std::clamp(IniFloat(ini, L"WarningShotRange", 8.0f), 0.0f, 200.0f) * 100.0f;
    g_warnPause = std::clamp(IniFloat(ini, L"WarningShotPause", 0.8f), 0.0f, 10.0f);
    g_darkAim = std::clamp(IniFloat(ini, L"DarknessAim", 0.7f), 0.0f, 1.0f);
    g_woundAim = std::clamp(IniFloat(ini, L"WoundedAim", 0.5f), 0.0f, 1.0f);

    if (g_turnFix)
    {
        bool a = false, b = false, c = false;
        if (const uintptr_t p = Mem::Find("guard aim turn", "68 00 00 40 3F 52 E8 ?? ?? ?? ?? 83 C4 10 8D 44 24 2C 50 8B CF E8"))
            a = Mem::CallHook("guard aim turn", p + 6, &OnTurnShoot);
        if (const uintptr_t p = Mem::Find("guard focus turn", "68 00 00 40 3F 50 E8 ?? ?? ?? ?? 8B 08 89 4C 24 3C 8B 50 04"))
            b = Mem::CallHook("guard focus turn", p + 6, &OnTurnGoal);
        if (const uintptr_t p = Mem::Find("guard goal-animation turn", "68 00 00 40 3F 50 E8 ?? ?? ?? ?? 8B 08 89 4C 24 24 8B 50 04"))
            c = Mem::CallHook("guard goal-animation turn", p + 6, &OnTurnAim);
        g_turnHooked = a || b || c;
        if (g_uavTurn > 0)
            if (const uintptr_t p = Mem::Find("UAV camera turn", "DB 44 24 78 8B 4C 24 2C 8B 96 EC 16 00 00 8D BE EC 16 00 00 51 D9 5C 24 7C 8B 5C 24 7C 52 53 E8"))
                if (Mem::MidHook("UAV camera turn", p + 25, 5, &OnUAVLook, Mem::MidMode::OriginalAfter))
                {
                    g_turnHooked = true;
                    LOG("Alert: UAV camera heads turn by time at x%.2f the original 30 fps speed (90 deg in %.2f s when attacking)",
                        g_uavTurn, 16384.0f / (5000.0f * g_turnHz * g_uavTurn));
                }
        if (const uintptr_t p = Mem::Find("suppressive fire aim scatter", "E8 ?? ?? ?? ?? F6 43 2C 10 75 13 D8 1D ?? ?? ?? ?? DF E0 F6 C4 41"))
        {
            Mem::Write<uint32_t>(p + 13, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_aimRoll)));
            g_turnHooked = true;
            LOG("Alert: suppressive fire re-aims at the original rate whatever the frame rate");
        }
        LOG("Alert: guard turning %s: %.0f original steps a second whatever the frame rate (90%% of a turn in %.2f s)%s",
            a && b && c ? "fixed" : g_turnHooked ? "PARTLY fixed (a turn not found)" : "NOT fixed (turns not found)",
            g_turnHz, std::log(0.1f) / std::log(0.75f) / g_turnHz,
            g_draw > 0 && g_drawTurn < 1.0f ? "; x slower while drawing on Sam" : "");
        if (g_draw > 0 && g_drawTurn < 1.0f)
            LOG("Alert: while drawing on Sam a guard turns at x%.2f (90%% in %.2f s)", g_drawTurn,
                std::log(0.1f) / std::log(0.75f) / (g_turnHz * g_drawTurn));
    }
    else
        LOG("Alert: guard turning as in the original (per AI tick: faster the higher the frame rate)");

    // Hit chance (independent of the rest).
    const bool aim = g_aimSettle > 0 || g_aimTrack > 0 || g_aimMoving < 1.0f || !g_firstShotHit || g_fairRoll || g_minShot > 0 || g_supAim > 0 ||
                     g_warnChance > 0 || g_darkAim > 0 || g_woundAim > 0;
    if (aim)
    {
        bool first = false, chance = false;
        if (const uintptr_t a = Mem::Find("NPC hit chance", "83 EC 10 56 57 8B 7C 24 1C 85 FF 8B F1 0F 84 ?? ?? ?? ?? 8B 86 9C 09 00 00 A8 01"))
            first = Mem::Detour("NPC hit chance", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkShouldHit), reinterpret_cast<void**>(&oShouldHit));
        if (const uintptr_t a = Mem::Find("NPC hit chance compare", "68 00 00 20 41 51 8B CE D9 1C 24 E8 ?? ?? ?? ?? D9 44 24 1C D8 0D A4 8A 0B 11"))
            chance = Mem::MidHook("NPC hit chance compare", a + 16, 10, &OnHitChance, Mem::MidMode::OriginalAfter);
        LOG("Alert: guard aim %s: hit chance x(0.2 rising to 1 over %.1f s of aiming at Sam) x(1/(1 + Sam's speed across his view / %.2f rad/s))"
            " x%.2f while he moves (from when his gun is up); first shot %s; %s", chance ? "on" : "OFF (not found)", g_aimSettle, g_aimTrack, g_aimMoving,
            g_firstShotHit ? "always hits (original)" : (first ? "a normal shot" : "always hits (hook not found)"),
            !first ? "the game's own roll (hook not found)" : (g_fairRoll ? "a shot hits with exactly its chance" :
            "the game's own roll (a guard who hasn't hit lately is sure to)"));
    }
    LOG("Alert: flinch %.2f s when hit; suppression x(1 - %.2f x level), pinned %.2f s per near miss, halving every %.1f s; "
        "warning burst %.0f%% of the time beyond %.0f m (then %.2f s pause); darkness x(1 - %.2f) at most; wounded x(1 - %.2f) at most",
        g_flinch, g_supAim, g_supPin, g_supHalf, g_warnChance * 100.0f, g_warnRange / 100.0f, g_warnPause, g_darkAim, g_woundAim);
    if (g_duck > 0)
    {
        bool ok = false;
        if (const uintptr_t a = Mem::Find("guard shooting stance", "83 C9 01 89 88 B8 02 00 00 8B 6E 78"))
            ok = Mem::MidHook("guard shooting stance", a + 3, 6, &OnShootStance, Mem::MidMode::OriginalAfter);
        LOG("Alert: suppression ducking %s: a guard under heavy fire (suppression %.2f or more) fires crouched for %.1f s",
            ok ? "on" : "OFF (shoot stance not found)", g_duckLevel, g_duck);
    }
    if (g_runAccel > 0)
        LOG("Alert: guards speed up past %.0f u/s by at most %.0f u/s per second (sprint windup)", kFreeSpeed, g_runAccel);
    if (g_stanceBlend > 0)
        LOG("Alert: guards crouching or standing up while still blend between the poses over %.2f s", g_stanceBlend);

    if (g_draw <= 0 && g_callOut <= 0 && g_combatReact <= 0 && g_minShot <= 0)
    {
        LOG("Alert: draw time, combat reaction and call-out off (guards shoot and alert their group at once, as in the original)");
        return;
    }

    // The broadcast hook also tells the draw when a guard spotted Sam, so it goes in for either.
    if (const uintptr_t a = Mem::Find("group alert", "83 EC 28 53 8B 5C 24 30 55 56 57 8B 7C 24 40 8B 97 F4 04 00 00 33 C0 8B E9"))
        Mem::Detour("group alert", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkBroadcast), reinterpret_cast<void**>(&oBroadcast));
    if (g_draw > 0 || g_combatReact > 0 || g_minShot > 0)
        if (const uintptr_t a = Mem::Find("NPC shot", "A0 ?? ?? ?? ?? 83 EC 3C A8 01 53 55 56 57 8B F1 BD 01 00 00 00 75 1F"))
            Mem::Detour("NPC shot", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkShoot), reinterpret_cast<void**>(&oShoot));

    if (g_draw > 0)
        LOG("Alert: draw time %s: a guard's first shot at Sam comes %.2f s (x his reaction) after he spots him, with his gun on Sam "
            "for at least %.2f s; again after %.0f s without shooting at him", oShoot ? "on" : "OFF (NPC shot not found)",
            g_draw, g_drawAim, g_drawReady);
    if (g_draw > 0 && oShoot)
    {
        if (g_raise > 0)
            LOG("Alert: gun raise on: a guard whose gun was down brings it up over %.2f s after %.2f s, one whose gun was "
                "put away draws it over %.2f s first (x0.8-1.4 by reaction); he shoots only once it's up", g_raise, g_raiseDelay, g_drawAnim);
        else
            LOG("Alert: gun raise off (the game's own 0.2 s tween)");
    }
    if (g_callOut > 0)
        LOG("Alert: call-out %s: a guard who spots Sam tells the guards around him after %.2f s (x his reaction), "
            "unless he's stopped first (a hit holds it back); finding Sam again after %.1f s without him (even mid-fight) "
            "has to be called in again", oBroadcast ? "on" : "OFF (group alert not found)", g_callOut, g_callAgain);
    else if (!oBroadcast)
        LOG("Alert: group alert not found: the draw time counts from his gun coming up only");
    if (g_minShot > 0)
        LOG("Alert: NPC fire rate %s: at most one shot every %.3f s (firing at a spot behind a door went off every frame)",
            oShoot ? "capped" : "NOT capped (NPC shot not found)", g_minShot);
    if (g_combatReact > 0)
        LOG("Alert: combat reaction %s: a guard who gets Sam back in his sights after %.2f s out of them holds fire for his reaction time (x%.2f)",
            oShoot ? "on" : "OFF (NPC shot not found)", g_reacquire, g_combatReact);
}
