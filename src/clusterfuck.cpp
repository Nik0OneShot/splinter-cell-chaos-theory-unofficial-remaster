// CLUSTERFUCK: a fourth difficulty, selectable in the menu, layered on Expert.
//
// Menu: the difficulty page (vtable around 0x110E39C0: B_Select / Lst_Difficulty / P_Difficulty) fills its list
// in 0x10B54E50 (thiscall, no arguments) with Localize("OPTIONSVALUE", "Difficulty[1..3]") through the list's
// AddItem (0x10D2FC80: thiscall (const TCHAR* text, int data), ret 8). Its Select handler reads the list's
// selected row (list +0A8h, a word: 0/1/2), turns it into difficulty 1/2/3 and calls 0x10B55050 (difficulty),
// which starts the campaign or mission (anything else becomes 2). Here the list gets a fourth row, CLUSTERFUCK;
// selecting it starts the game on Expert (3) and marks the campaign that starts with the next map load.
//
// Campaigns: the game knows nothing of them, so the mod keeps them in System\SCCTRemaster_Campaigns.ini: each
// CLUSTERFUCK campaign gets an ID when its first mission loads, and every save written while it is active is
// recorded under it (the savegame CreateFileW hook in guards.cpp reports writes and the save a map load reads).
// Loading a save makes its campaign active again (or none); a mission that follows another (MISSIONDIFF in the
// load URL, no pending choice) keeps the active one; a new game picked with Normal/Hard/Expert ends it.
#include "pch.h"
#include "features.h"

extern HMODULE g_self;

