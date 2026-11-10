// Weapon accuracy and the mouse ([Mouse] AimAccuracyFix, AimFreeTurnSpeed, AimFullTurnSpeed, PerfectStillAim and
// AimLog in SCCTRemaster.ini).
//
// The player's gun ticks its accuracy in EWeapon 0x10BF8280 (thiscall (DeltaTime, ...), ret 8): the accuracy it
// wants (AccuracyDesired +52Ch) is the worst of AccuracyBase (+528h, x0.6 crouched; SC-20K 0.5, pistol 0.8),
// Sam's movement (EPawn +304h) x AccuracyMovementModifier (+520h; 6 / 9) and, once the camera's turn or look rate
// passes 0.4, (rate - 0.4) x 1.67 x the modifier. The rate is EPlayerCam m_dampedTurn / m_dampedLookUp
// (+1244h/+1248h) clamped to +-1. Accuracy (+530h) jumps straight to a worse value and comes back by
// AccuracyReturnModifier (+524h; 16 / 24) x dt x (how far it is from the base), never below the base. The shot's
// spread is int(sqrt(RealAccuracy +534h) x 4.5) (0x10BF7FD0): 0 below 0.049, 3 at the SC-20K's base.
// For the mouse the camera's rate is aTurn / aLookUp, which PlayerInput builds as this frame's mouse delta
// (aMouseX/Y, controller +3A0h/+3A4h) / DeltaTime, and the camera update (0x10C09330) copies it whenever the
// mouse moved, else eases it toward the stick input at a fixed rate x dt (+2F4h of the camera mode's 70h-byte
// entry). The camera then turns by rate x a mode/zoom factor x m_yawSpeed (+127Ch, 40000) / m_pitchSpeed (+1280h)
// x dt. So the penalty is on one frame's input: at 160 fps a one-count nudge reads 5.3x faster than at 30 fps,
// and where the aiming camera turns slower for the same input, slow on-screen turning already counts.
// AimAccuracyFix: the accuracy reads our own value instead, from how fast the view (the camera's Rotation +F4h,
// or Sam's controller's, whichever turned more) actually turned over the last 0.1 s (signed, so small back-and-forth corrections cancel): nothing below
// AimFreeTurnSpeed degrees a second, rising to the weapon's full penalty at AimFullTurnSpeed. Without mouse
// movement for 2 s (a gamepad) it reads the camera's own value as before.
// PerfectStillAim: while the mouse sends nothing at all (no deltas in the last 1/30 s, the camera's rates exactly
// 0), the accuracy floor is 0 instead of AccuracyBase, so a still aim keeps settling to a perfect shot.
#include "pch.h"
#include "features.h"
#include "config.h"

