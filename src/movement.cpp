// Sam's walking speed: even diagonal speed ([Gameplay] EvenDiagonalSpeed) and finer mouse-wheel speed
// stages ([Gameplay] SpeedStages).
//
// Diagonal speed (vanilla bug): movement keys are bound as "Axis aForward Speed=+1.0" and
// "Axis aStrafe Speed=-1.0". For every held key (IST_Hold) UInput's Axis command (inside UInput::Exec,
// 0x10A35D70) STORES Speed x Delta x PlayerController.mouseWheelSpeedFactor into that axis. Sam's speed is
// driven by the "pushing force" sqrt(aForward^2 + aStrafe^2) (natives EPlayerController.GetPushingForce
// 0x10BBC9D0 and PlayOnGround 0x10BBE1B0), mapped onto small step .. walk .. run and footstep noise.
// Forward + strafe keys therefore push with 1.41x the force of a single key: faster and louder, up to a
// full run at the slowest wheel setting's double.
// Fix: right after a held key stores aForward or aStrafe, rescale the pair so its length equals the larger
// of the two. The direction is unchanged and a single key is untouched. Analog sticks (IST_Axis) are left
// alone; their diagonals are already the stick's real deflection.
//
// Speed stages: "AxisMod SpeedInc=0.20" (mouse wheel) adds SpeedInc to mouseWheelSpeedFactor, clamped to
// 0.2..1.0, i.e. 5 stages. SpeedStages N spaces N stages evenly over the same range, snapped to a grid so
// the vanilla stages stay reachable when (N - 1) is a multiple of 4 (5, 9, 13, 17 ...).
//
// PlayerController: aForward +3B0h, aStrafe +3B4h, mouseWheelSpeedFactor +3F0h (field order from Engine.u:
// aMouseX, aMouseY, aBakForward, aBakStrafe, aForward, aStrafe, aTurn, aLookUp ... mouseWheelSpeedFactor).
// UInput (the 'this' Exec runs with): Viewport +E78h (Viewport->Actor +30h), current action +F4Ch
// (EInputAction: 1 press, 2 hold, 3 release, 4 axis).
//
// Optic cable speed ([Mouse] OpticCableSpeed): EOpticCable.s_Sneaking.Tick turns the camera by
// PlayerController.aTurn x Damping every tick (pitch: aLookUp x Damping x 1.5). aTurn is a rate (the input
// script adds aMouseX / DeltaTime), so turning per second grew with the frame rate: about 5.5x too fast at
// 165 FPS compared with the 30 FPS the game was tuned at (the sticky camera multiplies by DeltaTime and is
// fine). Fix: for the duration of the optic cable's tick, Damping becomes Damping x DeltaTime x 10 x
// OpticCableSpeed: 10000 units/s per unit of aTurn, exactly the sticky camera's rate (EStickyCamera.Damping
// 10000 x DeltaTime at full zoom-out), at any frame rate. (dev.8 used 40000, which the user found still far
// too fast with a mouse.) Restored right after the tick (saved games keep 1000).
// EOpticCable.Damping is at +428h (property layout computed from EchelonIngredient.u, default 1000).
#include "pch.h"
#include "features.h"

namespace
{
    constexpr uintptr_t kViewport = 0xE78, kViewportActor = 0x30, kAction = 0xF4C;
    constexpr uintptr_t kForward = 0x3B0, kStrafe = 0x3B4;
    constexpr int kHold = 2;
    constexpr float kMinFactor = 0.2f, kMaxFactor = 1.0f;

    // Per axis: the value the key binding stored this frame, and what we wrote back after rescaling.
    struct AxisState { float raw = 0.0f, written = 0.0f; };
    AxisState g_axes[2];
    int g_stages = 0;
    bool g_loggedDiagonal = false;

    template <class T> T& At(uintptr_t base, uintptr_t off) { return *reinterpret_cast<T*>(base + off); }

    constexpr uintptr_t kOpticDamping = 0x428;
    constexpr float kOpticDefaultDamping = 1000.0f;
    constexpr float kOpticRate = 10.0f;   // 1000 x 10 = 10000 units/s per unit of aTurn, the same as the sticky camera
    void* g_opticActor = nullptr;
    float g_opticSaved = 0.0f;

    // Runs right after the Axis command stored a value into the axis at edi (esi = UInput).
    void __cdecl OnAxisStored(Mem::Regs* r)
    {
        if (At<int>(r->esi, kAction) != kHold)
            return;
        const uintptr_t viewport = At<uintptr_t>(r->esi, kViewport);
        const uintptr_t pc = viewport ? At<uintptr_t>(viewport, kViewportActor) : 0;
        if (!pc)
            return;
        int which;
        if (r->edi == pc + kForward)
            which = 0;
        else if (r->edi == pc + kStrafe)
            which = 1;
        else
            return;

        float* axes = reinterpret_cast<float*>(pc + kForward);   // [0] aForward, [1] aStrafe
        float& mine = axes[which];
        float& other = axes[1 - which];
        AxisState& me = g_axes[which];
        AxisState& them = g_axes[1 - which];
        me.raw = mine;
        // The other axis holds either what its key stored this frame, or our rescaled version of that
        // (then its raw value is the one we recorded). A second key on the same axis stores again, so
        // the pair is always rescaled from the raw values.
        if (!(other != 0.0f && other == them.written))
            them.raw = other;

        const float a = me.raw, b = them.raw;
        const float big = std::max(std::fabs(a), std::fabs(b));
        const float len = std::sqrt(a * a + b * b);
        if (big > 0.0f && len > big * 1.0001f)
        {
            const float k = big / len;
            mine = a * k;
            other = b * k;
            if (!g_loggedDiagonal)
            {
                g_loggedDiagonal = true;
                LOG("Movement: diagonal key input evened out (forward %.3f, strafe %.3f -> x%.3f)", axes[0] / k, axes[1] / k, k);
            }
        }
        else
        {
            mine = a;
            other = b;
        }
        me.written = mine;
        them.written = other;
    }

    // Runs right before "fld SpeedInc; fadd mouseWheelSpeedFactor" (esi = &mouseWheelSpeedFactor,
    // [esp+14h] = SpeedInc parsed from the binding). Turns the step into "next stage up/down".
    void __cdecl OnSpeedInc(Mem::Regs* r)
    {
        float& inc = r->Stack<float>(0x14);
        if (inc == 0.0f || g_stages < 2)
            return;
        const float cur = *reinterpret_cast<const float*>(r->esi);
        const float step = (kMaxFactor - kMinFactor) / static_cast<float>(g_stages - 1);
        int stage = static_cast<int>(std::lround((std::clamp(cur, kMinFactor, kMaxFactor) - kMinFactor) / step));
        stage = std::clamp(stage + (inc > 0.0f ? 1 : -1), 0, g_stages - 1);
        inc = kMinFactor + static_cast<float>(stage) * step - cur;   // the game clamps to 0.2..1.0 afterwards
    }
}

namespace
{
    using SplitCheck_t = int(__fastcall*)(void* ctrl, void* edx);
    SplitCheck_t g_splitCheck = nullptr;   // EPlayerController split-zone check (0x10BCA8E0)
    using UpdateLastAction_t = void(__fastcall*)(void* pawn, void* edx, void* stack, void* result);
    UpdateLastAction_t oUpdateLastAction = nullptr;   // native 1699 (see "Crouch noise")
    void __fastcall hkUpdateLastAction(void* pawn, void* edx, void* stack, void* result);
    using StopRelaxed_t = void(__fastcall*)(void* pawn, void* edx, float time);
    StopRelaxed_t g_stopRelaxed = nullptr;   // EPawn 0x10B80220 (see "Instant crouch switch")
}

void Movement::Init()
{
    const auto& s = g_settings;
    if (s.evenDiagonalSpeed)
    {
        // Key hold with the mouse-wheel factor: ... fmul [edx+3F0h]; mov eax,1; fstp [edi]; | pop edi; pop esi; mov ecx,[esp+210h]
        const uintptr_t a = Mem::Find("Axis store (wheel factor)", "D8 8A F0 03 00 00 B8 01 00 00 00 D9 1F 5F 5E 8B 8C 24 10 02 00 00");
        // Other axis stores (key hold without the factor, analog): fld [esp+14h]; fstp [edi]; | pop edi; mov eax,1
        const uintptr_t b = Mem::Find("Axis store", "D9 44 24 14 D9 1F 5F B8 01 00 00 00 5E 8B 8C 24 10 02 00 00");
        const bool ok = a && b &&
                        Mem::MidHook("Axis store (wheel factor)", a + 0x0D, 9, &OnAxisStored, Mem::MidMode::OriginalAfter) &&
                        Mem::MidHook("Axis store", b + 0x06, 6, &OnAxisStored, Mem::MidMode::OriginalAfter);
        LOG("Movement: even diagonal speed %s", ok ? "on" : "FAILED");
    }
    if (s.opticCableSpeed > 0.0f)
        LOG("Movement: optic cable turn speed x%.2f, frame-rate independent", s.opticCableSpeed);
    if (s.speedStages >= 2)
    {
        // add esi,3F0h; call Parse; | fld [esp+14h]; fadd [esi] | add esp,0Ch; push 1.0f
        const uintptr_t c = Mem::Find("AxisMod SpeedInc", "81 C6 F0 03 00 00 E8 ? ? ? ? D9 44 24 14 D8 06 83 C4 0C 68 00 00 80 3F");
        if (c && Mem::MidHook("AxisMod SpeedInc", c + 0x0B, 6, &OnSpeedInc, Mem::MidMode::OriginalAfter))
        {
            g_stages = s.speedStages;
            LOG("Movement: %d mouse-wheel speed stages (step %.3f)", g_stages, (kMaxFactor - kMinFactor) / (g_stages - 1));
        }
        else
            LOG("Movement: speed stages FAILED");
    }
    if (s.crouchNoiseFix || s.logCrouch || s.quietStandingSteps)
    {
        // EPawn native 1699: P_FINISH, then fld [esi+308h] (SoundWalkingRatio) ...
        const uintptr_t a = Mem::Find("UpdateLastAction (native 1699)",
            "8B 44 24 04 56 8B F1 8B 48 0C 41 89 48 0C 80 39 42 75 10 41 6A 00 89 48 0C 8B 48 08 50 FF 15 ? ? ? ? D9 86 08 03 00 00");
        const bool ok = a && Mem::Detour("UpdateLastAction", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkUpdateLastAction),
                                         reinterpret_cast<void**>(&oUpdateLastAction));
        LOG("Movement: crouch noise fix %s%s%s", !s.crouchNoiseFix ? "off" : ok ? "on" : "FAILED", s.logCrouch ? ", crouch log on" : "",
            s.quietStandingSteps ? (ok ? ", standing footsteps as quiet as crouched" : ", quiet standing footsteps FAILED") : "");
    }
    if (s.crouchInstantSwitch)
    {
        g_stopRelaxed = reinterpret_cast<StopRelaxed_t>(Mem::Find("stop relaxed pose (EPawn)",
            "56 8B F1 8B 8E 84 01 00 00 85 C9 74 ? 8B 01 57 56 FF 90 84 00 00 00 8B F8 85 FF 74 ? 8B 4E 7C DD 81 C4 00 00 00 6A 09 8B CF D9 9E B4 07 00 00 E8"));
        LOG("Movement: instant crouch switch on%s", g_stopRelaxed ? "" : " (relaxed-pose fade not found)");
    }
    if (s.wallBounce)
    {
        if (s.wallBounceSplit)
            g_splitCheck = reinterpret_cast<SplitCheck_t>(Mem::Find("split-zone check",
                "83 EC 48 A1 ? ? ? ? 53 55 56 8B F1 8B 0D ? ? ? ? 8B 56 7C 57 89 4C 24 24 8B 8A 10 3A 00 00"));
        LOG("Movement: wall bounce on (up %.0f, out %.0f, reach %.0f, %d per jump, animation %ls for %.2f s, noise %d, "
            "into split jumps %s)", s.wallBounceUp, s.wallBounceOut, s.wallBounceReach, s.wallBounceMax, s.wallBounceAnim.c_str(),
            s.wallBounceAnimTime, s.wallBounceNoise ? 1 : 0, g_splitCheck ? "on" : s.wallBounceSplit ? "FAILED" : "off");
    }
}