namespace
{
    template <class T> T& At(void* p, uintptr_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p) + off); }

    using Builder_t = void(__fastcall*)(void* page, void* edx);
    using AddItem_t = void(__fastcall*)(void* list, void* edx, const wchar_t* text, int data);
    Builder_t oBuilder = nullptr;
    AddItem_t pAddItem = nullptr;
    constexpr uintptr_t kPageList = 0x40, kListSel = 0xA8, kListMaxRows = 0xA6;
    constexpr int kRow = 3;   // the list row CLUSTERFUCK gets

    enum class Pending { None, Normal, Clusterfuck };
    Pending g_pending = Pending::None;
    std::wstring g_active;   // the active campaign's ID ("" = not a CLUSTERFUCK game)
    std::wstring g_store;    // System\SCCTRemaster_Campaigns.ini
    std::wstring g_map;      // the map being played

    std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

    std::wstring ReadStr(const std::wstring& sec, const wchar_t* key)
    {
        wchar_t buf[1024] = L"";
        GetPrivateProfileStringW(sec.c_str(), key, L"", buf, 1024, g_store.c_str());
        return buf;
    }
    int ReadInt(const std::wstring& sec, const wchar_t* key) { return GetPrivateProfileIntW(sec.c_str(), key, 0, g_store.c_str()); }
    void WriteStr(const std::wstring& sec, const wchar_t* key, const std::wstring& v) { WritePrivateProfileStringW(sec.c_str(), key, v.c_str(), g_store.c_str()); }
    void WriteInt(const std::wstring& sec, const wchar_t* key, int v) { WriteStr(sec, key, std::to_wstring(v)); }

    std::wstring Section(const std::wstring& id) { return L"Campaign " + id; }

    // All campaign IDs in the store.
    std::vector<std::wstring> Campaigns()
    {
        std::vector<std::wstring> out;
        std::vector<wchar_t> buf(65536);
        const DWORD n = GetPrivateProfileSectionNamesW(buf.data(), static_cast<DWORD>(buf.size()), g_store.c_str());
        for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1)
            if (!wcsncmp(p, L"Campaign ", 9)) out.emplace_back(p + 9);
        return out;
    }

    // The campaign a save file belongs to ("" if none).
    std::wstring CampaignOfSave(const std::wstring& path)
    {
        const std::wstring want = Lower(path);
        for (const auto& id : Campaigns())
        {
            const std::wstring sec = Section(id);
            const int n = ReadInt(sec, L"Saves");
            for (int i = 1; i <= n; ++i)
                if (Lower(ReadStr(sec, (L"Save" + std::to_wstring(i)).c_str())) == want) return id;
        }
        return L"";
    }

    std::wstring NowText()
    {
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t b[64];
        swprintf_s(b, L"%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        return b;
    }

    void StartCampaign()
    {
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t id[64];
        swprintf_s(id, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        g_active = id;
        const std::wstring sec = Section(g_active);
        WriteStr(sec, L"Started", NowText());
        WriteStr(sec, L"Status", L"alive");
        WriteInt(sec, L"Saves", 0);
        WriteInt(sec, L"Missions", 0);
        LOG("[CLUSTERFUCK] a new CLUSTERFUCK campaign begins (%ls)", g_active.c_str());
    }

    void NoteMission(const std::wstring& map)
    {
        if (g_active.empty() || map.empty()) return;
        const std::wstring sec = Section(g_active);
        const int n = ReadInt(sec, L"Missions");
        if (n > 0 && !_wcsicmp(ReadStr(sec, (L"Mission" + std::to_wstring(n)).c_str()).c_str(), map.c_str())) return;
        WriteInt(sec, L"Missions", n + 1);
        WriteStr(sec, (L"Mission" + std::to_wstring(n + 1)).c_str(), map);
    }

    void __fastcall hkBuilder(void* page, void* edx)
    {
        oBuilder(page, edx);
        __try
        {
            void* list = At<void*>(page, kPageList);
            if (list && pAddItem)
            {
                // The list shows at most +A6h rows (visible = min(that, items), +A0h); the page sets 3.
                uint8_t& maxRows = At<uint8_t>(list, kListMaxRows);
                if (maxRows < kRow + 1) maxRows = kRow + 1;
                pAddItem(list, nullptr, L"CLUSTERFUCK", 0);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // At the Select handler's call 0x10B55050(difficulty): row 3 is CLUSTERFUCK (Expert underneath).
    void __cdecl OnSelect(Mem::Regs* r)
    {
        __try
        {
            void* list = At<void*>(reinterpret_cast<void*>(r->esi), kPageList);
            const int row = list ? At<int16_t>(list, kListSel) : -1;
            if (row == kRow)
            {
                r->Stack<int>(0) = 3;
                g_pending = Pending::Clusterfuck;
                LOG("[CLUSTERFUCK] difficulty CLUSTERFUCK selected (Expert underneath)");
            }
            else
                g_pending = Pending::Normal;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// ---- Sam: 50 HP, no medkits, half ammo, no ammo pickups, no light/sound meters ----
//
// Health: ESam.PostBeginPlay sets Health (Pawn +420h) by difficulty and EPawn.PostBeginPlay copies it to
// InitialHealth (+1530h), the cap of every heal (revive, medkit, cheat) and the HUD bar's full mark; saves restore
// both. Here, every tick: InitialHealth 50, Health clamped down to it.
// Medkits are wall cabinets (EMedKit, EMedKitInteraction): its IsAvailable (0x10C63790, thiscall, no arguments)
// returns 0 here, as it does for an empty cabinet: no prompt, no use.
// Ammo: UEMissionManager::SetupPlayer (0x10C27AB0, thiscall (EPC), ret 4) resets the SC-20K's and the FN7's pools
// (EWeapon +434h: clip, capacity +4, total +8; EPC +560h main gun, +564h handgun) to their class defaults and adds
// the mission's clips; the attachments (EMainGun +56Ch sniper, pool +454h; +570h shotgun, pool +41Ch) only get
// clips added. After it: main and hand totals halved, attachments get half of what was added.
// Pickups: EPickupInteraction::IsAvailable (0x10C04C10) returns 0 for an EAmmo (crates, boxes, bodies' ammo).
// HUD: the Magma HUD's per-frame update (0x10B6A690, thiscall (float dt), ret 4) runs the meters; their nodes are
// hud +6Ch ("stealthbar"), +70h ("indicator_stealth") and the sound meter ("Slider_soundmeter", the child of the
// container hud +28h whose object (+8) is hud +78h). A node and its children are drawn only with bit 0 of +18h.
namespace
{
    constexpr uintptr_t kHealth = 0x420, kInitialHealth = 0x1530;
    constexpr int kMaxHealth = 50;

    using Avail_t = int(__fastcall*)(void* self, void* edx);
    using Setup_t = void(__fastcall*)(void* mm, void* edx, void* epc);
    using HudTick_t = void(__fastcall*)(void* hud, void* edx, float dt);
    Avail_t oMedkitAvail = nullptr, oPickupAvail = nullptr;
    Setup_t oSetupPlayer = nullptr;
    HudTick_t oHudTick = nullptr;
    bool g_hudHidden = false;
    std::unordered_map<void*, bool> g_isAmmo;   // pickup owner -> is an EAmmo
    void* g_ammoLevel = nullptr;

    int __fastcall hkMedkitAvail(void* self, void* edx)
    {
        if (Clusterfuck::Active()) return 0;
        return oMedkitAvail(self, edx);
    }

    bool IsAmmo(void* obj)
    {
        wchar_t chain[8][64] = {};
        const int n = Game::ClassChain(obj, chain, 8);
        for (int i = 0; i < n; ++i)
            if (!wcscmp(chain[i], L"EAmmo")) return true;
        return false;
    }

    bool OwnerIsAmmo(void* self)
    {
        void* owner = At<void*>(self, 0x6C);
        if (!owner) return false;
        void* level = At<void*>(owner, 0x78);
        if (level != g_ammoLevel) { g_ammoLevel = level; g_isAmmo.clear(); }
        auto it = g_isAmmo.find(owner);
        if (it == g_isAmmo.end()) it = g_isAmmo.emplace(owner, IsAmmo(owner)).first;
        return it->second;
    }

    bool OwnerIsAmmoSafe(void* self)
    {
        __try { return OwnerIsAmmo(self); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    int __fastcall hkPickupAvail(void* self, void* edx)
    {
        if (Clusterfuck::Active() && OwnerIsAmmoSafe(self)) return 0;
        return oPickupAvail(self, edx);
    }

    struct Pool { int clip, capacity, total; };
    Pool* PoolOf(void* weapon, uintptr_t off) { return weapon ? reinterpret_cast<Pool*>(reinterpret_cast<uintptr_t>(weapon) + off) : nullptr; }

    void HalveAfterSetup(void* epc, int sniperBefore, int shotgunBefore)
    {
        void* main = At<void*>(epc, 0x560);
        void* hand = At<void*>(epc, 0x564);
        for (Pool* p : { PoolOf(main, 0x434), PoolOf(hand, 0x434) })
            if (p && p->total > 0) { p->total /= 2; p->clip = std::min(p->clip, p->total); }
        void* sniper = main ? At<void*>(main, 0x56C) : nullptr;
        void* shotgun = main ? At<void*>(main, 0x570) : nullptr;
        if (Pool* p = PoolOf(sniper, 0x454)) { if (p->total > sniperBefore) p->total = sniperBefore + (p->total - sniperBefore) / 2; p->clip = std::min(p->clip, p->total); }
        if (Pool* p = PoolOf(shotgun, 0x41C)) { if (p->total > shotgunBefore) p->total = shotgunBefore + (p->total - shotgunBefore) / 2; p->clip = std::min(p->clip, p->total); }
        LOG("[CLUSTERFUCK] Sam's ammo halved: SC-20K %d, FN7 %d, sniper %d, shotgun %d", PoolOf(main, 0x434) ? PoolOf(main, 0x434)->total : -1,
            PoolOf(hand, 0x434) ? PoolOf(hand, 0x434)->total : -1, PoolOf(sniper, 0x454) ? PoolOf(sniper, 0x454)->total : -1,
            PoolOf(shotgun, 0x41C) ? PoolOf(shotgun, 0x41C)->total : -1);
    }

    void __fastcall hkSetupPlayer(void* mm, void* edx, void* epc)
    {
        int sniperBefore = 0, shotgunBefore = 0;
        const bool cf = Clusterfuck::Active() && epc;
        if (cf)
        {
            __try
            {
                void* main = At<void*>(epc, 0x560);
                void* sniper = main ? At<void*>(main, 0x56C) : nullptr;
                void* shotgun = main ? At<void*>(main, 0x570) : nullptr;
                if (sniper) sniperBefore = PoolOf(sniper, 0x454)->total;
                if (shotgun) shotgunBefore = PoolOf(shotgun, 0x41C)->total;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        oSetupPlayer(mm, edx, epc);
        if (cf)
        {
            __try { HalveAfterSetup(epc, sniperBefore, shotgunBefore); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    void SetVisible(void* node, bool on)
    {
        if (!node) return;
        uint32_t& f = At<uint32_t>(node, 0x18);
        f = on ? (f | 1u) : (f & ~1u);
    }

    void* SoundMeterNode(void* hud)
    {
        void* container = At<void*>(hud, 0x28);
        void* target = At<void*>(hud, 0x78);
        if (!container || !target) return nullptr;
        void** kids = At<void**>(container, 0x8);
        const int n = At<int>(container, 0xC);
        for (int i = 0; kids && i < n && i < 4096; ++i)
            if (kids[i] && At<void*>(kids[i], 0x8) == target) return kids[i];
        return nullptr;
    }

    void MeterVisibility(void* hud, bool show)
    {
        SetVisible(At<void*>(hud, 0x6C), show);
        SetVisible(At<void*>(hud, 0x70), show);
        SetVisible(SoundMeterNode(hud), show);
    }

    void __fastcall hkHudTick(void* hud, void* edx, float dt)
    {
        oHudTick(hud, edx, dt);
        __try
        {
            if (Clusterfuck::Active()) { MeterVisibility(hud, false); g_hudHidden = true; }
            else if (g_hudHidden) { MeterVisibility(hud, true); g_hudHidden = false; }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// ---- alarms ----
//
// The level's alarm count is LevelInfo.CurrentAlertLevel (+738h, int: -1 none, 0..2 = alarms 1-3, 3 = base
// alert); alarms go through AEAIZone::StartAlarm (0x1099BB50: thiscall (int bPropagated, Pawn* starter, FVector
// loc by value, UBOOL bForce, Actor* instigator), ret 1Ch), which counts one stage when forced, puts the zone in
// alarm (lights, sirens, AI_HEAR_ALARM, backup to loc, the zone's alert layout), gives guards body armour from the
// 2nd alarm and helmets from the 3rd, and at base alert puts every zone in alarm. LevelInfo +758h bit 1 is
// bAlarmSystemIsDisable (the Bathhouse turns its alarms off after the meeting); EAIZones is a TArray at +728h.
// Level scripts step through the stages one at a time (polling every 1-3 s), so they are raised one by one.
// Lighthouse, Seoul One and Seoul Two have no alarm system and are left alone.
// CLUSTERFUCK: base alert at the start of every mission with alarms (four forced alarms 4.5 s apart, from 3 s in,
// backup sent to the zone itself), and any body a guard finds (AI_CHANGED_DEAD 112 / UNCONSCIOUS 113) raises an
// alarm in his zone 1.5 s later, unless he's stopped first.
namespace
{
    using StartAlarm_t = void(__fastcall*)(void* zone, void* edx, int prop, void* starter, float x, float y, float z, int force, void* inst);
    StartAlarm_t pStartAlarm = nullptr;
    constexpr uintptr_t kAlertLevel = 0x738, kAlarmFlags = 0x758, kZones = 0x728, kLocation = 0xE8, kTime = 0x430;
    void* g_alarmLevel = nullptr;
    float g_alarmNext = -1;
    int g_alarmsRaised = 0;
    struct BodyAlarm { void* ctrl; void* body; float due; };
    std::vector<BodyAlarm> g_bodyAlarms;
    std::vector<void*> g_bodiesReported;

    struct Vec3 { float x, y, z; };

    bool MapHasAlarms()
    {
        const std::wstring m = Lower(g_map);
        return !m.empty() && m.find(L"lighthouse") == std::wstring::npos && m.find(L"seoulone") == std::wstring::npos &&
               m.find(L"seoultwo") == std::wstring::npos;
    }

    bool AlarmsOff(void* level) { return (At<uint32_t>(level, kAlarmFlags) & 2) != 0; }

    void* FirstZone(void* level)
    {
        void** zones = At<void**>(level, kZones);
        return zones && At<int>(level, kZones + 4) > 0 ? zones[0] : nullptr;
    }

    void RaiseAlarm(void* zone, void* starter, const Vec3& at)
    {
        if (pStartAlarm && zone) pStartAlarm(zone, nullptr, 0, starter, at.x, at.y, at.z, 1, nullptr);
    }

    bool OutOfAction(void* ctrl, void* pawn)
    {
        if (!pawn || At<int>(pawn, kHealth) <= 0) return true;
        wchar_t st[96];
        for (void* o : { ctrl, pawn })
            if (Script::NameText(Script::StateName(o), st, 96) && (wcsstr(st, L"Unconscious") || wcsstr(st, L"Dead") || wcsstr(st, L"Dying")))
                return true;
        return false;
    }

    void AlarmTick(void* sam)
    {
        void* level = At<void*>(sam, 0x78);
        if (!level) return;
        const float now = At<float>(level, kTime);
        if (level != g_alarmLevel)
        {
            g_alarmLevel = level;
            g_alarmNext = now + 3.0f;
            g_alarmsRaised = 0;
            g_bodyAlarms.clear();
            g_bodiesReported.clear();
        }
        // Base alert at the start.
        if (g_alarmNext >= 0 && now >= g_alarmNext)
        {
            const int stage = At<int>(level, kAlertLevel);
            void* zone = FirstZone(level);
            if (!MapHasAlarms() || AlarmsOff(level) || stage >= 3 || !zone || g_alarmsRaised >= 6)
            {
                if (g_alarmsRaised) LOG("[CLUSTERFUCK] base alert reached (alarm level %d)", At<int>(level, kAlertLevel));
                g_alarmNext = -1;
            }
            else
            {
                RaiseAlarm(zone, nullptr, At<Vec3>(zone, kLocation));
                ++g_alarmsRaised;
                LOG("[CLUSTERFUCK] alarm %d raised at the start of the mission (level now %d)", g_alarmsRaised, At<int>(level, kAlertLevel));
                g_alarmNext = now + 4.5f;
            }
        }
        // Bodies found.
        for (size_t i = 0; i < g_bodyAlarms.size();)
        {
            BodyAlarm& b = g_bodyAlarms[i];
            if (now < b.due) { ++i; continue; }
            void* pawn = At<void*>(b.ctrl, 0x41C);
            if (!OutOfAction(b.ctrl, pawn) && !AlarmsOff(level))
            {
                void* zone = At<void*>(pawn, 0x428);
                if (!zone) zone = FirstZone(level);
                RaiseAlarm(zone, pawn, At<Vec3>(pawn, kLocation));
                LOG("[CLUSTERFUCK] a guard found a body and raised the alarm (level now %d)", At<int>(level, kAlertLevel));
            }
            else
                LOG("[CLUSTERFUCK] a guard who found a body was stopped before he raised the alarm");
            g_bodyAlarms.erase(g_bodyAlarms.begin() + i);
        }
    }
}

namespace
{
    void AlarmTickSafe(void* sam)
    {
        __try { AlarmTick(sam); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

namespace { void LightEvent(uint8_t* ev); }

void Clusterfuck::OnGuardEvent(void* ctrl, const uint8_t* ev)
{
    if (g_active.empty()) return;
    __try { LightEvent(const_cast<uint8_t*>(ev)); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!pStartAlarm) return;
    const uint8_t type = ev[0];
    if (type != 112 && type != 113) return;
    __try
    {
        void* body = *reinterpret_cast<void* const*>(ev + 0x38);
        void* level = At<void*>(ctrl, 0x78);
        if (!body || !level) return;
        if (std::find(g_bodiesReported.begin(), g_bodiesReported.end(), body) != g_bodiesReported.end()) return;
        g_bodiesReported.push_back(body);
        g_bodyAlarms.push_back({ ctrl, body, At<float>(level, kTime) + 1.5f });
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Wall mines: EWallMineInteraction.InitInteract sets DefuseSuccessful (+2CCh bit 0) to !Emitting (true below
// Expert) and makes itself the player's Interaction (EPlayerController +284h); Interact, at the end of the defuse,
// detonates the mine when it is false. Here, while Sam defuses one, it stays false: every defuse sets it off.
namespace
{
    void* g_mineInter = nullptr;    // last interaction checked
    bool g_mineIsMine = false;

    void SabotageDefuse(void* pawn)
    {
        void* ctrl = At<void*>(pawn, 0x2B4);
        void* inter = ctrl ? At<void*>(ctrl, 0x284) : nullptr;
        if (!inter) return;
        if (inter != g_mineInter)
        {
            g_mineInter = inter;
            wchar_t chain[2][64] = {};
            g_mineIsMine = Game::ClassChain(inter, chain, 1) > 0 && !wcscmp(chain[0], L"EWallMineInteraction");
            if (g_mineIsMine) LOG("[CLUSTERFUCK] Sam is defusing a wall mine: it will go off");
        }
        if (g_mineIsMine) At<uint32_t>(inter, 0x2CC) &= ~1u;
    }
}

// ---- guards and the world ----
//
// Night vision: EPawn +42Ch bit 2 (bUseNightVision), read live by the sight check (0x10BB24DB: with goggles
// Sam's visibility is at least 115, darkness gives no cover). Flashlights: bit 1 of the same flags
// (bCanUseFlashlight) and EchelonGameInfo +334h bit 0 (m_bEnableFlashlightToAll, Level +5D0h): guards light
// dark spots with their weapon lights. Elite stats (EElite class defaults x1.5 on Expert): health 225,
// GunAccuracy/Min/Max (+9C8h/+9CCh/+9D0h) 1.0/0.45/1.0 at least, gas immunity (Pawn +424h bit 0).
// Ammo: the weapon's pool (+434h clip, +438h capacity, +43Ch total) kept full; the soldier pattern's bNeedReload
// (BaseSoldier +814h bit 9) cleared so the reload label never runs. Searches: bInfiniteSearch (+814h bit 10), and
// while searching (+410h bit 3) the search's end (StateEvent.fValue, pattern +5F4h) is kept a minute ahead and
// the stress step (+3F0h) at 4 or more, so ReachedSearchEnd carries on. Knocked-out guards (controller in
// s_Unconscious, EPawn.TimeOfDeath +9B8h set when he went down) are revived after kWakeAfter seconds through
// their own s_Unconscious.Revive (self as the reviver), as a guard reviving them would.
// Cameras and turrets (ESensor): AlarmDetectionDelay (+43Ch) 0; turrets: ShootDetectionDelay (+4ECh) 0,
// BulletsPerMinute (+4F0h) 1000+, BulletDamage (+4F4h) x1.5, spread (+4FCh) 0, no burst pauses (+50Ch).
// Lights put out with the OCP (AI_CHANGED_MALFUNCTIONING 60 / AI_HEAR_DISFUNCTION 61) reach guards as
// AI_CHANGED_LIGHT_OUT (80), the event a shot-out light sends, which soldiers investigate even when calm.
namespace
{
    constexpr float kWakeAfter = 150.0f;
    void* g_worldLevel = nullptr;
    float g_worldNext = 0;
    std::unordered_map<void*, float> g_guardSeen;    // pawn -> next wake attempt time (also: elite stats applied)
    std::unordered_map<void*, int> g_patternKind;    // pattern -> 1 soldier, 0 other
    std::unordered_map<void*, int> g_classKind;      // actor class -> 0 other, 1 sensor, 2 turret

    bool ChainHas(void* obj, const wchar_t* name, int depth = 12)
    {
        wchar_t chain[16][64] = {};
        const int n = Game::ClassChain(obj, chain, std::min(depth, 16));
        for (int i = 0; i < n; ++i)
            if (wcsstr(chain[i], name)) return true;
        return false;
    }

    bool StateHas(void* obj, const wchar_t* part)
    {
        wchar_t st[96];
        return obj && Script::NameText(Script::StateName(obj), st, 96) && wcsstr(st, part);
    }

    void GuardTick(void* pawn)
    {
        void* level = At<void*>(pawn, 0x78);
        void* ctrl = At<void*>(pawn, 0x2B4);
        if (!level || !ctrl || At<int>(pawn, kHealth) <= 0) return;
        if (level != g_worldLevel) return;   // the world pass resets the per-level maps first
        void* weapon = At<void*>(pawn, 0x6EC);
        const float now = At<float>(level, kTime);
        auto seen = g_guardSeen.find(pawn);
        if (seen == g_guardSeen.end())
        {
            seen = g_guardSeen.emplace(pawn, 0.0f).first;
            if (weapon)   // an armed guard: elite stats
            {
                float& acc = At<float>(pawn, 0x9C8); float& lo = At<float>(pawn, 0x9CC); float& hi = At<float>(pawn, 0x9D0);
                acc = std::max(acc, 1.0f); lo = std::max(lo, 0.45f); hi = std::max(hi, 1.0f);
                int& init = At<int>(pawn, kInitialHealth); int& hp = At<int>(pawn, kHealth);
                if (init > 0 && init < 225) { hp = hp * 225 / init; init = 225; }
                At<uint32_t>(pawn, 0x424) |= 1u;
            }
        }
        // Knocked out: wake up after a while.
        if (StateHas(ctrl, L"Unconscious"))
        {
            const float down = At<float>(pawn, 0x9B8);
            if (down > 0 && now - down > kWakeAfter && now >= seen->second && !StateHas(pawn, L"Carr") && !StateHas(pawn, L"Grab") &&
                !StateHas(ctrl, L"Carr") && !StateHas(ctrl, L"Grab"))
            {
                seen->second = now + 10.0f;
                static int revive = 0;
                if (!revive) revive = Script::Name(L"Revive");
                void* parms[4] = { pawn, nullptr, nullptr, nullptr };
                if (revive && Script::Call(ctrl, revive, parms))
                    LOG("[CLUSTERFUCK] a guard knocked out %.0f s ago wakes up", now - down);
            }
            return;
        }
        if (!weapon) return;
        At<uint32_t>(pawn, 0x42C) |= 4u | 2u;   // night vision, flashlight
        int& clip = At<int>(weapon, 0x434); const int cap = At<int>(weapon, 0x438); int& total = At<int>(weapon, 0x43C);
        if (cap > 0 && cap < 1000) { clip = cap; total = std::max(total, cap * 20); }
        void* pattern = At<void*>(ctrl, 0x42C);
        if (!pattern) return;
        auto kind = g_patternKind.find(pattern);
        if (kind == g_patternKind.end()) kind = g_patternKind.emplace(pattern, ChainHas(pattern, L"BaseSoldier") ? 1 : 0).first;
        if (kind->second != 1) return;
        uint32_t& flags = At<uint32_t>(pattern, 0x814);
        flags = (flags & ~0x200u) | 0x400u;   // no reload, infinite search
        if (At<uint32_t>(pattern, 0x410) & 0x08)
        {
            float& end = At<float>(pattern, 0x5F4);
            end = std::max(end, now + 60.0f);
            uint8_t& stress = At<uint8_t>(pattern, 0x3F0);
            stress = std::max<uint8_t>(stress, 4);
        }
    }

    void WorldTick(void* sam)
    {
        void* level = At<void*>(sam, 0x78);
        void* xlevel = At<void*>(sam, 0x7C);
        if (!level || !xlevel) return;
        const float now = At<float>(level, kTime);
        static std::vector<void*> tuned;
        if (level != g_worldLevel)
        {
            g_worldLevel = level; g_worldNext = now + 1.0f;
            g_guardSeen.clear(); g_patternKind.clear(); tuned.clear();
        }
        if (now < g_worldNext) return;
        g_worldNext = now + 5.0f;
        if (void* game = At<void*>(level, 0x5D0)) At<uint32_t>(game, 0x334) |= 1u;   // flashlights for all
        void** actors = At<void**>(xlevel, 0x28);
        const int n = At<int>(xlevel, 0x2C);
        int cams = 0, turrets = 0;
        for (int i = 0; actors && i < n && i < 500000; ++i)
        {
            void* a = actors[i];
            if (!a || (At<uint32_t>(a, 0x28) & 4) || (At<uint32_t>(a, 0x2C) & 0x10)) continue;   // skip deleted and pawns
            // What kind of actor, by class (0 other, 1 sensor, 2 turret), looked up once per class.
            void* cls = At<void*>(a, 0x24);
            auto kind = g_classKind.find(cls);
            if (kind == g_classKind.end())
                kind = g_classKind.emplace(cls, !ChainHas(a, L"ESensor", 6) ? 0 : (ChainHas(a, L"Turret", 6) || ChainHas(a, L"EIFVTank", 6)) ? 2 : 1).first;
            if (!kind->second) continue;
            if (std::find(tuned.begin(), tuned.end(), a) != tuned.end()) continue;
            tuned.push_back(a);
            At<float>(a, 0x43C) = 0.0f;
            ++cams;
            if (kind->second == 2)
            {
                At<float>(a, 0x4EC) = 0.0f;
                At<float>(a, 0x4F0) = std::max(At<float>(a, 0x4F0), 1000.0f);
                At<int>(a, 0x4F4) = At<int>(a, 0x4F4) * 3 / 2;
                At<int>(a, 0x4FC) = 0;
                At<float>(a, 0x50C) = 0.0f;
                ++turrets;
            }
        }
        if (cams) LOG("[CLUSTERFUCK] %d security devices made instant (%d turrets made deadlier)", cams, turrets);
    }

    void LightEvent(uint8_t* ev)
    {
        if (ev[0] != 60 && ev[0] != 61) return;
        void* about = *reinterpret_cast<void**>(ev + 0x38);
        if (about && ChainHas(about, L"EGameplayObjectLight", 6)) ev[0] = 80;
    }
}

void Clusterfuck::OnNpcTick(void* pawn)
{
    if (g_active.empty() || !pawn) return;
    __try { GuardTick(pawn); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- Ironman and permadeath ----
//
// Saving: SaveMgr::Save (pause-menu Save page confirm 0x10B553B0, or QuickSave via the menu's HandleEvent
// 0x10B256D0 event 31) runs SAVEGAME; the engine writes it next tick in UGameEngine::SaveGame (0x10A44EE0,
// thiscall (const TCHAR* file), returns UBOOL) to file.tmp and copies it over file. Each saved file of an active
// campaign is recorded from there. Ironman: the Save page and QuickSave are refused (the stock "save failed" box,
// menu event 19 with 1), except the mod's own save at the start of every mission: QuickSave's name (the global
// the QuickSave routine reads first, 0x111C2224) is pointed at "CLUSTERFUCK <campaign> <map>" for that one call,
// as soon as the player controller's CanSaveGame (0x10A3CD40) allows it.
// Death: AActor::eventTakeDamage (0x10B7B2A0, every damage source) taking Sam from above 0 to 0 health (also
// caught in the tick: KillerActor EPawn +CF4h). The campaign's saves are deleted through SaveMgr::DeleteSave
// (0x10AEC8A0, thiscall (FString* profileName, Profile*, int index): file, record and the profile's .dat; the
// SaveMgr is UGameEngine +1D4h, the profile and its name the menu's +12Ch/+130h, its save records the TArray at
// profile +ACh: {FString name, BYTE mission, BYTE difficulty}, 16 bytes) and any file left is deleted too. A
// "REST IN PEACE" text takes their place. Retry (menu RestartMission 0x10B23870) is refused after the death.
namespace
{
    using SaveGame_t = int(__fastcall*)(void* engine, void* edx, const wchar_t* file);
    using HandleEvent_t = int(__fastcall*)(void* menu, void* edx, int id, int a, const wchar_t* s);
    using SavePage_t = void(__fastcall*)(void* page, void* edx, void* name);
    using Restart_t = void(__fastcall*)(void* menu, void* edx, int a);
    using CanSave_t = int(__fastcall*)(void* pc, void* edx);
    using GetEngine_t = void* (__cdecl*)();
    using DeleteSave_t = void(__fastcall*)(void* mgr, void* edx, void* profileName, void* profile, int index);
    using TakeDamage_t = void(__fastcall*)(void* actor, void* edx, int dmg, void* inst, float hx, float hy, float hz,
                                           float nx, float ny, float nz, float mx, float my, float mz, int type, int pill);
    SaveGame_t oSaveGame = nullptr;
    HandleEvent_t oHandleEvent = nullptr;
    SavePage_t oSavePage = nullptr;
    Restart_t oRestart = nullptr;
    CanSave_t pCanSave = nullptr;
    GetEngine_t pGetEngine = nullptr;
    DeleteSave_t pDeleteSave = nullptr;
    TakeDamage_t oTakeDamage = nullptr;
    const wchar_t** g_quickName = nullptr;

    void* g_menu = nullptr;
    void* g_samPawn = nullptr;
    bool g_allowSave = false, g_startSave = false, g_dead = false;
    float g_startSaveAfter = 0;
    std::wstring g_startName;

    int __fastcall hkSaveGame(void* engine, void* edx, const wchar_t* file)
    {
        const int ok = oSaveGame(engine, edx, file);
        if (ok && file) Clusterfuck::OnSaveWritten(file);
        return ok;
    }

    int __fastcall hkHandleEvent(void* menu, void* edx, int id, int a, const wchar_t* s)
    {
        g_menu = menu;
        if (id == 0x1F && !g_active.empty() && !g_allowSave)
        {
            LOG("[CLUSTERFUCK] quicksave refused: Ironman (only the mission-start save)");
            return oHandleEvent(menu, edx, 0x13, 1, L"");
        }
        return oHandleEvent(menu, edx, id, a, s);
    }

    void __fastcall hkSavePage(void* page, void* edx, void* name)
    {
        if (!g_active.empty())
        {
            void* menu = page ? At<void*>(page, 0x14) : nullptr;
            LOG("[CLUSTERFUCK] save refused: Ironman (only the mission-start save)");
            if (menu && oHandleEvent) oHandleEvent(menu, nullptr, 0x13, 1, L"");
            return;
        }
        oSavePage(page, edx, name);
    }

    void __fastcall hkRestart(void* menu, void* edx, int a)
    {
        g_menu = menu;
        if (g_dead)
        {
            LOG("[CLUSTERFUCK] retry refused: permadeath");
            return;
        }
        oRestart(menu, edx, a);
    }

    void TryStartSave(void* pawn)
    {
        void* level = At<void*>(pawn, 0x78);
        if (!level || !g_menu || !g_quickName || !pCanSave || !oHandleEvent) return;
        if (At<float>(level, kTime) < g_startSaveAfter) return;
        void* profile = At<void*>(g_menu, 0x12C);
        void* pc = At<void*>(pawn, 0x2B4);
        if (!profile || !pc || !pCanSave(pc, nullptr)) return;
        const wchar_t* old = *g_quickName;
        *g_quickName = g_startName.c_str();
        g_allowSave = true;
        oHandleEvent(g_menu, nullptr, 0x1F, 0, nullptr);
        g_allowSave = false;
        *g_quickName = old;
        g_startSave = false;
        LOG("[CLUSTERFUCK] mission-start save made: %ls", g_startName.c_str());
    }

    const wchar_t* DamageName(int t)
    {
        static const wchar_t* const n[] = { L"gunfire", L"electrostatic damage", L"ambient damage", L"fire", L"electrocution",
                                            L"a blow", L"gas", L"a stun", L"a fall or crushing", L"a knife", L"sticky-cam gas" };
        return t >= 0 && t < 11 ? n[t] : L"unknown causes";
    }

    std::wstring Describe(void* killer, void* sam)
    {
        if (!killer) return L"something unseen";
        if (killer == sam) return L"himself";
        wchar_t chain[2][64] = {};
        std::wstring s = Game::ClassChain(killer, chain, 1) > 0 ? chain[0] : L"someone";
        if (const wchar_t* label = Guards::Label(killer)) s += std::wstring(L" (") + label + L")";
        return s;
    }

    // The profile's save list index of a file ("...\NAME.sav" <-> record "NAME"), -1 if none.
    int RecordIndex(void* profile, const std::wstring& path)
    {
        std::wstring base = path.substr(path.find_last_of(L"\\/") + 1);
        if (base.size() > 4 && !_wcsicmp(base.c_str() + base.size() - 4, L".sav")) base.resize(base.size() - 4);
        const uint8_t* recs = At<const uint8_t*>(profile, 0xAC);
        const int n = At<int>(profile, 0xB0);
        for (int i = 0; recs && i < n && i < 100000; ++i)
        {
            const wchar_t* name = *reinterpret_cast<const wchar_t* const*>(recs + i * 16);
            if (name && !_wcsicmp(name, base.c_str())) return i;
        }
        return -1;
    }

    bool DeleteThroughGame(const std::wstring& path)
    {
        if (!g_menu || !pGetEngine || !pDeleteSave) return false;
        void* profile = At<void*>(g_menu, 0x12C);
        void* engine = pGetEngine();
        void* mgr = engine ? At<void*>(engine, 0x1D4) : nullptr;
        if (!profile || !mgr) return false;
        bool any = false;
        for (int guard = 0; guard < 8; ++guard)
        {
            const int i = RecordIndex(profile, path);
            if (i < 0) break;
            pDeleteSave(mgr, nullptr, reinterpret_cast<uint8_t*>(g_menu) + 0x130, profile, i);
            any = true;
        }
        return any;
    }

    bool DeleteThroughGameSafe(const std::wstring& path)
    {
        __try { return DeleteThroughGame(path); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    struct Stats { int counts[9] = {}; };
    void ReadStats(void* pawn, Stats& st)
    {
        void* pc = At<void*>(pawn, 0x2B4);
        void* mm = pc ? At<void*>(pc, 0x5C0) : nullptr;
        const uint8_t* data = mm ? At<const uint8_t*>(mm, 0x88) : nullptr;
        const int n = mm ? At<int>(mm, 0x8C) : 0;
        for (int i = 0; data && i < n && i < 100000; ++i)
            if (data[i * 8] < 9) ++st.counts[data[i * 8]];
    }
    void ReadStatsSafe(void* pawn, Stats& st)
    {
        __try { ReadStats(pawn, st); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void Obituary(void* sam, void* killer, int damage, int type)
    {
        g_dead = true;
        const std::wstring sec = Section(g_active);
        void* level = At<void*>(sam, 0x78);
        const float t = level ? At<float>(level, kTime) : 0.0f;
        Stats st;
        ReadStatsSafe(sam, st);
        const std::wstring who = Describe(killer, sam);
        // The campaign's saves.
        std::vector<std::wstring> saves, deleted;
        const int ns = ReadInt(sec, L"Saves");
        for (int i = 1; i <= ns; ++i) saves.push_back(ReadStr(sec, (L"Save" + std::to_wstring(i)).c_str()));
        std::wstring folder;
        for (const auto& p : saves)
        {
            if (p.empty()) continue;
            if (folder.empty()) folder = p.substr(0, p.find_last_of(L"\\/") + 1);
            const bool viaGame = DeleteThroughGameSafe(p);
            const bool gone = DeleteFileW(p.c_str()) || GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES;
            if (viaGame || gone) deleted.push_back(p.substr(p.find_last_of(L"\\/") + 1));
            LOG("[CLUSTERFUCK] permadeath: %ls %s", p.c_str(), (viaGame || gone) ? "deleted" : "could NOT be deleted");
        }
        if (folder.empty()) folder = g_store.substr(0, g_store.find_last_of(L"\\/") + 1);
        // The missions he reached.
        std::wstring missions;
        const int nm = ReadInt(sec, L"Missions");
        for (int i = 1; i <= nm; ++i) missions += L"    " + std::to_wstring(i) + L". " + ReadStr(sec, (L"Mission" + std::to_wstring(i)).c_str()) + L"\r\n";
        std::vector<wchar_t> buf(8192);
        swprintf_s(buf.data(), buf.size(),
            L"                         REST IN PEACE\r\n"
            L"                          SAM FISHER\r\n"
            L"                 Third Echelon, Splinter Cell\r\n\r\n"
            L"Difficulty:   CLUSTERFUCK (campaign %ls)\r\n"
            L"Started:      %ls\r\n"
            L"Died:         %ls\r\n"
            L"Where:        %ls, %d:%02d into the mission\r\n"
            L"Killed by:    %ls, by %ls (%d damage)\r\n\r\n"
            L"How far he got: %d mission%ls\r\n%ls\r\n"
            L"His last mission:\r\n"
            L"    enemies killed %d, knocked out %d, injured %d\r\n"
            L"    civilians killed %d, knocked out %d, injured %d\r\n"
            L"    bodies found %d, times identified %d, alarms %d\r\n\r\n"
            L"%d save%ls of this campaign %ls deleted. There is no going back.\r\n",
            g_active.c_str(), ReadStr(sec, L"Started").c_str(), NowText().c_str(), g_map.c_str(), int(t) / 60, int(t) % 60,
            who.c_str(), DamageName(type), damage, nm, nm == 1 ? L"" : L"s", missions.c_str(),
            st.counts[2], st.counts[0], st.counts[1], st.counts[5], st.counts[3], st.counts[4], st.counts[6], st.counts[7], st.counts[8],
            int(deleted.size()), deleted.size() == 1 ? L"" : L"s", deleted.size() == 1 ? L"was" : L"were");
        std::wstring text = buf.data();
        if (!deleted.empty())
        {
            text += L"\r\nDeleted:\r\n";
            for (const auto& d : deleted) text += L"    " + d + L"\r\n";
        }
        const std::wstring rip = folder + L"REST IN PEACE - CLUSTERFUCK " + g_active + L".txt";
        if (FILE* f = _wfopen(rip.c_str(), L"wb"))
        {
            const int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string utf8(len > 0 ? len - 1 : 0, '\0');
            if (len > 1) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), len, nullptr, nullptr);
            fwrite(utf8.data(), 1, utf8.size(), f);
            fclose(f);
        }
        WriteStr(sec, L"Status", L"dead");
        WriteStr(sec, L"Died", NowText());
        WriteStr(sec, L"Obituary", rip);
        WriteInt(sec, L"Saves", 0);
        LOG("[CLUSTERFUCK] Sam died (%ls, %ls): campaign %ls is over; %d saves deleted; %ls", who.c_str(), DamageName(type),
            g_active.c_str(), int(deleted.size()), rip.c_str());
    }

    void ObituarySafe(void* sam, void* killer, int damage, int type)
    {
        __try { Obituary(sam, killer, damage, type); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_dead = true; LOG("[CLUSTERFUCK] permadeath failed part-way"); }
    }

    int HealthOf(void* actor)
    {
        __try { return At<int>(actor, kHealth); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
    }

    void __fastcall hkTakeDamage(void* actor, void* edx, int dmg, void* inst, float hx, float hy, float hz,
                                 float nx, float ny, float nz, float mx, float my, float mz, int type, int pill)
    {
        const bool watch = actor && actor == g_samPawn && !g_active.empty() && !g_dead;
        const int before = watch ? HealthOf(actor) : 0;
        oTakeDamage(actor, edx, dmg, inst, hx, hy, hz, nx, ny, nz, mx, my, mz, type, pill);
        if (watch && before > 0 && HealthOf(actor) <= 0) ObituarySafe(actor, inst, dmg, type & 0xFF);
    }

    void WorldTickSafe(void* sam)
    {
        __try { WorldTick(sam); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void StartSaveSafe(void* sam)
    {
        __try { TryStartSave(sam); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_startSave = false; }
    }
}

void Clusterfuck::OnPlayerTick(void* pawn)
{
    g_samPawn = pawn;
    if (g_active.empty() || !pawn) return;
    bool died = false;
    __try
    {
        At<int>(pawn, kInitialHealth) = kMaxHealth;
        int& hp = At<int>(pawn, kHealth);
        if (hp > kMaxHealth) hp = kMaxHealth;
        SabotageDefuse(pawn);
        died = !g_dead && hp <= 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (died) ObituarySafe(pawn, At<void*>(pawn, 0xCF4), 0, -1);   // a death TakeDamage didn't see
    AlarmTickSafe(pawn);
    WorldTickSafe(pawn);
    if (g_startSave) StartSaveSafe(pawn);
}

bool Clusterfuck::Active() { return !g_active.empty(); }
const wchar_t* Clusterfuck::CampaignId() { return g_active.c_str(); }

void Clusterfuck::OnMapLoad(const wchar_t* map, const wchar_t* options)
{
    const std::wstring m = map ? map : L"", o = Lower(options ? options : L"");
    if (m.empty() || Lower(m).find(L"menu") != std::wstring::npos) return;   // the menu keeps the campaign as it is
    g_map = m;
    g_dead = false;
    g_startSave = false;
    if (o.find(L"missiondiff=") == std::wstring::npos) return;   // a save: OnSaveLoaded decides
    // A new mission: a new game picked in the menu, or the next mission of the same campaign.
    if (g_pending == Pending::Clusterfuck) StartCampaign();
    else if (g_pending == Pending::Normal)
    {
        if (!g_active.empty()) LOG("[CLUSTERFUCK] a normal new game: the CLUSTERFUCK campaign is no longer active");
        g_active.clear();
    }
    g_pending = Pending::None;
    NoteMission(m);
    if (!g_active.empty())
    {
        LOG("[CLUSTERFUCK] mission %ls (campaign %ls)", m.c_str(), g_active.c_str());
        g_startSave = true;   // Ironman: the one save of this mission, as soon as saving is possible
        g_startSaveAfter = 1.0f;
        g_startName = L"CLUSTERFUCK " + g_active + L" " + m;
    }
}

void Clusterfuck::OnSaveLoaded(const wchar_t* path)
{
    if (!path) return;
    g_pending = Pending::None;
    g_dead = false;
    g_startSave = false;
    const std::wstring id = CampaignOfSave(path);
    if (id != g_active)
        LOG("[CLUSTERFUCK] %ls is %ls", path, id.empty() ? L"not a CLUSTERFUCK save" : (L"from CLUSTERFUCK campaign " + id).c_str());
    g_active = id;
    if (!g_active.empty() && ReadStr(Section(g_active), L"Status") == L"dead")
        LOG("[CLUSTERFUCK] warning: campaign %ls already died, yet this save of it still exists", g_active.c_str());
}

void Clusterfuck::OnSaveWritten(const wchar_t* path)
{
    if (!path || g_active.empty()) return;
    if (CampaignOfSave(path) == g_active) return;
    const std::wstring sec = Section(g_active);
    const int n = ReadInt(sec, L"Saves") + 1;
    WriteInt(sec, L"Saves", n);
    WriteStr(sec, (L"Save" + std::to_wstring(n)).c_str(), path);
    LOG("[CLUSTERFUCK] save %ls recorded for campaign %ls", path, g_active.c_str());
}

void Clusterfuck::Init()
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(g_self, self, MAX_PATH);
    g_store = self;
    g_store = g_store.substr(0, g_store.find_last_of(L"\\/") + 1) + L"SCCTRemaster_Campaigns.ini";
    if (!g_settings.clusterfuck)
    {
        LOG("Clusterfuck: off");
        return;
    }
    bool menu = false;
    if (const uintptr_t a = Mem::Find("difficulty list", "83 EC 0C 56 57 6A 01 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? 8B F1 8B 46 38"))
    {
        // AddItem: the first 'mov ecx,[esi+40h]; add esp,18h; push eax; call AddItem' in the builder.
        static const uint8_t seq[] = { 0x8B, 0x4E, 0x40, 0x83, 0xC4, 0x18, 0x50, 0xE8 };
        for (uintptr_t p = a; p < a + 0x200 && !pAddItem; ++p)
            if (!memcmp(reinterpret_cast<const void*>(p), seq, sizeof(seq)))
                pAddItem = reinterpret_cast<AddItem_t>(p + 12 + *reinterpret_cast<const int32_t*>(p + 8));
        if (pAddItem)
            menu = Mem::Detour("difficulty list", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkBuilder), reinterpret_cast<void**>(&oBuilder));
    }
    if (const uintptr_t a = Mem::Find("difficulty select", "51 8B CE E8 ?? ?? ?? ?? B0 01 8B 4E 14 5F 88 41 14"))
        menu = Mem::CallHook("difficulty select", a + 3, &OnSelect) && menu;
    LOG("Clusterfuck: menu entry %s; campaigns in %ls", menu ? "added (CLUSTERFUCK after EXPERT)" : "NOT added (menu code not found)",
        g_store.c_str());

    // Sam's side (each only acts while a CLUSTERFUCK campaign is active).
    int sam = 0;
    if (const uintptr_t a = Mem::Find("medkit available", "83 EC 3C 56 8B F1 8B 86 C8"))
        sam += Mem::Detour("medkit available", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkMedkitAvail), reinterpret_cast<void**>(&oMedkitAvail));
    if (const uintptr_t a = Mem::Find("pickup available", "56 8B F1 8B 46 6C 50 E8 ?? ?? ?? ?? 83 C4 04 85"))
        sam += Mem::Detour("pickup available", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkPickupAvail), reinterpret_cast<void**>(&oPickupAvail));
    if (const uintptr_t a = Mem::Find("setup player", "53 55 8B 6C 24 0C 33 DB 3B"))
        sam += Mem::Detour("setup player", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkSetupPlayer), reinterpret_cast<void**>(&oSetupPlayer));
    if (const uintptr_t a = Mem::Find("HUD update", "56 8B F1 8A 46 20 84 C0 57 0F 84"))
        sam += Mem::Detour("HUD update", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkHudTick), reinterpret_cast<void**>(&oHudTick));
    // Ironman and permadeath.
    int saves = 0;
    if (const uintptr_t a = Mem::Find("engine SaveGame", "83 EC 20 53 55 57 8B 7C"))
        saves += Mem::Detour("engine SaveGame", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkSaveGame), reinterpret_cast<void**>(&oSaveGame));
    if (const uintptr_t a = Mem::Find("menu HandleEvent", "81 EC 48 01 00 00 8B 84"))
        saves += Mem::Detour("menu HandleEvent", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkHandleEvent), reinterpret_cast<void**>(&oHandleEvent));
    if (const uintptr_t a = Mem::Find("save page confirm", "56 8B F1 8B 06 FF 50 40 84 C0 0F 84 81"))
        saves += Mem::Detour("save page confirm", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkSavePage), reinterpret_cast<void**>(&oSavePage));
    if (const uintptr_t a = Mem::Find("restart mission", "8B 44 24 04 83 EC 0C 85 C0 56 57 8B F9"))
        saves += Mem::Detour("restart mission", reinterpret_cast<void*>(a), reinterpret_cast<void*>(&hkRestart), reinterpret_cast<void**>(&oRestart));
    if (const uintptr_t a = Mem::Find("can save game call", "E8 ?? ?? ?? ?? 85 C0 0F 84 EC 00 00 00 8B 96"))
        pCanSave = reinterpret_cast<CanSave_t>(a + 5 + *reinterpret_cast<const int32_t*>(a + 1));
    if (const uintptr_t a = Mem::Find("get engine", "E8 ?? ?? ?? ?? 8B 40 34 C3 CC CC CC CC CC CC CC 56"))
        pGetEngine = reinterpret_cast<GetEngine_t>(a);
    if (const uintptr_t a = Mem::Find("delete save", "83 EC 18 53 55 8B 6C 24 28 8B 85"))
        pDeleteSave = reinterpret_cast<DeleteSave_t>(a);
    if (const uintptr_t a = Mem::Find("quicksave name", "83 EC 0C A1 ?? ?? ?? ?? 56 8B F1 50"))
        g_quickName = *reinterpret_cast<const wchar_t***>(a + 4);
    if (const uintptr_t a = Mem::Find("take damage call", "E8 ?? ?? ?? ?? 8B 86 CC 00 00 00 45"))
        saves += Mem::Detour("take damage", reinterpret_cast<void*>(a + 5 + *reinterpret_cast<const int32_t*>(a + 1)),
                             reinterpret_cast<void*>(&hkTakeDamage), reinterpret_cast<void**>(&oTakeDamage));
    LOG("Clusterfuck: Ironman/permadeath %d of 5 hooks; can-save %s, engine %s, delete-save %s, quicksave name %s", saves,
        pCanSave ? "ok" : "MISSING", pGetEngine ? "ok" : "MISSING", pDeleteSave ? "ok" : "MISSING", g_quickName ? "ok" : "MISSING");

    if (const uintptr_t a = Mem::Find("start alarm", "83 EC 18 53 55 8B E9 8B 85 D8 03 00 00 56 8B C8 57 D1 E9 BF 01 00 00 00"))
        pStartAlarm = reinterpret_cast<StartAlarm_t>(a);
    LOG("Clusterfuck: alarms %s (base alert at mission start, found bodies raise the alarm)", pStartAlarm ? "on" : "OFF (StartAlarm not found)");
    LOG("Clusterfuck: Sam %d of 4 hooks (no medkits, no ammo pickups, half ammo, no light/sound meters) + %d HP max", sam, kMaxHealth);
}