namespace
{
    template <class T> T& At(void* p, uintptr_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p) + off); }

    constexpr uintptr_t kLevel = 0x78, kTime = 0x430, kDeltaTime = 0x75C, kPitch = 0xF4, kYaw = 0xF8;
    constexpr uintptr_t kCamCtrl = 0x1198, kDampedTurn = 0x1244, kDampedLook = 0x1248;
    constexpr uintptr_t kMouseX = 0x3A0, kMouseY = 0x3A4;
    constexpr uintptr_t kWeaponAccuracy = 0x530, kWeaponModifier = 0x520;
    constexpr float kStillWindow = 1.0f / 30.0f, kSpeedWindow = 0.1f, kGamepadAfter = 2.0f;
    constexpr float kUnitsToDeg = 360.0f / 65536.0f;

    struct Sample { float t, dt; bool mouse; int dyaw, dpitch, cyaw, cpitch; };   // camera and controller turns
    Sample g_samples[128];
    int g_head = 0, g_count = 0;
    float g_lastT = -1.0f, g_lastMouse = -100.0f, g_now = 0.0f;
    int g_lastYaw = 0, g_lastPitch = 0, g_lastCYaw = 0, g_lastCPitch = 0;
    bool g_haveRot = false;
    bool g_moving = false;      // mouse deltas in the last 1/30 s
    float g_speed = 0.0f;       // view turn speed, deg/s over the last 0.1 s
    float g_frameInput = 0.0f;  // the camera's own rate this frame (for the log)
    bool g_fix = false, g_perfect = false, g_log = false;
    float g_free = 90.0f, g_full = 360.0f;
    float g_lastLog = -100.0f;

    int Wrap(int d) { return ((d + 0x8000) & 0xFFFF) - 0x8000; }

    // Once per camera update: this frame's mouse deltas and how far the view turned since the last one.
    void SampleMouse(void* cam)
    {
        void* ctrl = At<void*>(cam, kCamCtrl);
        void* level = At<void*>(cam, kLevel);
        if (!ctrl || !level) return;
        const float now = At<float>(level, kTime);
        const float dt = At<float>(level, kDeltaTime);
        if (now == g_lastT) return;
        if (now < g_lastT) { g_count = 0; g_lastMouse = -100.0f; g_haveRot = false; }   // a load / new level
        g_lastT = now;
        g_now = now;
        const int yaw = At<int>(cam, kYaw) & 0xFFFF, pitch = At<int>(cam, kPitch) & 0xFFFF;
        const int cyaw = At<int>(ctrl, kYaw) & 0xFFFF, cpitch = At<int>(ctrl, kPitch) & 0xFFFF;
        int dyaw = g_haveRot ? Wrap(yaw - g_lastYaw) : 0, dpitch = g_haveRot ? Wrap(pitch - g_lastPitch) : 0;
        int dcyaw = g_haveRot ? Wrap(cyaw - g_lastCYaw) : 0, dcpitch = g_haveRot ? Wrap(cpitch - g_lastCPitch) : 0;
        if (std::abs(dyaw) > 8192 || std::abs(dpitch) > 8192) dyaw = dpitch = 0;       // a cut, not a turn
        if (std::abs(dcyaw) > 8192 || std::abs(dcpitch) > 8192) dcyaw = dcpitch = 0;
        g_lastYaw = yaw; g_lastPitch = pitch; g_lastCYaw = cyaw; g_lastCPitch = cpitch; g_haveRot = true;
        const bool mouse = At<float>(ctrl, kMouseX) != 0.0f || At<float>(ctrl, kMouseY) != 0.0f;
        if (mouse) g_lastMouse = now;
        g_samples[g_head] = { now, std::max(dt, 0.0f), mouse, dyaw, dpitch, dcyaw, dcpitch };
        g_head = (g_head + 1) % 128;
        if (g_count < 128) ++g_count;
        g_frameInput = std::max(std::fabs(At<float>(cam, kDampedTurn)), std::fabs(At<float>(cam, kDampedLook)));

        int sy = 0, sp = 0, scy = 0, scp = 0;
        float span = 0;
        bool moving = false;
        for (int i = 0; i < g_count; ++i)
        {
            const Sample& o = g_samples[(g_head + 127 - i) % 128];
            const float age = now - o.t;
            if (i > 0 && age >= kSpeedWindow) break;
            sy += o.dyaw; sp += o.dpitch; scy += o.cyaw; scp += o.cpitch; span += o.dt;
            if (o.mouse && (i == 0 || age < kStillWindow)) moving = true;
        }
        g_moving = moving;
        const float deg = std::max(std::sqrt(float(sy) * sy + float(sp) * sp), std::sqrt(float(scy) * scy + float(scp) * scp)) * kUnitsToDeg;
        g_speed = deg / std::max(span, kSpeedWindow);
    }

    void __cdecl OnCamera(Mem::Regs* r)
    {
        __try { SampleMouse(reinterpret_cast<void*>(r->ecx)); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    bool MouseInUse() { return g_now - g_lastMouse < kGamepadAfter; }

    // The value the accuracy tick takes as the turn rate: 0.4 (no penalty yet) at AimFreeTurnSpeed, 1 (full) at
    // AimFullTurnSpeed.
    float TurnValue()
    {
        if (g_speed <= g_free) return g_free > 0 ? 0.4f * g_speed / g_free : 0.4f;
        return 0.4f + 0.6f * std::min(1.0f, (g_speed - g_free) / std::max(g_full - g_free, 1.0f));
    }

    // The accuracy tick's reads of the camera's rates (camera in ebx): mov edx, [ebx+1244h] / mov eax, [ebx+1248h].
    void __cdecl OnReadTurn(Mem::Regs* r)
    {
        float v = 0.0f;
        __try { v = At<float>(reinterpret_cast<void*>(r->ebx), kDampedTurn); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (g_fix && MouseInUse()) v = TurnValue();
        r->edx = *reinterpret_cast<uint32_t*>(&v);
    }
    void __cdecl OnReadLook(Mem::Regs* r)
    {
        float v = 0.0f;
        __try { v = At<float>(reinterpret_cast<void*>(r->ebx), kDampedLook); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (g_fix && MouseInUse()) v = 0.0f;   // the turn value already covers both axes
        r->eax = *reinterpret_cast<uint32_t*>(&v);
    }

    // Before the accuracy tick loads its floor (AccuracyBase, x0.6 crouched, at [esp+20h]; weapon in esi, camera
    // in ebx): 0 while perfectly still. Also the [AIM] log of what turning costs.
    void __cdecl OnFloor(Mem::Regs* r)
    {
        __try
        {
            void* cam = reinterpret_cast<void*>(r->ebx);
            void* weapon = reinterpret_cast<void*>(r->esi);
            const bool still = !g_moving && At<float>(cam, kDampedTurn) == 0.0f && At<float>(cam, kDampedLook) == 0.0f;
            if (g_perfect && still) r->Stack<float>(0x20) = 0.0f;
            if (g_log && g_fix && MouseInUse() && g_now - g_lastLog > 0.5f)
            {
                const float v = TurnValue();
                if (v > 0.4f || g_frameInput > 0.4f)
                {
                    g_lastLog = g_now;
                    const float mod = At<float>(weapon, kWeaponModifier);
                    const float now = v > 0.4f ? (v - 0.4f) * 1.6667f * mod : 0.0f;
                    const float orig = g_frameInput > 0.4f ? (std::min(g_frameInput, 1.0f) - 0.4f) * 1.6667f * mod : 0.0f;
                    LOG("[AIM] view turning %.0f deg/s (free up to %.0f): accuracy penalty %.2f (original: %.2f from input %.2f); "
                        "accuracy %.2f, floor %.2f", g_speed, g_free, now, orig, g_frameInput,
                        At<float>(weapon, kWeaponAccuracy), r->Stack<float>(0x20));
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void Aim::Init()
{
    g_fix = g_settings.aimAccuracyFix;
    g_perfect = g_settings.perfectStillAim;
    g_free = g_settings.aimFreeTurnSpeed;
    g_full = std::max(g_settings.aimFullTurnSpeed, g_free + 1.0f);
    g_log = g_settings.aimLog;
    if (!g_fix && !g_perfect)
    {
        LOG("Aim: accuracy from the mouse as in the original");
        return;
    }
    bool cam = false, reads = false, floor = false;
    if (const uintptr_t a = Mem::Find("camera update (mouse sample)", "83 EC 40 53 55 56 8B F1 8B 86 9C 11 00 00 05 F4 00 00 00"))
        cam = Mem::MidHook("camera update (mouse sample)", a, 5, &OnCamera, Mem::MidMode::OriginalAfter);
    if (g_fix && cam)
    {
        const uintptr_t t = Mem::Find("accuracy turn read", "8B 9F C0 07 00 00 8B 93 44 12 00 00 55 68 00 00 80 3F 68 00 00 80 BF 52 E8");
        const uintptr_t l = Mem::Find("accuracy look read", "8B 83 48 12 00 00 68 00 00 80 3F 68 00 00 80 BF 50 E8");
        if (t && l)
            reads = Mem::MidHook("accuracy turn read", t + 6, 6, &OnReadTurn, Mem::MidMode::Replace) &&
                    Mem::MidHook("accuracy look read", l, 6, &OnReadLook, Mem::MidMode::Replace);
    }
    if (cam)
        if (const uintptr_t a = Mem::Find("accuracy floor", "D9 80 04 03 00 00 8B 6C 24 20 D8 8E 20 05 00 00"))
            floor = Mem::MidHook("accuracy floor", a, 6, &OnFloor, Mem::MidMode::OriginalAfter);
    if (!cam) { g_fix = g_perfect = false; LOG("Aim: camera update not found; accuracy as in the original"); return; }
    g_fix = g_fix && reads;
    g_perfect = g_perfect && floor;
    if (g_fix)
        LOG("Aim: turning costs accuracy from how fast the view turns over 0.1 s: nothing up to %.0f deg/s, the most at %.0f deg/s",
            g_free, g_full);
    else
        LOG("Aim: turning costs accuracy as in the original");
    LOG("Aim: perfectly still aim %s", g_perfect ? "settles to a perfect shot" : "keeps the weapon's usual floor");
}