void Movement::BeforeOpticTick(void* opticCable, float dt)
{
    if (g_settings.opticCableSpeed <= 0.0f || !opticCable || dt <= 0.0f)
        return;
    float& damping = At<float>(reinterpret_cast<uintptr_t>(opticCable), kOpticDamping);
    if (std::fabs(damping - kOpticDefaultDamping) > 0.5f)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            LOG("Movement: optic cable Damping is %.3f, not %.0f; turn speed fix disabled", damping, kOpticDefaultDamping);
        }
        return;
    }
    g_opticActor = opticCable;
    g_opticSaved = damping;
    damping = kOpticDefaultDamping * std::min(dt, 0.1f) * kOpticRate * g_settings.opticCableSpeed;
}

void Movement::AfterOpticTick(void* opticCable)
{
    if (g_opticActor && g_opticActor == opticCable)
    {
        At<float>(reinterpret_cast<uintptr_t>(opticCable), kOpticDamping) = g_opticSaved;
        g_opticActor = nullptr;
    }
}

// ---- Wall bounce ([Gameplay] WallBounce) ------------------------------------------------------------------
// Splinter Cell 1 / Pandora Tomorrow's wall jump, removed in Chaos Theory (it has no animation for it). In the
// air (Physics +4Ch = PHYS_Falling 2, controller in script state s_PlayerJumping, not bInTransition +550h bit 1),
// a fresh press of jump (EPlayerController.bJump +5FEh, the "Button bJump" input byte) with a wall within
// WallBounceReach of Sam's side in any direction (12 traces around him at chest and knee height, the nearest
// wall wins; trace flags 17h = the script Trace's world geometry without actors, so boxes, movers and static
// meshes count, characters don't) kicks him up it: Velocity (+100h) = a 30 units/s nudge off the wall + 3/4 of
// his speed along the wall, Z = WallBounceUp; he only goes away from the wall when he steers that way (below).
// So running at a wall or alongside it, jumping and pressing jump again works.
// Steering: the direction held on the movement keys (EPlayerController aForward +3B0h / aStrafe +3B4h, turned
// by the controller's yaw +F8h; in the air the game itself only pushes Sam along his facing) replaces that push
// with WallBounceBoost in that direction (never slower than he already went that way, and always clear of the
// wall), at the bounce and for WallBounceSteerTime after it: run at a wall, jump, jump, hold back = a boost back.
// Keys pointing into the wall, or none, keep the plain kick straight up. A steered kick always leaves the wall
// at WallBounceOut / 2 or more.
// Chaos Theory's own split jump only starts from the ground: EPlayerController native 1187 (0x10BCACA0, called
// from s_PlayerWalking.PlayerMove) runs the split-zone check 0x10BCA8E0 (Sam inside a split-zone volume, walls
// traced for it; it stores the split's start location/rotation in the pawn) and then GotoState('s_Split') +
// GotoLabel('Split') (UObject vtable +30h (FName, 0) -> 1 on success, +34h (FName, 0, 0)), so the bounce never
// gets in its way. WallBounceSplit: after a bounce, that check runs every tick until Sam lands, as if he stood at
// standing height (StandHeight +328h) above the floor straight below him (Location.z +F0h is set for the call
// and put back): in the air above the spot the check fails, and by the time he comes down to that height the
// steering boost has carried him past it; the check is also tried 25/50 units either way across the corridor
// (the spot is a narrow band). When it passes he stops drifting (horizontal velocity 0) and, once he's up at the
// split's height (or starts falling), is put straight into the split pose (see EnterSplitDirect), skipping the
// split's intro (which drops him to the start height and plays a 2.5 s root-motion climb). If that fails, the
// game's own split is used, from where he is. All of this only while jump is held (a tap just bounces, so wall
// jumps can be chained), and with SplitFromFall also while falling without a bounce (corridor axis from the
// nearest wall), e.g. to catch a fall; after dropping out of a split, spots as high as it or higher are ignored
// until Sam lands, so he can't snap straight back in. A split that catches a fast fall costs SplitCatchDamage of
// the fall damage (see SplitCatchDamage). Getting there is a 0.3 s glide (position and facing eased, pose
// tweened in through PlaySplit with a 0.3 s TweenTime), with the controller held in transition until he's there.
// The kick animation moves one leg: EPawn.BlendAnimOverCurrent (native 1538: Sequence, BlendStrength, BoneName,
// Rate, TweenTime, Channel, bBackward, BlendTime, in_loop, in_pauseatframe; natively AnimBlendParams(Channel, 0,
// 0, 0, Bone), AnimBlendToAlpha(Channel, BlendStrength, BlendTime), PlayAnim on the channel) from "B R Thigh" or
// "B L Thigh" down, on blend channel 14 (the scripts use 1-7, 10, 15, 18) over whatever the body does. A wall to
// the side: the leg on that side (wall direction -normal against Sam's right, Rotation yaw + 90 degrees) takes
// the split pose (WallBounceAnim) at WallBounceKickStrength (the split leg sticks out sideways; half of it is a
// leg angled down). A wall in front (within 40 degrees of his facing): the foot-on-ledge hoist pose
// (WallBounceFrontAnimRight/Left, ledgcralftr/l: knee up, foot forward) at WallBounceFrontFrame and
// WallBounceFrontStrength. The channel (MeshInstance +64h -> Blends TArray +94h/+98h, 80h bytes each: sequence
// FName +00h, rate +08h, frame +0Ch (0..1), BlendAlpha +30h, blend time +3Ch, target alpha +40h, bone index byte
// +46h) is checked for the sequence (a streamed animation that isn't loaded gets replaced; then the side pose is
// used), frozen at the pose frame and driven by our own envelope every tick. A partial-strength pose blends over
// whatever the leg is doing, and the in-air animation changes after a bounce (knees bent in the running jump,
// straight in later ones), so the kicking leg is first held in a fixed reference pose on the channel below (the
// full-body pose Sam had at the first bounce of a running jump, kept across jumps) and the kick blends over that:
// every kick looks the same. (PlayInAir's LoopAnimOnly fades all
// other channels, target 0 over 0.2 s, right after the bounce): up and back to 0 within WallBounceAnimTime, one
// kick. If no pose plays, or with WallBounceLeg = Both, the whole body plays WallBounceAnim instead:
// EPawn.PlayAnimNoMovement (a script function; it zeroes GroundSpeed +2E8h, which is put back) held by setting
// EPawn.SpecialAnim (+980h), which s_PlayerJumping's PlayInAir keeps playing until it's cleared.
// EPawn.PlayJump (script) makes the jump sound and its AI noise. For 4 s after a bounce, changes of Sam's
// controller state (StateFrame +0Ch -> node +1Ch), physics and Base (+A8h) are logged ([MOVE] after bounce).
// Only a moving jump bounces: Sam's horizontal speed when he left the ground (the first PHYS_Falling tick) must
// be at least WallBounceMinSpeed, so jumping up at something to climb it doesn't kick off the wall. With
// WallBounceStanding a standing jump bounces too, straight up (WallBounceStandingUp, a 30 units/s nudge off the
// wall, no steering).
//
// ---- Landing roll ([Gameplay] RollOnlyWhenCrouching) ------------------------------------------------------
// s_PlayerJumping.NotifyLanded rolls when ShouldRoll(): moving forward faster than 180, falling faster than 400
// and (bWantsToCrouch || Velocity.Z < -600). The -600 (EX_FloatConst 1Eh, C4160000h) in that state function's
// bytecode (UFunction found with FindFunction while Sam is in s_PlayerJumping; its Script TArray located by
// scanning the object for an array holding both -600 and -400 constants) becomes -1e9: only a crouch press
// (bWantsToCrouch) makes him roll.
//
// ---- Climbing over railings ([Gameplay] ClimbOverRailings) ------------------------------------------------
// Hanging from a ledge (s_Ledge), forward or jump hoists him (native1163 room check -> s_HoistingAboveLedge) or,
// below a railing, climbs over it (label 'OverRailing': climb animation, then SetLocation(EPawn.m_locationEnd
// +368h), crouched, s_PlayerWalking). Hanging from a railing's top bar (where a bounce often lands him) both
// fail. Then, after 0.15 s of holding forward/jump in s_Ledge (not bInTransition +550h bit 1): a crouched box
// (CollisionRadius +220h, CrouchHeight +320h) is swept from just above the hands (Location + m_LedgeGrabArmsZone
// +550h z) forward over the edge and down to a walkable floor (normal z >= 0.7); m_locationStart +35Ch /
// m_locationEnd are set and the controller goes to label 'OverRailing' (UObject vtable +34h GotoLabel).
namespace
{
    struct Vec3 { float x, y, z; };
    using SingleLineCheck_t = int(__fastcall*)(void* level, void* edx, uint8_t* hit, void* source, const Vec3* end, const Vec3* start,
                                               uint32_t flags, float ex, float ey, float ez, int node, int extra);
    using GotoState_t = int(__fastcall*)(void* obj, void* edx, int state, int arg);
    using GotoLabel_t = int(__fastcall*)(void* obj, void* edx, int label, int a, int b);

    template <class T> T& Fld(void* p, uintptr_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p) + off); }
    float Dot2(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y; }

    // Hit.Actor +4, Hit.Location +8, Hit.Normal +14h; Time +24h and Item +28h preset like the engine does.
    bool TraceWall(void* level, void* source, const Vec3& start, const Vec3& end, Vec3& normal, float& time)
    {
        alignas(8) uint8_t buf[0x40] = {};
        *reinterpret_cast<float*>(buf + 0x24) = 1.0f;
        *reinterpret_cast<int*>(buf + 0x28) = -1;
        __try
        {
            const auto slc = reinterpret_cast<SingleLineCheck_t>((*reinterpret_cast<void***>(level))[0xDC / 4]);
            slc(level, nullptr, buf, source, &end, &start, 0x17, 0, 0, 0, 0, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!*reinterpret_cast<void**>(buf + 4)) return false;
        normal = *reinterpret_cast<Vec3*>(buf + 0x14);
        time = *reinterpret_cast<float*>(buf + 0x24);
        return std::fabs(normal.z) < 0.4f;   // walls, not floors or ceilings
    }

    // The nearest wall around Sam within 'reach' of his centre, at chest or knee height.
    bool NearestWall(void* level, void* pawn, const Vec3& loc, float h, float reach, Vec3& normal, float& dist)
    {
        float best = 2.0f;
        for (int i = 0; i < 12; ++i)
        {
            const float a = static_cast<float>(i) * (3.14159265f / 6.0f);
            const Vec3 d{ std::cos(a), std::sin(a), 0 };
            for (const float zo : { 0.4f * h, -0.4f * h })
            {
                const Vec3 s{ loc.x, loc.y, loc.z + zo };
                const Vec3 e{ s.x + d.x * reach, s.y + d.y * reach, s.z };
                Vec3 n{};
                float t = 1;
                if (TraceWall(level, pawn, s, e, n, t) && t < best) { best = t; normal = n; }
            }
        }
        dist = best * reach;
        return best <= 1.0f;
    }

    // Direction held on the movement keys, in the world (false when none).
    bool InputDir(void* ctrl, Vec3& dir)
    {
        const float f = Fld<float>(ctrl, 0x3B0), s = Fld<float>(ctrl, 0x3B4);
        const float m = std::sqrt(f * f + s * s);
        if (m < 0.05f) return false;
        const float yaw = static_cast<float>(Fld<int>(ctrl, 0xF8) & 0xFFFF) * (3.14159265f / 32768.0f);
        const Vec3 fw{ std::cos(yaw), std::sin(yaw), 0 }, rt{ -fw.y, fw.x, 0 };
        dir = { (fw.x * f + rt.x * s) / m, (fw.y * f + rt.y * s) / m, 0 };
        return true;
    }

    int g_nameJumping = -1, g_namePlayAnim = -1, g_namePlayJump = -1, g_nameBounceAnim = -1;
    int g_nameFrontR = -1, g_nameFrontL = -1;
    int g_nameSplitState = -1, g_nameSplitLabel = -1;
    uint8_t g_prevJump = 0;
    DWORD g_jumpPressAt = 0;           // when the current jump press began
    bool g_jumpPressBounced = false;   // the current press kicked off a wall
    int g_bounces = 0, g_heldAnim = 0;
    DWORD g_heldUntil = 0;
    void* g_heldPawn = nullptr;

    constexpr int kLegRefChannel = 14;   // the kicking leg's reference pose, under the kick
    constexpr int kLegChannel = 15;      // the kick
    int g_legRefSeq = 0;                 // reference pose for the current kick (0 = none)
    float g_legRefFrame = 0;
    int g_goodRefSeq = 0;                // Sam's pose at the first bounce of a running jump (kept across jumps)
    float g_goodRefFrame = 0;
    bool g_legLookup = false;
    void* g_blendCode = nullptr;   // EPawn.BlendAnimOverCurrent
    void* g_alphaCode = nullptr;   // Actor.AnimBlendToAlpha
    int g_nameAlpha = 0, g_nameThighR = 0, g_nameThighL = 0;
    void* g_legPawn = nullptr;
    DWORD g_legStart = 0, g_legUntil = 0;
    float g_legFrame = 0, g_legStrength = 0.5f;

    // Steering after a bounce.
    void* g_steerPawn = nullptr;
    DWORD g_steerUntil = 0;
    Vec3 g_steerN{}, g_steerBase{}, g_steerDir{}, g_steerPrev{};
    bool g_steerHasDir = false;

    // State log after a bounce.
    void* g_trackPawn = nullptr;
    DWORD g_trackStart = 0, g_trackUntil = 0;
    void* g_trackNode = nullptr;
    void* g_trackBase = nullptr;
    int g_trackPhys = -1, g_trackLines = 0;

    float g_takeoffSpeed = 0;   // horizontal speed when Sam left the ground
    int g_prevPhys = -1;
    int g_rollPatch = 0, g_rollTries = 0;   // 1 patched, -1 gave up
    int g_nameLedge = -1, g_nameOverRailing = -1;
    DWORD g_climbSince = 0;
    bool g_climbTried = false;

    // Sets blend channel 'ch' to 'alpha': BlendAlpha +30h and its target +40h (AnimBlendToAlpha sets the target
    // and its time +3Ch; the engine moves BlendAlpha towards it), and when 'frame' >= 0 the animation's frame
    // (+0Ch, 0..1; rate +08h). Returns the alpha the game had, or -1 when there's no such channel.
    float HoldChannel(void* pawn, int ch, float alpha, float frame)
    {
        __try
        {
            void* inst = Fld<void*>(pawn, 0x64);
            if (!inst || Fld<int>(inst, 0x98) <= ch) return -1.0f;
            uint8_t* c = Fld<uint8_t*>(inst, 0x94) + ch * 0x80;
            const float had = *reinterpret_cast<float*>(c + 0x30);
            *reinterpret_cast<float*>(c + 0x30) = alpha;
            *reinterpret_cast<float*>(c + 0x40) = alpha;
            if (frame >= 0.0f) *reinterpret_cast<float*>(c + 0x0C) = frame;
            return had;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
    }

    // True when blend channel 'ch' of the pawn's skeletal mesh instance plays sequence 'name'.
    bool ChannelHas(void* pawn, int ch, int name)
    {
        __try
        {
            void* inst = Fld<void*>(pawn, 0x64);
            if (!inst || Fld<int>(inst, 0x98) <= ch) return false;
            return *reinterpret_cast<const int*>(Fld<uint8_t*>(inst, 0x94) + ch * 0x80) == name;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // The full-body animation Sam's legs follow now: the highest channel below ours with BlendAlpha >= 0.5 and no
    // bone mask (bone byte +46h = 0), else channel 0.
    bool BasePose(void* pawn, int& seq, float& frame)
    {
        __try
        {
            void* inst = Fld<void*>(pawn, 0x64);
            if (!inst) return false;
            const int num = std::min(Fld<int>(inst, 0x98), kLegRefChannel);
            const uint8_t* data = Fld<uint8_t*>(inst, 0x94);
            for (int ch = num - 1; ch >= 0; --ch)
            {
                const uint8_t* c = data + ch * 0x80;
                const int s = *reinterpret_cast<const int*>(c);
                if (s && (ch == 0 || (*reinterpret_cast<const float*>(c + 0x30) >= 0.5f && c[0x46] == 0)))
                {
                    seq = s;
                    frame = std::clamp(*reinterpret_cast<const float*>(c + 0x0C), 0.0f, 1.0f);
                    return true;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    bool PlayOnLeg(void* pawn, int anim, bool right, int channel)
    {
        if (!g_legLookup)
        {
            g_legLookup = true;
            g_blendCode = Script::Native(1538);
            g_nameAlpha = Script::Name(L"AnimBlendToAlpha");
            g_alphaCode = Script::NativeCode(pawn, g_nameAlpha);
            g_nameThighR = Script::Name(L"B R Thigh");
            g_nameThighL = Script::Name(L"B L Thigh");
            LOG("Movement: one-leg wall-bounce kick %s", g_blendCode && g_nameThighR && g_nameThighL ? "available"
                : "unavailable (whole-body animation instead)");
        }
        const int bone = right ? g_nameThighR : g_nameThighL;
        if (!g_blendCode || !bone || !anim) return false;
        Script::Args a;   // Sequence, BlendStrength, BoneName, Rate, TweenTime, Channel, bBackward, BlendTime, in_loop, pause
        a.Name(anim); a.Float(1.0f); a.Name(bone); a.Float(1.0f); a.Float(0.05f); a.Int(channel);
        a.Skip(); a.Float(0.05f); a.Skip(); a.Skip();
        return Script::CallNative(pawn, g_blendCode, a.End()) && ChannelHas(pawn, channel, anim);
    }
    bool PlayLegKick(void* pawn, int anim, bool right) { return PlayOnLeg(pawn, anim, right, kLegChannel); }

    void FadeChannel(void* pawn, int ch)
    {
        if (HoldChannel(pawn, ch, 0.0f, -1.0f) >= 0.0f) return;
        if (g_alphaCode)
        {
            Script::Args a;
            a.Int(ch); a.Float(0.0f); a.Float(0.25f);
            Script::CallNative(pawn, g_alphaCode, a.End());
        }
        else if (g_nameAlpha)
        {
            struct { int stage; float alpha, time; int spare[4]; } parms{ ch, 0.0f, 0.25f, {} };
            Script::Call(pawn, g_nameAlpha, &parms);
        }
    }
    void FadeLegKick(void* pawn)
    {
        FadeChannel(pawn, kLegChannel);
        if (g_legRefSeq) FadeChannel(pawn, kLegRefChannel);
        g_legRefSeq = 0;
    }

    // Horizontal velocity for a bounce off a wall with normal n: the held direction (if any, and not into the
    // wall) at WallBounceBoost, else 'base'.
    bool SteerVelocity(const Vec3& n, const Vec3& dir, const Vec3& before, Vec3& out)
    {
        const float boost = g_settings.wallBounceBoost;
        if (boost <= 0.0f || Dot2(dir, n) < -0.3f) return false;
        const float speed = std::max(boost, Dot2(before, dir));
        out = { dir.x * speed, dir.y * speed, 0 };
        const float away = Dot2(out, n), minAway = g_settings.wallBounceOut * 0.5f;
        if (away < minAway) { out.x += n.x * (minAway - away); out.y += n.y * (minAway - away); }
        return true;
    }

    // Box sweep through the world (same flags as the walls); true on a hit.
    bool Sweep(void* level, void* source, const Vec3& start, const Vec3& end, const Vec3& ext, Vec3* hitLoc, Vec3* normal)
    {
        alignas(8) uint8_t buf[0x40] = {};
        *reinterpret_cast<float*>(buf + 0x24) = 1.0f;
        *reinterpret_cast<int*>(buf + 0x28) = -1;
        __try
        {
            const auto slc = reinterpret_cast<SingleLineCheck_t>((*reinterpret_cast<void***>(level))[0xDC / 4]);
            slc(level, nullptr, buf, source, &end, &start, 0x17, ext.x, ext.y, ext.z, 0, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
        if (!*reinterpret_cast<void**>(buf + 4)) return false;
        if (hitLoc) *hitLoc = *reinterpret_cast<Vec3*>(buf + 8);
        if (normal) *normal = *reinterpret_cast<Vec3*>(buf + 0x14);
        return true;
    }

    // ShouldRoll's -600 -> -1e9 (see the header).
    int PatchShouldRoll(void* ctrl)
    {
        static const uint8_t k600[5] = { 0x1E, 0x00, 0x00, 0x16, 0xC4 }, k400[5] = { 0x1E, 0x00, 0x00, 0xC8, 0xC3 };
        const int name = Script::Name(L"ShouldRoll");
        void* fn = name ? Script::Function(ctrl, name) : nullptr;
        if (!fn) return 0;
        __try
        {
            for (int off = 0x28; off <= 0x90; off += 4)
            {
                uint8_t* data = Fld<uint8_t*>(fn, off);
                const int num = Fld<int>(fn, off + 4), max = Fld<int>(fn, off + 8);
                if (!data || num < 16 || num > 4096 || max < num) continue;
                int at600 = -1;
                bool has400 = false;
                for (int i = 0; i + 5 <= num; ++i)
                {
                    if (!memcmp(data + i, k600, 5)) at600 = i;
                    if (!memcmp(data + i, k400, 5)) has400 = true;
                }
                if (at600 >= 0 && has400)
                {
                    const float v = -1.0e9f;
                    memcpy(data + at600 + 1, &v, 4);
                    return 1;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    // Hanging from a ledge the game won't let Sam climb: find a floor over the edge (see the header).
    bool FindClimbSpot(void* pawn, Vec3& start, Vec3& end)
    {
        void* level = Fld<void*>(pawn, 0x7C);
        if (!level) return false;
        const Vec3 loc = Fld<Vec3>(pawn, 0xE8), arms = Fld<Vec3>(pawn, 0x550);
        const float rad = std::clamp(Fld<float>(pawn, 0x220), 10.0f, 60.0f);
        const float ch = std::clamp(Fld<float>(pawn, 0x320), 20.0f, 80.0f);
        const float yaw = static_cast<float>(Fld<int>(pawn, 0xF8) & 0xFFFF) * (3.14159265f / 32768.0f);
        const Vec3 f{ std::cos(yaw), std::sin(yaw), 0 };
        const float hands = loc.z + arms.z;
        const Vec3 ext{ rad, rad, ch };
        start = { loc.x - f.x * 4.0f, loc.y - f.y * 4.0f, hands + ch + 12.0f };
        // room above the hands, where he lifts himself to
        if (Sweep(level, pawn, { loc.x - f.x * 4.0f, loc.y - f.y * 4.0f, start.z - 1.0f }, start, ext, nullptr, nullptr)) return false;
        for (const float d : { 0.0f, 20.0f, 40.0f })
        {
            const float reach = std::max(arms.x, 0.0f) + rad + 30.0f + d;
            const Vec3 over{ start.x + f.x * reach, start.y + f.y * reach, start.z };
            if (Sweep(level, pawn, start, over, ext, nullptr, nullptr)) continue;   // something in the way
            Vec3 hit{}, n{};
            if (!Sweep(level, pawn, over, { over.x, over.y, over.z - (ch + 260.0f) }, ext, &hit, &n) || n.z < 0.7f) continue;
            end = { hit.x, hit.y, hit.z + 2.0f };
            return true;
        }
        return false;
    }

    void ClimbCheck(void* pawn, void* ctrl, DWORD now)
    {
        if (g_nameLedge < 0) { g_nameLedge = Script::Name(L"s_Ledge"); g_nameOverRailing = Script::Name(L"OverRailing"); }
        const bool hanging = g_nameLedge && Script::StateName(ctrl) == g_nameLedge && !(Fld<uint32_t>(ctrl, 0x550) & 1);
        const bool up = Fld<float>(ctrl, 0x3B0) > 0.25f || Fld<uint8_t>(ctrl, 0x5FE);
        if (!hanging || !up) { g_climbSince = 0; g_climbTried = false; return; }
        if (!g_climbSince) { g_climbSince = now; return; }
        if (g_climbTried || now - g_climbSince < 150 || !g_nameOverRailing) return;
        g_climbTried = true;   // once per press
        Vec3 start{}, end{};
        if (!FindClimbSpot(pawn, start, end))
        {
            LOG("[MOVE] climb: no room over this ledge");
            return;
        }
        const Vec3 loc = Fld<Vec3>(pawn, 0xE8);
        bool ok = false;
        __try
        {
            Fld<Vec3>(pawn, 0x35C) = start;
            Fld<Vec3>(pawn, 0x368) = end;
            void** vt = *reinterpret_cast<void***>(ctrl);
            reinterpret_cast<GotoLabel_t>(vt[0x34 / 4])(ctrl, nullptr, g_nameOverRailing, 0, 0);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        LOG("[MOVE] climb over the ledge%s: from (%.0f %.0f %.0f) to (%.0f %.0f %.0f)", ok ? "" : " FAILED",
            loc.x, loc.y, loc.z, end.x, end.y, end.z);
    }

    // Name after the last '.' of an object's full name ("State Echelon.EPlayerController.s_Fence" -> "s_Fence").
    void ShortName(void* obj, wchar_t* out, size_t len)
    {
        out[0] = 0;
        if (!obj || !Game::GetFullName) { wcscpy_s(out, len, L"none"); return; }
        wchar_t buf[512];
        buf[0] = 0;
        __try { Game::GetFullName(obj, nullptr, buf); }
        __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = 0; }
        buf[511] = 0;
        const wchar_t* dot = wcsrchr(buf, L'.');
        wcsncpy_s(out, len, dot ? dot + 1 : buf, _TRUNCATE);
    }

    void* StateNode(void* ctrl)
    {
        __try
        {
            void* frame = Fld<void*>(ctrl, 0xC);
            return frame ? Fld<void*>(frame, 0x1C) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // The game's split-zone check, run as if Sam stood at p (Location +E8h is put back right after; the check
    // also stores that spot in m_locationStart/End +35Ch/+368h, which are put back too, or the split's own code
    // would first move him there).
    bool SplitCheckAt(void* pawn, void* ctrl, const Vec3& p)
    {
        if (!g_splitCheck) return false;
        Vec3& loc = Fld<Vec3>(pawn, 0xE8);
        const Vec3 real = loc, s0 = Fld<Vec3>(pawn, 0x35C), s1 = Fld<Vec3>(pawn, 0x368);
        loc = p;
        int ok = 0;
        __try { ok = g_splitCheck(ctrl, nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        loc = real;
        Fld<Vec3>(pawn, 0x35C) = s0;
        Fld<Vec3>(pawn, 0x368) = s1;
        return ok != 0;
    }

    // Is Sam above a split-jump spot? The check is made at standing height above the floor straight below him
    // (where a split from the ground starts; returned in startZ), at his spot and up to 50 units either way
    // across the corridor (along the bounced wall's normal n): the spot is a narrow band. 'spot' gets the place
    // that passed.
    bool SplitSpotBelow(void* pawn, void* ctrl, const Vec3& n, float& startZ, bool& floorFound, Vec3& spot)
    {
        floorFound = false;
        void* level = Fld<void*>(pawn, 0x7C);
        if (!level) return false;
        const Vec3 loc = Fld<Vec3>(pawn, 0xE8);
        Vec3 hit{}, hn{};
        if (!Sweep(level, pawn, loc, { loc.x, loc.y, loc.z - 3000.0f }, { 0, 0, 0 }, &hit, &hn) || hn.z < 0.7f) return false;
        floorFound = true;
        startZ = hit.z + std::clamp(Fld<float>(pawn, 0x328), 40.0f, 120.0f);   // StandHeight
        for (const float k : { 0.0f, 25.0f, -25.0f, 50.0f, -50.0f })
        {
            const Vec3 p{ loc.x + n.x * k, loc.y + n.y * k, startZ };
            if (SplitCheckAt(pawn, ctrl, p)) { spot = p; return true; }
        }
        return false;
    }

    constexpr float kSplitRise = 212.0f;   // the split's intro lifts Sam this far from where it starts (measured)
    int g_nameInEEV = -1;

    // ULevel::FarMoveActor (vtable +88h); bTest only checks whether the actor would fit there.
    bool SafeFarMove(void* level, void* actor, const Vec3& p, bool test = false)
    {
        using FarMoveActor_t = int(__fastcall*)(void* level, void* edx, void* actor, float x, float y, float z,
                                                int bTest, int bNoCheck, int bAttachedMove);
        __try
        {
            const auto f = reinterpret_cast<FarMoveActor_t>((*reinterpret_cast<void***>(level))[0x88 / 4]);
            return f(level, nullptr, actor, p.x, p.y, p.z, test ? 1 : 0, 0, 0) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Gliding into the split pose (see EnterSplitDirect).
    void* g_glidePawn = nullptr;
    void* g_glideCtrl = nullptr;
    Vec3 g_glideFrom{}, g_glideTo{};
    int g_glideYawFrom = 0, g_glideYawTo = 0;
    DWORD g_glideStart = 0;
    constexpr DWORD kGlideMs = 300;
    int g_namePlaySplit = -1;

    bool GotoStateLabel(void* ctrl, int state, int label)
    {
        __try
        {
            void** vt = *reinterpret_cast<void***>(ctrl);
            if (reinterpret_cast<GotoState_t>(vt[0x30 / 4])(ctrl, nullptr, state, 0) != 1) return false;
            reinterpret_cast<GotoLabel_t>(vt[0x34 / 4])(ctrl, nullptr, label, 0, 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Straight into the split pose, skipping its intro: what the intro ends with is done directly. Sam is put
    // kSplitRise above the split's start height, centred between the two walls across the corridor (traced along
    // n), facing along it (the way nearer his facing), with the split's collision (SetCollisionSize native 283:
    // CollisionRadius, StandHeight x 0.35) and physics (native 3970 = SetPhysics, 8), and the controller goes to
    // s_Split at its other label, 'InEEV' (split camera, then nothing): the state's PlayerMove then plays the split
    // hold pose (PlaySplit ASplitWait), and getting out works as usual (its EndState restores the collision).
    bool EnterSplitDirect(void* pawn, void* ctrl, const Vec3& spot, float startZ, const Vec3& n, Vec3& placed)
    {
        if (g_nameSplitState < 0) { g_nameSplitState = Script::Name(L"s_Split"); g_nameSplitLabel = Script::Name(L"Split"); }
        if (g_nameInEEV < 0) g_nameInEEV = Script::Name(L"InEEV");
        void* setPhysics = Script::Native(3970);
        void* setCollision = Script::Native(283);
        void* level = Fld<void*>(pawn, 0x7C);
        if (!g_nameSplitState || !g_nameInEEV || !setPhysics || !setCollision || !level) return false;
        if (Fld<uint32_t>(pawn, 0x2B8) & 1) return false;   // crouched
        Vec3 q{ spot.x, spot.y, startZ + kSplitRise };
        Vec3 hit{}, hn{};
        float d1 = -1.0f, d2 = -1.0f;
        if (Sweep(level, pawn, q, { q.x + n.x * 250.0f, q.y + n.y * 250.0f, q.z }, { 0, 0, 0 }, &hit, &hn))
            d1 = std::sqrt((hit.x - q.x) * (hit.x - q.x) + (hit.y - q.y) * (hit.y - q.y));
        if (Sweep(level, pawn, q, { q.x - n.x * 250.0f, q.y - n.y * 250.0f, q.z }, { 0, 0, 0 }, &hit, &hn))
            d2 = std::sqrt((hit.x - q.x) * (hit.x - q.x) + (hit.y - q.y) * (hit.y - q.y));
        if (d1 >= 0.0f && d2 >= 0.0f) { const float s = (d1 - d2) * 0.5f; q.x += n.x * s; q.y += n.y * s; }
        const float radius = Fld<float>(pawn, 0x220), stand = Fld<float>(pawn, 0x328);
        {
            Script::Args a;
            a.Float(radius); a.Float(stand * 0.35f);
            Script::CallNative(pawn, setCollision, a.End());
        }
        if (!SafeFarMove(level, pawn, q, true))   // would he fit there?
        {
            Script::Args a;
            a.Float(radius); a.Float(stand);
            Script::CallNative(pawn, setCollision, a.End());
            return false;
        }
        {
            Script::Args a;
            a.Byte(8);
            Script::CallNative(pawn, setPhysics, a.End());
        }
        Fld<Vec3>(pawn, 0x100) = { 0, 0, 0 };
        Fld<Vec3>(pawn, 0x10C) = { 0, 0, 0 };
        const int yawNow = Fld<int>(pawn, 0xF8) & 0xFFFF;
        const float yaw = static_cast<float>(yawNow) * (3.14159265f / 32768.0f);
        Vec3 t{ -n.y, n.x, 0 };
        if (t.x * std::cos(yaw) + t.y * std::sin(yaw) < 0.0f) t = { -t.x, -t.y, 0 };
        Fld<uint32_t>(ctrl, 0x554) &= ~0x10u;   // m_CrStart
        if (!GotoStateLabel(ctrl, g_nameSplitState, g_nameInEEV)) return false;
        // Glide there over kGlideMs (moved every tick, see SplitGlideTick), held in transition so the state's
        // PlayerMove doesn't restart the pose or let him drop out half way; the pose blends in over the same time.
        Fld<uint32_t>(ctrl, 0x550) |= 1u;       // bInTransition
        g_glidePawn = pawn;
        g_glideCtrl = ctrl;
        g_glideFrom = Fld<Vec3>(pawn, 0xE8);
        g_glideTo = q;
        g_glideYawFrom = yawNow;
        g_glideYawTo = static_cast<int>(std::atan2(t.y, t.x) * (32768.0f / 3.14159265f)) & 0xFFFF;
        g_glideStart = GetTickCount();
        if (g_namePlaySplit < 0) g_namePlaySplit = Script::Name(L"PlaySplit");
        if (g_namePlaySplit)
        {
            // PlaySplit(largeAnim, narrowAnim, bLoop, TweenTime): ASplitWait +12C0h, ASplitWaitSm +12C4h
            struct { int large, narrow, loop; float tween; int spare[4]; } parms{ Fld<int>(pawn, 0x12C0), Fld<int>(pawn, 0x12C4), 1,
                                                                                  kGlideMs / 1000.0f, {} };
            Script::Call(ctrl, g_namePlaySplit, &parms);
        }
        placed = q;
        LOG("[MOVE] split: gliding to (%.0f %.0f %.0f), walls %.0f / %.0f away across the corridor", q.x, q.y, q.z, d1, d2);
        return true;
    }

    // Into the split jump (the split-zone check must have passed just before), as native 1187 does from the ground.
    bool EnterSplit(void* ctrl, void* pawn)
    {
        if (g_nameSplitState < 0) { g_nameSplitState = Script::Name(L"s_Split"); g_nameSplitLabel = Script::Name(L"Split"); }
        if (!g_nameSplitState || !g_nameSplitLabel) return false;
        __try
        {
            if (Fld<uint32_t>(pawn, 0x2B8) & 1) return false;   // crouched (the ground version uncrouches first)
            Fld<uint32_t>(ctrl, 0x554) &= ~0x10u;              // m_CrStart: not a crouched start
            void** vt = *reinterpret_cast<void***>(ctrl);
            if (reinterpret_cast<GotoState_t>(vt[0x30 / 4])(ctrl, nullptr, g_nameSplitState, 0) != 1) return false;
            reinterpret_cast<GotoLabel_t>(vt[0x34 / 4])(ctrl, nullptr, g_nameSplitLabel, 0, 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool g_splitMagnet = false;
    int g_splitProbes = 0, g_splitProbeFloors = 0;
    Vec3 g_splitSpot{}, g_splitN{};
    float g_splitStartZ = 0;
    constexpr float kNoSplit = -1.0e9f;
    float g_lastSplitZ = kNoSplit;   // height of the split Sam last left; spots that high or higher are ignored until he lands
    Vec3 g_fallN{};
    int g_fallNTicks = 0;

    void TrackAfterBounce(void* pawn, void* ctrl, DWORD now)
    {
        if (pawn != g_trackPawn) return;
        if (now >= g_trackUntil || g_trackLines >= 16) { g_trackPawn = nullptr; return; }
        void* node = StateNode(ctrl);
        const int phys = Fld<uint8_t>(pawn, 0x4C);
        void* base = Fld<void*>(pawn, 0xA8);
        if (node == g_trackNode && phys == g_trackPhys && base == g_trackBase) return;
        g_trackNode = node; g_trackPhys = phys; g_trackBase = base;
        ++g_trackLines;
        wchar_t st[96], bs[96];
        ShortName(node, st, 96);
        ShortName(base, bs, 96);
        const Vec3 loc = Fld<Vec3>(pawn, 0xE8), vel = Fld<Vec3>(pawn, 0x100);
        LOG("[MOVE] after bounce +%.2f s: state %ls, physics %d, on %ls, at (%.0f %.0f %.0f), speed (%.0f %.0f %.0f)",
            static_cast<float>(now - g_trackStart) / 1000.0f, st, phys, bs, loc.x, loc.y, loc.z, vel.x, vel.y, vel.z);
    }
}

// ---- Crouch noise ([Gameplay] CrouchNoiseFix, [Debug] LogCrouch) ------------------------------------------
// Footstep AI noise comes from the walk animations: their notifies call EPawn.PlayLeft/RightFootStep ->
// ProcessLeft/RightFootStep, which calls native 1699 (0x10B81170: LastAction +D4Ah = !bIsCrouched (+2B8h bit 1),
// | 2 when SoundWalkingRatio +308h is over the run threshold) and then GenerateAINoise(Sounds_Move, LastAction,
// LastMaterial) -> MakeNoise type 89, louder for standing (1, 3) than crouched (0, 2). Steps that never play
// make no noise, which is how spamming crouch made Sam silent in the older games. So, with CrouchNoiseFix:
// during a crouch transition (crouch toggled in the last 0.6 s, or bIsCrouched != bWantsToCrouch +2B8h bit 0)
// a step counts as standing (LastAction |= 1 after the native), and while crouch was toggled in the last 1.5 s
// a step that doesn't come is made (QuietStandingSteps instead clears that bit on every step: standing steps
// are as quiet as crouched ones at the same speed, so the stance stops mattering for noise): when Sam walks (s_PlayerWalking, PHYS_Walking) 1.8 strides (learned from
// his real steps) without one, ProcessLeft/RightFootStep is called for him. LogCrouch logs Sam's animation
// channels (MeshInstance +64h -> Blends +94h/+98h: sequence +00h, frame +0Ch, alpha +30h) and the crouch fade
// (EPawn.mCrouchToStandFade +698h) every 0.1 s for 2 s after each crouch toggle ([CROUCH]).
// Instant crouch switch ([Gameplay] CrouchInstantSwitch): a stance change copies blend channels 0/1 (the stance's
// walk/wait animation: stand walk +F60h, stand wait +F34h, crouch walk +F74h, crouch wait +F4Ch) to 2/3 and fades
// those out over 0.3 s while the new stance plays on 0/1. Sam's idle code (0x10B88870) then won't switch again
// until channel 2 has faded below ~0.05, so a quick second press waits up to 0.3 s (or is skipped): the stagger.
// When channel 0 holds the other stance's animation while channel 2 still holds this stance's (the pose being
// faded out), the channel pairs 0<->2, 1<->3 and (when moving) the jog layer 4<->5 (speed-blended, it covers the
// walk layer when running) are swapped (everything but the blend fields +30h..+43h and the bone bytes +45h..+47h),
// and the fading layers get their layer's alpha minus what they had, fading to 0: the transition reverses from
// where it is. Channels 0-5 are also kept from tick to tick (this runs right after the pawn's tick, before it is
// drawn): when the game switches a layer to the other stance itself without a proper crossfade (the jog layer
// sometimes switches outright; a reversal while moving restarts the jog fade at full strength), the old pose is
// put back on the fading layer at the strength it still had and faded out.
// Like every stance switch of the game's, it first calls EPawn 0x10B80220(0.3) (thiscall, float): it stamps the
// time (+7B4h, from Level +C4h) and fades out channel 9, the "relaxed" idle pose (WalkStRxFd0 / WalkCrRxFd0) that
// comes in at full strength after a few seconds of standing still and would otherwise snap between stances.
namespace
{
    void* g_crouchPawn = nullptr;
    DWORD g_lastCrouchToggle = 0, g_crouchLogUntil = 0, g_crouchLogNext = 0, g_crouchLogStart = 0;
    int g_prevWants = -1;
    Vec3 g_prevPos{};
    float g_distSinceStep = 0, g_stride = 90.0f;
    bool g_stepsSeen = false, g_nextStepLeft = true;
    int g_madeSteps = 0, g_louderSteps = 0, g_loggedLouder = 0;
    int g_nameWalking = -1, g_nameStepL = -1, g_nameStepR = -1;

    bool InCrouchTransition(void* pawn, DWORD now)
    {
        const uint32_t f = Fld<uint32_t>(pawn, 0x2B8);
        return (g_lastCrouchToggle && now - g_lastCrouchToggle < 600) || (((f >> 1) & 1) != (f & 1));
    }

    struct ChanInfo { int ch, seq; float alpha, frame; };

    int ReadChannels(void* pawn, ChanInfo* out, int max)
    {
        int n = 0;
        __try
        {
            void* inst = Fld<void*>(pawn, 0x64);
            if (!inst) return 0;
            const int num = std::min(Fld<int>(inst, 0x98), 24);
            const uint8_t* data = Fld<uint8_t*>(inst, 0x94);
            for (int ch = 0; ch < num && n < max; ++ch)
            {
                const uint8_t* c = data + ch * 0x80;
                const int seq = *reinterpret_cast<const int*>(c);
                const float alpha = *reinterpret_cast<const float*>(c + 0x30);
                if (ch == 0 || (seq && alpha > 0.01f))
                    out[n++] = { ch, seq, alpha, *reinterpret_cast<const float*>(c + 0x0C) };
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return n;
    }

    void LogChannels(void* pawn, DWORD now)
    {
        ChanInfo ch[24];
        const int n = ReadChannels(pawn, ch, 24);
        const uint32_t f = Fld<uint32_t>(pawn, 0x2B8);
        const Vec3 v = Fld<Vec3>(pawn, 0x100);
        char line[1600];
        int o = sprintf_s(line, "[CROUCH] +%.1f s: wants %u crouched %u, speed %.0f, action %u, fade %.2f |",
                          static_cast<float>(now - g_crouchLogStart) / 1000.0f, f & 1, (f >> 1) & 1,
                          std::sqrt(v.x * v.x + v.y * v.y), static_cast<unsigned>(Fld<uint8_t>(pawn, 0xD4A)), Fld<float>(pawn, 0x698));
        for (int i = 0; i < n && o > 0 && o < static_cast<int>(sizeof(line)) - 80; ++i)
        {
            wchar_t name[48];
            if (!Script::NameText(ch[i].seq, name, 48)) swprintf_s(name, L"#%d", ch[i].seq);
            const int w = sprintf_s(line + o, sizeof(line) - o, " ch%d %ls a%.2f f%.2f", ch[i].ch, name, ch[i].alpha, ch[i].frame);
            if (w <= 0) break;
            o += w;
        }
        LOG("%s", line);
    }

    // Native 1699 (EPawn exec, P_FINISH only): sets LastAction for the step being made.
    void __fastcall hkUpdateLastAction(void* pawn, void* edx, void* stack, void* result)
    {
        oUpdateLastAction(pawn, edx, stack, result);
        if (!pawn || pawn != g_crouchPawn) return;
        const DWORD now = GetTickCount();
        __try
        {
            const bool transition = InCrouchTransition(pawn, now);
            if (!transition && g_stepsSeen && g_distSinceStep > 20.0f && g_distSinceStep < 400.0f)
                g_stride = g_stride * 0.8f + g_distSinceStep * 0.2f;
            g_stepsSeen = true;
            g_distSinceStep = 0;
            g_nextStepLeft = !g_nextStepLeft;
            uint8_t& act = Fld<uint8_t>(pawn, 0xD4A);
            if (g_settings.quietStandingSteps)
                act &= ~1;   // stance makes no difference: every step as loud as a crouched one at that speed
            else if (g_settings.crouchNoiseFix && transition && !(act & 1))
            {
                act |= 1;
                ++g_louderSteps;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    int g_swaps = 0;
    bool g_loggedStanceAnims = false;

    // Swaps the animation part of channels a and b and sets b's alpha (and its target) to 'alphaB'.
    bool SwapChannels(void* pawn, int a, int b, float alphaB)
    {
        __try
        {
            void* inst = Fld<void*>(pawn, 0x64);
            if (!inst || Fld<int>(inst, 0x98) <= std::max(a, b)) return false;
            uint8_t* ca = Fld<uint8_t*>(inst, 0x94) + a * 0x80;
            uint8_t* cb = Fld<uint8_t*>(inst, 0x94) + b * 0x80;
            uint8_t tmp[0x80];
            memcpy(tmp, ca, 0x80);
            for (int i = 0; i < 0x80; ++i)
            {
                if ((i >= 0x30 && i < 0x44) || (i >= 0x45 && i < 0x48)) continue;
                ca[i] = cb[i];
                cb[i] = tmp[i];
            }
            *reinterpret_cast<float*>(cb + 0x30) = alphaB;
            *reinterpret_cast<float*>(cb + 0x40) = alphaB;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void StopRelaxedPose(void* pawn)
    {
        if (!g_stopRelaxed) return;
        __try { g_stopRelaxed(pawn, nullptr, 0.3f); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void BlendOut(void* pawn, int ch, float time)
    {
        if (!g_alphaCode) return;
        Script::Args a;
        a.Int(ch); a.Float(0.0f); a.Float(time);
        Script::CallNative(pawn, g_alphaCode, a.End());
    }

    // 1 crouched, 0 standing, -1 not a stance animation (walk +F60h/+F74h, wait +F34h/+F4Ch, jog +F84h/+F98h).
    int StanceOf(void* pawn, int seq)
    {
        if (!seq) return -1;
        if (seq == Fld<int>(pawn, 0xF74) || seq == Fld<int>(pawn, 0xF4C) || seq == Fld<int>(pawn, 0xF98)) return 1;
        if (seq == Fld<int>(pawn, 0xF60) || seq == Fld<int>(pawn, 0xF34) || seq == Fld<int>(pawn, 0xF84)) return 0;
        return -1;
    }

    constexpr int kChanSize = 0x80;
    uint8_t g_snap[6][kChanSize];   // channels 0-5 as they were after the previous tick
    void* g_snapPawn = nullptr;
    bool g_snapValid = false;
    int g_fixedSwitches = 0;

    bool ReadChannels6(void* pawn, uint8_t (*out)[kChanSize])
    {
        __try
        {
            void* inst = Fld<void*>(pawn, 0x64);
            if (!inst || Fld<int>(inst, 0x98) < 6) return false;
            memcpy(out, Fld<uint8_t*>(inst, 0x94), 6 * kChanSize);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Writes the animation part of 'src' into channel 'ch' (keeping its blend fields +30h..+43h and bone bytes
    // +45h..+47h) and sets its alpha and target to 'alpha'.
    bool WriteAnim(void* pawn, int ch, const uint8_t* src, float alpha)
    {
        __try
        {
            void* inst = Fld<void*>(pawn, 0x64);
            if (!inst || Fld<int>(inst, 0x98) <= ch) return false;
            uint8_t* c = Fld<uint8_t*>(inst, 0x94) + ch * kChanSize;
            for (int i = 0; i < kChanSize; ++i)
                if (!((i >= 0x30 && i < 0x44) || (i >= 0x45 && i < 0x48))) c[i] = src[i];
            *reinterpret_cast<float*>(c + 0x30) = alpha;
            *reinterpret_cast<float*>(c + 0x40) = alpha;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    int SeqOf(const uint8_t* c) { return *reinterpret_cast<const int*>(c); }
    float AlphaOf(const uint8_t* c) { return *reinterpret_cast<const float*>(c + 0x30); }

    // Each stance layer and the layer its old pose fades out on: walk 0->2, upper body 1->3, jog (speed-blended) 4->5.
    constexpr int kPairs[3][2] = { { 0, 2 }, { 1, 3 }, { 4, 5 } };

    // See "Instant crouch switch" in the header.
    void InstantCrouchSwitch(void* pawn, void* ctrl, bool walking)
    {
        if (!walking || (Fld<uint32_t>(ctrl, 0x550) & 1)) { g_snapValid = false; return; }
        if (!g_legLookup)
        {
            g_legLookup = true;
            g_blendCode = Script::Native(1538);
            g_nameAlpha = Script::Name(L"AnimBlendToAlpha");
            g_alphaCode = Script::NativeCode(pawn, g_nameAlpha);
            g_nameThighR = Script::Name(L"B R Thigh");
            g_nameThighL = Script::Name(L"B L Thigh");
        }
        if (!g_loggedStanceAnims)
        {
            g_loggedStanceAnims = true;
            wchar_t a[48], b[48], c[48], d[48], e[48], f[48];
            Script::NameText(Fld<int>(pawn, 0xF60), a, 48); Script::NameText(Fld<int>(pawn, 0xF34), b, 48);
            Script::NameText(Fld<int>(pawn, 0xF84), c, 48); Script::NameText(Fld<int>(pawn, 0xF74), d, 48);
            Script::NameText(Fld<int>(pawn, 0xF4C), e, 48); Script::NameText(Fld<int>(pawn, 0xF98), f, 48);
            LOG("[CROUCH] stance animations: standing walk %ls, wait %ls, jog %ls; crouched walk %ls, wait %ls, jog %ls", a, b, c, d, e, f);
        }
        if (pawn != g_snapPawn) { g_snapPawn = pawn; g_snapValid = false; }
        uint8_t cur[6][kChanSize];
        if (!ReadChannels6(pawn, cur)) { g_snapValid = false; return; }
        const int crouched = static_cast<int>((Fld<uint32_t>(pawn, 0x2B8) >> 1) & 1);
        bool changed = false;

        // 1) The game switched a layer to the other stance during its update this tick. Its old pose has to fade
        //    out from where it was: a switch without a crossfade (seen on the jog layer), or a fresh full-strength
        //    crossfade over one that was half done, would snap.
        if (g_snapValid)
        {
            int fixedLayers = 0;
            for (const auto& p : kPairs)
            {
                const uint8_t* was = g_snap[p[0]];
                const uint8_t* wasFade = g_snap[p[1]];
                const int sOld = StanceOf(pawn, SeqOf(was)), sNew = StanceOf(pawn, SeqOf(cur[p[0]]));
                if (sOld < 0 || sNew < 0 || sOld == sNew) continue;
                const float layer = AlphaOf(was);
                // how much of the new pose was already showing through the fading layer
                const float shown = (SeqOf(wasFade) == SeqOf(cur[p[0]]) && layer > 0.001f)
                                        ? std::clamp(AlphaOf(wasFade) / layer, 0.0f, 1.0f) : 0.0f;
                const float want = std::max(0.0f, AlphaOf(cur[p[0]]) * (1.0f - shown));
                if (SeqOf(cur[p[1]]) == SeqOf(was) && std::fabs(AlphaOf(cur[p[1]]) - want) < 0.05f) continue;   // fine already
                if (WriteAnim(pawn, p[1], was, want))
                {
                    BlendOut(pawn, p[1], std::max(0.05f, 0.3f * (1.0f - shown)));
                    ++fixedLayers;
                }
            }
            if (fixedLayers)
            {
                changed = true;
                ++g_fixedSwitches;
                if (g_fixedSwitches <= 5 || g_fixedSwitches % 25 == 0)
                    LOG("[CROUCH] smoothed a stance switch the game made (%d layer%s, %d so far)", fixedLayers, fixedLayers == 1 ? "" : "s",
                        g_fixedSwitches);
            }
        }

        // 2) The walk layer still shows the other stance while its fading layer holds this one (the idle code
        //    waits for the fade to finish): swap the pairs so the transition reverses from where it is.
        if (!changed && StanceOf(pawn, SeqOf(cur[0])) == 1 - crouched && StanceOf(pawn, SeqOf(cur[2])) == crouched)
        {
            const bool jogMoving = AlphaOf(cur[4]) > 0.05f && StanceOf(pawn, SeqOf(cur[4])) == 1 - crouched;
            const bool jogReady = StanceOf(pawn, SeqOf(cur[5])) == crouched;
            if (!jogMoving || jogReady)   // otherwise the game's own moving switch handles it (and step 1 smooths it)
            {
                const float a2 = AlphaOf(cur[2]);
                const float back = std::clamp(1.0f - a2, 0.0f, 1.0f);
                StopRelaxedPose(pawn);
                if (SwapChannels(pawn, 0, 2, back))
                {
                    SwapChannels(pawn, 1, 3, std::max(0.0f, AlphaOf(cur[1]) - AlphaOf(cur[3])));
                    if (jogMoving && jogReady) SwapChannels(pawn, 4, 5, std::max(0.0f, AlphaOf(cur[4]) - AlphaOf(cur[5])));
                    const float t = std::max(0.05f, 0.3f * back);
                    BlendOut(pawn, 2, t);
                    BlendOut(pawn, 3, t);
                    if (jogMoving && jogReady) BlendOut(pawn, 5, t);
                    changed = true;
                    ++g_swaps;
                    if (g_swaps <= 5 || g_swaps % 25 == 0)
                        LOG("[CROUCH] switched to %s at once, reversing the fade from %.2f%s (%d so far)", crouched ? "crouched" : "standing",
                            a2, jogMoving ? ", jog layer too" : "", g_swaps);
                }
            }
        }

        // 3) Remember the layers for the next tick.
        if (changed) g_snapValid = ReadChannels6(pawn, g_snap);
        else { memcpy(g_snap, cur, sizeof(cur)); g_snapValid = true; }
    }

    void CrouchTick(void* pawn, void* ctrl, DWORD now, int phys)
    {
        g_crouchPawn = pawn;
        const int wants = static_cast<int>(Fld<uint32_t>(pawn, 0x2B8) & 1);
        if (g_prevWants >= 0 && wants != g_prevWants)
        {
            g_lastCrouchToggle = now;
            if (g_settings.logCrouch)
            {
                if (now >= g_crouchLogUntil) g_crouchLogStart = now;   // a new burst
                g_crouchLogUntil = now + 2000;
            }
        }
        g_prevWants = wants;
        if (g_louderSteps > g_loggedLouder && (g_loggedLouder < 5 || g_louderSteps % 20 == 0))
        {
            g_loggedLouder = g_louderSteps;
            LOG("[CROUCH] footstep during a crouch transition counted as standing (%d so far)", g_louderSteps);
        }

        // distance walked since the last step
        const Vec3 loc = Fld<Vec3>(pawn, 0xE8);
        const float dx = loc.x - g_prevPos.x, dy = loc.y - g_prevPos.y, d = std::sqrt(dx * dx + dy * dy);
        g_prevPos = loc;
        if (g_nameWalking < 0)
        {
            g_nameWalking = Script::Name(L"s_PlayerWalking");
            g_nameStepL = Script::Name(L"ProcessLeftFootStep");
            g_nameStepR = Script::Name(L"ProcessRightFootStep");
        }
        const bool walking = phys == 1 && g_nameWalking && Script::StateName(ctrl) == g_nameWalking;
        if (!walking || d > 200.0f) g_distSinceStep = 0;   // not walking, or moved by the game
        else g_distSinceStep += d;
        if (g_settings.crouchInstantSwitch) InstantCrouchSwitch(pawn, ctrl, walking);

        if (g_settings.crouchNoiseFix && walking && g_lastCrouchToggle && now - g_lastCrouchToggle < 1500 &&
            g_distSinceStep > std::max(120.0f, g_stride * 1.8f))
        {
            const float missed = g_distSinceStep;
            const int fn = g_nextStepLeft ? g_nameStepL : g_nameStepR;
            int spare[8] = {};
            if (fn && Script::Call(pawn, fn, spare))
            {
                ++g_madeSteps;
                if (g_madeSteps <= 5 || g_madeSteps % 20 == 0)
                    LOG("[CROUCH] no footstep for %.0f units while crouch was being toggled (stride %.0f): made one (%d so far)",
                        missed, g_stride, g_madeSteps);
            }
            g_distSinceStep = 0;
        }

        if (g_settings.logCrouch && now < g_crouchLogUntil && now >= g_crouchLogNext)
        {
            g_crouchLogNext = now + 100;
            LogChannels(pawn, now);
        }
    }
}

namespace
{
    int g_nameTakeDamage = -1;

    // A split that catches a fall still hurts, but less than the landing would: the game's own fall damage for
    // that speed (s_PlayerJumping.NotifyLanded: (|Velocity.Z| - MinBeforeDamage) / (MaxBeforeDeath -
    // MinBeforeDamage) x InitialHealth, from eGame +4A4h: +444h / +448h, InitialHealth +1530h), times
    // SplitCatchDamage, never taking him below 1 health (catching yourself always saves you). Applied like the
    // landing does: EPawn.TakeDamage(Damage, Sam, Location, up, no momentum, 8).
    void SplitCatchDamage(void* pawn, void* ctrl, float fallSpeed)
    {
        const float scale = std::clamp(g_settings.splitCatchDamage, 0.0f, 1.0f);
        if (scale <= 0.0f) return;
        float minSpeed = 1000.0f, maxSpeed = 1600.0f;
        if (void* game = Fld<void*>(ctrl, 0x4A4))
        {
            const float a = Fld<float>(game, 0x444), b = Fld<float>(game, 0x448);
            if (a > 100.0f && b > a) { minSpeed = a; maxSpeed = b; }
        }
        if (fallSpeed < minSpeed) return;
        const int initial = Fld<int>(pawn, 0x1530), health = Fld<int>(pawn, 0x420);
        if (initial <= 0 || health <= 1) return;
        const float full = (fallSpeed - minSpeed) / (maxSpeed - minSpeed) * static_cast<float>(initial);
        const int damage = std::min(std::max(1, static_cast<int>(full * scale + 0.5f)), health - 1);
        if (g_nameTakeDamage < 0) g_nameTakeDamage = Script::Name(L"TakeDamage");
        struct
        {
            int damage; void* instigator; Vec3 location, normal, momentum; uint8_t type, pad[3]; int pillTag; uint8_t spare[96];
        } parms{ damage, pawn, Fld<Vec3>(pawn, 0xE8), { 0, 0, 1 }, { 0, 0, 0 }, 8, {}, 0, {} };   // no momentum: it could knock him out of the split
        const bool ok = g_nameTakeDamage && Script::Call(pawn, g_nameTakeDamage, &parms);
        LOG("[MOVE] the split caught a fall at %.0f units/s: %d damage%s (a landing at that speed alone would do %.0f)", fallSpeed, damage,
            ok ? "" : " (FAILED to apply)", full);
    }

    // Moves Sam along the glide into the split pose; ends it (and the hold on his input) when he's there.
    void SplitGlideTick(void* pawn, DWORD now)
    {
        if (!g_glidePawn) return;
        void* ctrl = g_glideCtrl;
        const bool inSplit = pawn == g_glidePawn && g_nameSplitState > 0 && Script::StateName(ctrl) == g_nameSplitState;
        void* level = Fld<void*>(pawn, 0x7C);
        const float t = std::clamp(static_cast<float>(now - g_glideStart) / static_cast<float>(kGlideMs), 0.0f, 1.0f);
        if (!inSplit || !level)
        {
            if (pawn == g_glidePawn) Fld<uint32_t>(ctrl, 0x550) &= ~1u;
            g_glidePawn = nullptr;
            return;
        }
        const float s = t * t * (3.0f - 2.0f * t);
        const Vec3 p{ g_glideFrom.x + (g_glideTo.x - g_glideFrom.x) * s, g_glideFrom.y + (g_glideTo.y - g_glideFrom.y) * s,
                      g_glideFrom.z + (g_glideTo.z - g_glideFrom.z) * s };
        if (!SafeFarMove(level, pawn, p)) SafeFarMove(level, pawn, g_glideTo);
        int dy = (g_glideYawTo - g_glideYawFrom) & 0xFFFF;
        if (dy > 0x8000) dy -= 0x10000;
        Fld<int>(pawn, 0xF8) = (g_glideYawFrom + static_cast<int>(static_cast<float>(dy) * s)) & 0xFFFF;
        Fld<Vec3>(pawn, 0x100) = { 0, 0, 0 };
        if (t >= 1.0f)
        {
            Fld<uint32_t>(ctrl, 0x550) &= ~1u;   // bInTransition off: the state takes over
            g_glidePawn = nullptr;
        }
    }
}

void Movement::OnPlayerTick(void* pawn)
{
    if (!pawn || !Script::Ready()) return;
    void* ctrl = Fld<void*>(pawn, 0x2B4);
    if (!ctrl) return;
    const DWORD now = GetTickCount();
    const int phys = Fld<uint8_t>(pawn, 0x4C);
    SplitGlideTick(pawn, now);
    if (g_settings.rollOnlyWhenCrouching && g_rollPatch == 0 && phys == 2)
    {
        if (g_nameJumping < 0) g_nameJumping = Script::Name(L"s_PlayerJumping");
        if (g_nameJumping && Script::StateName(ctrl) == g_nameJumping)
        {
            g_rollPatch = PatchShouldRoll(ctrl) ? 1 : ++g_rollTries >= 5 ? -1 : 0;
            if (g_rollPatch) LOG("Movement: landing roll %s", g_rollPatch > 0 ? "only when crouching (ShouldRoll patched)" : "patch FAILED (ShouldRoll not found)");
        }
    }
    if (g_settings.climbOverRailings) ClimbCheck(pawn, ctrl, now);
    if (oUpdateLastAction || g_settings.crouchInstantSwitch) CrouchTick(pawn, ctrl, now, phys);
    {
        const Vec3 v = Fld<Vec3>(pawn, 0x100);
        if (phys == 2 && g_prevPhys != 2) g_takeoffSpeed = std::sqrt(v.x * v.x + v.y * v.y);
        g_prevPhys = phys;
    }
    if (!g_settings.wallBounce) return;
    TrackAfterBounce(pawn, ctrl, now);
    if (g_heldAnim && (pawn != g_heldPawn || now >= g_heldUntil))
    {
        if (pawn == g_heldPawn && Fld<int>(pawn, 0x980) == g_heldAnim) Fld<int>(pawn, 0x980) = 0;
        g_heldAnim = 0;
    }
    if (g_legPawn && (pawn != g_legPawn || now >= g_legUntil || phys != 2))
    {
        if (pawn == g_legPawn) FadeLegKick(pawn);
        g_legPawn = nullptr;
    }
    else if (g_legPawn == pawn)
    {
        // One kick: the leg swings out to the pose (frozen at its frame) at the kick's strength and back, on our
        // own smoothstep envelope over WallBounceAnimTime. Written every tick, because Sam's in-air animation
        // (PlayInAir -> LoopAnimOnly, tween 0.2 s) fades every other channel.
        const float t = static_cast<float>(now - g_legStart) / 1000.0f;
        const float total = static_cast<float>(g_legUntil - g_legStart) / 1000.0f;
        const float up = std::min(0.08f, total / 3.0f), down = std::min(0.15f, total / 2.0f);
        const float k = std::clamp(t < up ? t / up : t > total - down ? (total - t) / down : 1.0f, 0.0f, 1.0f);
        const float s = k * k * (3.0f - 2.0f * k);
        if (g_legRefSeq) HoldChannel(pawn, kLegRefChannel, s, g_legRefFrame);
        HoldChannel(pawn, kLegChannel, g_legStrength * s, g_legFrame);
    }
    if (g_steerPawn && (pawn != g_steerPawn || now >= g_steerUntil || phys != 2))
        g_steerPawn = nullptr;
    else if (g_steerPawn == pawn)
    {
        // Steering window: re-aim the boost when the held direction changes.
        Vec3 dir{}, h{};
        if (InputDir(ctrl, dir) && (!g_steerHasDir || Dot2(dir, g_steerDir) < 0.9f) && SteerVelocity(g_steerN, dir, g_steerPrev, h))
        {
            Vec3& vel = Fld<Vec3>(pawn, 0x100);
            vel.x = h.x; vel.y = h.y;
            g_steerDir = dir; g_steerHasDir = true;
            LOG("[MOVE] wall bounce steered: boost (%.0f %.0f), %.0f units/s", h.x, h.y, std::sqrt(h.x * h.x + h.y * h.y));
        }
    }

    const uint8_t jump = Fld<uint8_t>(ctrl, 0x5FE);
    const bool press = jump && !g_prevJump;
    g_prevJump = jump;
    if (press) { g_jumpPressAt = now; g_jumpPressBounced = false; }
    if (phys != 2)
    {
        if (g_bounces > 0 && g_bounces < 99 && g_settings.wallBounceSplit && g_splitProbeFloors > 0)
            LOG("[MOVE] no split-jump spot under the bounce (checked %d times over a floor)", g_splitProbeFloors);
        g_bounces = 0;
        g_splitMagnet = false;
        g_splitProbes = g_splitProbeFloors = 0;
        g_fallNTicks = 0;
        if (phys == 1) g_lastSplitZ = kNoSplit;   // landed
        return;
    }
    if (g_nameJumping < 0) g_nameJumping = Script::Name(L"s_PlayerJumping");
    if (!g_nameJumping || Script::StateName(ctrl) != g_nameJumping || (Fld<uint32_t>(ctrl, 0x550) & 1)) return;
    Vec3& vel = Fld<Vec3>(pawn, 0x100);

    // Split snap: holding jump after a wall bounce (or while falling, SplitFromFall), once Sam is over a
    // split-jump spot he stops drifting, and at the split's height he goes into the split pose. "Holding" is a
    // press kept down for WallBounceSplitHold; after a bounce only the press that made the bounce counts, so a
    // tap (or a new press for the next wall jump) never splits.
    const DWORD holdMs = static_cast<DWORD>(std::clamp(g_settings.wallBounceSplitHold, 0.0f, 2.0f) * 1000.0f);
    const bool heldLong = jump && now - g_jumpPressAt >= holdMs;
    const bool afterBounce = g_bounces > 0 && g_bounces < 99 && g_jumpPressBounced;
    const bool armed = g_settings.wallBounceSplit && g_splitCheck && heldLong && g_bounces < 99 &&
                       (afterBounce || (g_settings.splitFromFall && vel.z < 0.0f));
    if (!armed) g_splitMagnet = false;   // let go of jump: no split
    if (armed)
    {
        const Vec3 at = Fld<Vec3>(pawn, 0xE8);
        if (!g_splitMagnet)
        {
            // across the corridor: the bounced wall's normal, or when falling, the nearest wall's (looked up 8x/s)
            Vec3 n = g_steerN;
            if (!afterBounce)
            {
                if (g_fallNTicks++ % 16 == 0)
                {
                    g_fallN = {};
                    void* lv = Fld<void*>(pawn, 0x7C);
                    float d = 0;
                    Vec3 wn{};
                    if (lv && NearestWall(lv, pawn, at, std::clamp(Fld<float>(pawn, 0x224), 20.0f, 120.0f),
                                          std::clamp(Fld<float>(pawn, 0x220), 10.0f, 80.0f) + 150.0f, wn, d))
                    {
                        const float l = std::sqrt(wn.x * wn.x + wn.y * wn.y);
                        if (l > 0.01f) g_fallN = { wn.x / l, wn.y / l, 0 };
                    }
                }
                n = g_fallN;
            }
            float startZ = 0;
            bool floorFound = false;
            Vec3 spot{};
            ++g_splitProbes;
            const bool over = SplitSpotBelow(pawn, ctrl, n, startZ, floorFound, spot);
            if (floorFound) ++g_splitProbeFloors;
            if (over && startZ + kSplitRise < g_lastSplitZ - 100.0f + (g_lastSplitZ == kNoSplit ? 2.0e9f : 0.0f))
            {
                g_splitMagnet = true;
                g_splitSpot = spot;
                g_splitStartZ = startZ;
                g_splitN = n;
                g_steerPawn = nullptr;
                LOG("[MOVE] %s over a split-jump spot at (%.0f %.0f), %.0f above its start, jump held %.2f s: holding there",
                    afterBounce ? "wall bounce" : "falling", spot.x, spot.y, at.z - startZ, (now - g_jumpPressAt) / 1000.0f);
            }
        }
        if (g_splitMagnet)
        {
            // hold over the spot; go into the split at its height (rising or falling through it), or straight
            // away if he's already falling below it
            vel.x = 0;
            vel.y = 0;
            const float finalZ = g_splitStartZ + kSplitRise;
            if (std::fabs(at.z - finalZ) <= 40.0f || (vel.z <= 0.0f && at.z < finalZ))
            {
                const float fallSpeed = -vel.z;
                Vec3 placed{};
                bool ok = EnterSplitDirect(pawn, ctrl, g_splitSpot, g_splitStartZ, g_splitN, placed);
                if (!ok)
                {
                    // fall back to the game's own split (with its intro), from where he is
                    ok = SplitCheckAt(pawn, ctrl, g_splitSpot) && EnterSplit(ctrl, pawn);
                    if (ok) { Fld<Vec3>(pawn, 0x35C) = at; Fld<Vec3>(pawn, 0x368) = at; }
                }
                if (ok)
                {
                    LOG("[MOVE] %s -> split jump%s from (%.0f %.0f %.0f)", afterBounce ? "wall bounce" : "falling",
                        placed.z != 0.0f ? "" : " (with its intro)", at.x, at.y, at.z);
                    g_lastSplitZ = finalZ;
                    g_bounces = 99;
                    g_splitMagnet = false;
                    SplitCatchDamage(pawn, ctrl, fallSpeed);
                    return;
                }
                g_splitMagnet = false;   // couldn't: carry on falling
            }
        }
    }
    if (!press) return;
    if (g_bounces >= std::max(1, g_settings.wallBounceMax)) return;
    if (vel.z < -900.0f) return;   // falling too fast to kick off anything
    const bool standing = g_takeoffSpeed < g_settings.wallBounceMinSpeed;
    if (standing && !g_settings.wallBounceStanding) return;
    void* level = Fld<void*>(pawn, 0x7C);
    if (!level) return;

    const Vec3 loc = Fld<Vec3>(pawn, 0xE8);
    const float rad = std::clamp(Fld<float>(pawn, 0x220), 10.0f, 80.0f), h = std::clamp(Fld<float>(pawn, 0x224), 20.0f, 120.0f);
    const float yaw = static_cast<float>(Fld<int>(pawn, 0xF8) & 0xFFFF) * (3.14159265f / 32768.0f);
    const Vec3 facing{ std::cos(yaw), std::sin(yaw), 0 };
    const Vec3 right{ -facing.y, facing.x, 0 };   // Unreal: X forward, Y right, Z up
    Vec3 n{};
    float dist = 0;
    if (!NearestWall(level, pawn, loc, h, rad + std::clamp(g_settings.wallBounceReach, 0.0f, 300.0f), n, dist)) return;

    const float nl = std::sqrt(n.x * n.x + n.y * n.y);
    if (nl < 0.01f) return;
    n = { n.x / nl, n.y / nl, 0 };
    const Vec3 t{ -n.y, n.x, 0 };
    const Vec3 before = vel;
    const float along = Dot2(before, t);
    // Unsteered: straight up the wall (a small nudge off it), keeping 3/4 of the speed along it when moving.
    const Vec3 base = standing ? Vec3{ n.x * 30.0f, n.y * 30.0f, 0 }
                               : Vec3{ n.x * 30.0f + t.x * along * 0.75f, n.y * 30.0f + t.y * along * 0.75f, 0 };
    Vec3 horiz = base, dir{};
    const bool steered = !standing && InputDir(ctrl, dir) && SteerVelocity(n, dir, before, horiz);
    const Vec3 newVel{ horiz.x, horiz.y, standing ? g_settings.wallBounceStandingUp : g_settings.wallBounceUp };
    vel = newVel;
    ++g_bounces;
    g_jumpPressBounced = true;
    g_steerPawn = g_settings.wallBounceBoost > 0.0f && !standing ? pawn : nullptr;
    g_steerUntil = now + static_cast<DWORD>(std::clamp(g_settings.wallBounceSteerTime, 0.0f, 2.0f) * 1000.0f);
    g_steerN = n; g_steerBase = base; g_steerPrev = before; g_steerDir = dir; g_steerHasDir = steered;
    g_trackPawn = pawn; g_trackStart = now; g_trackUntil = now + 4000; g_trackLines = 0;
    g_trackNode = nullptr; g_trackBase = nullptr; g_trackPhys = -1;

    // Animation: one leg, posed for a wall in front or to the side, else the whole body (held through PlayInAir
    // via SpecialAnim).
    if (g_nameBounceAnim < 0) g_nameBounceAnim = Script::Name(g_settings.wallBounceAnim.c_str());
    if (g_namePlayAnim < 0) g_namePlayAnim = Script::Name(L"PlayAnimNoMovement");
    if (g_nameFrontR < 0) g_nameFrontR = Script::Name(g_settings.wallBounceFrontAnimR.c_str());
    if (g_nameFrontL < 0) g_nameFrontL = Script::Name(g_settings.wallBounceFrontAnimL.c_str());
    const float side = -Dot2(n, right);     // > 0: the wall is on Sam's right
    const float front = -Dot2(n, facing);   // > 0: the wall is in front of him
    const bool rightLeg = (side >= 0.0f) != (g_settings.wallBounceLeg == 1);
    const bool wallInFront = front > 0.766f;   // within 40 degrees of his facing
    const DWORD animMs = static_cast<DWORD>(std::clamp(g_settings.wallBounceAnimTime, 0.0f, 3.0f) * 1000.0f);
    bool animOk = false, legOk = false, frontKick = false;
    const wchar_t* animUsed = g_settings.wallBounceAnim.c_str();
    int baseSeq = 0;
    float baseFrame = 0;
    const bool haveBase = BasePose(pawn, baseSeq, baseFrame);
    if (haveBase && g_bounces == 1 && !standing && before.z > 0.0f)
    {
        g_goodRefSeq = baseSeq;   // first bounce of a running jump, still rising: the knees-bent pose
        g_goodRefFrame = baseFrame;
    }
    if (g_settings.wallBounceLeg != 2)
    {
        if (g_legRefSeq) FadeChannel(pawn, kLegRefChannel);   // a kick still running
        g_legRefSeq = 0;
        const int frontAnim = rightLeg ? g_nameFrontR : g_nameFrontL;
        if (wallInFront && frontAnim && PlayLegKick(pawn, frontAnim, rightLeg))
        {
            legOk = frontKick = true;
            g_legFrame = std::clamp(g_settings.wallBounceFrontFrame, 0.0f, 1.0f);
            g_legStrength = std::clamp(g_settings.wallBounceFrontStrength, 0.0f, 1.0f);
            animUsed = rightLeg ? g_settings.wallBounceFrontAnimR.c_str() : g_settings.wallBounceFrontAnimL.c_str();
        }
        else if (g_nameBounceAnim && PlayLegKick(pawn, g_nameBounceAnim, rightLeg))
        {
            legOk = true;
            g_legFrame = 0.0f;
            g_legStrength = std::clamp(g_settings.wallBounceKickStrength, 0.0f, 1.0f);
        }
        else
            FadeLegKick(pawn);
        vel = newVel;
        if (legOk)
        {
            animOk = true;
            HoldChannel(pawn, kLegChannel, 0.0f, g_legFrame);   // the envelope starts from nothing
            const int ref = g_goodRefSeq ? g_goodRefSeq : haveBase ? baseSeq : 0;
            g_legRefFrame = g_goodRefSeq ? g_goodRefFrame : baseFrame;
            if (ref && PlayOnLeg(pawn, ref, rightLeg, kLegRefChannel))
            {
                g_legRefSeq = ref;
                HoldChannel(pawn, kLegRefChannel, 0.0f, g_legRefFrame);
            }
            else
                HoldChannel(pawn, kLegRefChannel, 0.0f, -1.0f);
            g_legPawn = pawn; g_legStart = now; g_legUntil = now + std::max<DWORD>(animMs, 150);
        }
    }
    if (!legOk && g_nameBounceAnim && g_namePlayAnim)
    {
        const float ground = Fld<float>(pawn, 0x2E8);
        struct { int sequence; float rate, tween; int backward, ret; int spare[3]; } parms{ g_nameBounceAnim, 1.0f, 0.1f, 0, 0, {} };
        animOk = Script::Call(pawn, g_namePlayAnim, &parms);
        Fld<float>(pawn, 0x2E8) = ground;
        vel = newVel;
        if (animOk)
        {
            Fld<int>(pawn, 0x980) = g_nameBounceAnim;
            g_heldAnim = g_nameBounceAnim;
            g_heldPawn = pawn;
            g_heldUntil = now + animMs;
        }
    }
    if (g_settings.wallBounceNoise)
    {
        if (g_namePlayJump < 0) g_namePlayJump = Script::Name(L"PlayJump");
        int spare[8] = {};
        Script::Call(pawn, g_namePlayJump, spare);
    }
    LOG("[MOVE] wall bounce %d (%ls, took off at %.0f units/s): wall %.0f away %ls (normal %.2f %.2f), up %.0f, %ls %.0f units/s, %ls on %ls (strength %.2f)%ls",
        g_bounces, standing ? L"standing" : L"moving", g_takeoffSpeed, dist,
        wallInFront ? L"in front" : side >= 0.0f ? L"on Sam's right" : L"on Sam's left", n.x, n.y, newVel.z,
        steered ? L"steered" : L"straight up,", std::sqrt(horiz.x * horiz.x + horiz.y * horiz.y), animUsed,
        legOk ? (rightLeg ? L"the right leg" : L"the left leg") : L"the whole body", legOk ? g_legStrength : 1.0f,
        animOk ? (wallInFront && !frontKick && legOk ? L" (front pose not available, side pose instead)" : L"")
               : g_nameBounceAnim ? L" (couldn't play)" : L" (not found)");
    if (legOk)
    {
        wchar_t refName[48] = L"none", baseName[48] = L"?";
        if (g_legRefSeq && !Script::NameText(g_legRefSeq, refName, 48)) swprintf_s(refName, L"#%d", g_legRefSeq);
        if (haveBase && !Script::NameText(baseSeq, baseName, 48)) swprintf_s(baseName, L"#%d", baseSeq);
        LOG("[MOVE]   leg over %ls f%.2f (%s), Sam's pose %ls f%.2f", refName, g_legRefFrame,
            !g_legRefSeq ? "no reference" : g_goodRefSeq && (g_goodRefSeq != baseSeq || g_bounces > 1) ? "from a running jump's first bounce"
                                                                                                       : "his pose now",
            baseName, baseFrame);
    }
}
