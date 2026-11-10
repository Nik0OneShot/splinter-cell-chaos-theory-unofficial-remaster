// Guard personalities ([Gameplay] GuardPersonalities, tables in SCCTRemaster_Guards.ini) and equal detection
// on every difficulty ([Gameplay] DetectionBaseline).
//
// Every guard is described as faction-role-personality, e.g. "guerilla-technician-paranoid":
//  - faction: from the class name prefix (EPeoplesVoice... = guerilla, EDisplace... = displace, ...), so a
//    People's Voice soldier placed in any map, custom ones included, is still a guerilla;
//  - role: from the rest of the class name (Soldier, Technician, Sergent, Colonel, Elite, ...);
//  - personality: rolled per guard from weighted tables; the weights depend on the difficulty (good guards
//    are rare on the lowest difficulty and common on Expert). The roll is seeded from the guard's object
//    name (e.g. 01_Lighthouse.EPeoplesVoiceSoldier3), so a guard is the same person on every reload.
// Each layer multiplies the guard's own sight range, sight cone, perception (how bright Sam looks to him),
// search accuracy, gun accuracy and health, and can grant thermal vision or deafness.
// Guards are the classes derived from EInexperienced / EExperienced / EElite (the game's own tiers); named
// characters listed in [Exclude] keep their authored values.
//
// Applied once, on a guard's first tick. Savegames store the changed values, so a marker is left in the low
// mantissa bits of m_VisibilityAngleVertical (a change of about one part in 10^4 of a degree); a guard
// restored from a save already carries it and is not scaled again.
//
// EAIPawn fields (offsets computed from the script packages, checked against known natives): Health +420h
// (int), bNoHearing +748h bit 2000h, bUseThermalVision +748h bit 8000000h (checked by the sight native),
// fVisibilityDecreaseFactor +810h (multiplies Sam's visibility in the sight check at 0x10BB23C0),
// SearchAccuracy +818h, GunAccuracy/Min/Max +9C8h/+9CCh/+9D0h, m_VisibilityConeAngle +9E4h,
// m_VisibilityMaxDistance +9E8h, m_VisibilityAngleVertical +9ECh (the pawn sight native around 0x10B7F4E9
// compares distance with +9E8h and the view angle with +9E4h). Actor.Level +78h, LevelInfo difficulty byte
// +454h (1 lowest .. 3 Expert). (The first build used offsets 60h too low for fields after SearchAccuracy:
// eight vector properties had been counted as size 0. The sanity check refused to touch anything.)
//
// DetectionBaseline (1/2/3, 0 = original): the engine changes detection by difficulty in three places, all
// forced to the chosen difficulty's values:
//  - sight check (0x10BB23C0): Sam's visibility x0.75 / x1 / x1.33 and a 125 / 150 / 200 limit;
//  - awareness distance (0x10BA3620): 100-400 units on the middle difficulty, 300-600 on Expert.
#include "pch.h"
#include "features.h"

extern HMODULE g_self;

namespace
{
    constexpr uintptr_t kActorLevel = 0x78, kLevelDifficulty = 0x454;
    constexpr uintptr_t kHealth = 0x420, kFlags748 = 0x748, kPerception = 0x810, kSearch = 0x818;
    constexpr uintptr_t kGunAcc = 0x9C8, kGunAccMin = 0x9CC, kGunAccMax = 0x9D0;
    constexpr uintptr_t kCone = 0x9E4, kRange = 0x9E8, kConeVertical = 0x9EC;
    constexpr uint32_t kNoHearing = 0x2000, kThermal = 0x8000000;
    constexpr uint32_t kMarkerMask = 0xFFF, kMarker = 0xA5C;

    struct Traits
    {
        float sight = 1, cone = 1, perception = 1, search = 1, aim = 1, health = 1;
        float shadow = 1;              // how often he checks for Sam's shadow (Perception: shadow check rate)
        float hearing = 1;             // how far he hears noises (Perception: noise radius per guard)
        float thermal = 0, deaf = 0;   // chance 0..1
        // Reaction time on top of the human minimum ([Perception] ReactionMin), in seconds: a range rolled per guard.
        // Added up over faction, role and personality (not multiplied).
        float reactLo = 0, reactHi = 0;
        bool hasReact = false;
    };
    // Per guard, for other modules (Guards::ShadowSense / Guards::Label). Cleared on level change.
    struct GuardInfo { float shadow = 1, hearing = 1; std::wstring label; float reaction = -1; };
    std::unordered_map<void*, GuardInfo> g_info;
    struct Personality { std::wstring name; Traits t; float weight[3] = { 1, 1, 1 }; };

    std::wstring g_iniPath;
    std::vector<std::pair<std::wstring, std::wstring>> g_factions;   // class prefix -> faction
    std::vector<std::pair<std::wstring, std::wstring>> g_roles;      // class suffix -> role
    std::unordered_map<std::wstring, Traits> g_factionTraits, g_roleTraits;
    std::vector<Personality> g_personalities;
    std::vector<std::wstring> g_exclude;
    bool g_log = true;
    bool g_ready = false;

    std::unordered_map<uint64_t, bool> g_guardClass;   // class (ptr | name index) -> is a guard
    std::unordered_map<uint64_t, int> g_done;          // actor (ptr | name index) -> ticks seen, -1 = handled
    void* g_level = nullptr;
    DWORD g_levelStart = 0;
    int g_restoreDiff = -1;        // difficulty recovered from the savegame being loaded (-1 = none)
    bool g_levelLogged = false;
    // Personalities are applied once the guard has ticked this often and the level is this old: a savegame
    // is read and applied only after the guards' first tick, so applying earlier would roll with the wrong
    // difficulty and could be overwritten by the restore.
    constexpr int kApplyAfterTicks = 10;
    constexpr DWORD kApplyAfterMs = 400;

    template <class T> T& At(void* p, uintptr_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p) + off); }

    std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }
    std::wstring Trim(const std::wstring& s)
    {
        const auto a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) return L"";
        const auto b = s.find_last_not_of(L" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // key=value pairs of a section, comments (';' or '//') stripped.
    std::vector<std::pair<std::wstring, std::wstring>> Section(const wchar_t* name)
    {
        std::vector<wchar_t> buf(32768);
        const DWORD n = GetPrivateProfileSectionW(name, buf.data(), DWORD(buf.size()), g_iniPath.c_str());
        std::vector<std::pair<std::wstring, std::wstring>> out;
        for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1)
        {
            std::wstring line(p);
            if (const auto c = line.find(L';'); c != std::wstring::npos) line.resize(c);
            if (const auto c = line.find(L"//"); c != std::wstring::npos) line.resize(c);
            const auto eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            const std::wstring k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
            if (!k.empty()) out.emplace_back(k, v);
        }
        return out;
    }

    Traits ReadTraits(const std::vector<std::pair<std::wstring, std::wstring>>& kv)
    {
        Traits t;
        for (const auto& [k, v] : kv)
        {
            const float f = static_cast<float>(_wtof(v.c_str()));
            const std::wstring key = Lower(k);
            if (key == L"sight") t.sight = f;
            else if (key == L"cone") t.cone = f;
            else if (key == L"perception") t.perception = f;
            else if (key == L"search") t.search = f;
            else if (key == L"aim") t.aim = f;
            else if (key == L"health") t.health = f;
            else if (key == L"shadow") t.shadow = f;
            else if (key == L"hearing") t.hearing = f;
            else if (key == L"thermal") t.thermal = f;
            else if (key == L"deaf") t.deaf = f;
            else if (key == L"reaction")
            {
                float lo = 0, hi = 0;
                const int n = swscanf_s(v.c_str(), L"%f , %f", &lo, &hi);
                if (n >= 1) { t.reactLo = lo; t.reactHi = n >= 2 ? hi : lo; t.hasReact = true; }
            }
        }
        return t;
    }

    // Reaction times for inis written before they existed (an update keeps the player's ini): the defaults of the
    // shipped ini, by personality / role name.
    void DefaultReaction(const std::wstring& name, Traits& t, bool personality)
    {
        static const struct { const wchar_t* name; float lo, hi; } kPers[] = {
            { L"noob", 0.3f, 0.45f }, { L"lazy", 0.35f, 0.5f }, { L"average", 0.2f, 0.3f },
            { L"paranoid", 0.05f, 0.15f }, { L"sharpeye", 0.1f, 0.15f }, { L"veteran", 0.05f, 0.1f } };
        static const struct { const wchar_t* name; float lo, hi; } kRoles[] = {
            { L"technician", 0.05f, 0.05f }, { L"scientist", 0.15f, 0.15f }, { L"pilot", 0.05f, 0.05f } };
        if (t.hasReact) return;
        if (personality)
        {
            for (const auto& d : kPers)
                if (name == d.name) { t.reactLo = d.lo; t.reactHi = d.hi; t.hasReact = true; return; }
        }
        else
        {
            for (const auto& d : kRoles)
                if (name == d.name) { t.reactLo = d.lo; t.reactHi = d.hi; t.hasReact = true; return; }
        }
    }

    void Load()
    {
        std::vector<wchar_t> names(32768);
        const DWORD n = GetPrivateProfileSectionNamesW(names.data(), DWORD(names.size()), g_iniPath.c_str());
        for (const wchar_t* p = names.data(); p < names.data() + n && *p; p += wcslen(p) + 1)
        {
            const std::wstring sec(p), low = Lower(sec);
            if (low.rfind(L"faction.", 0) == 0)
                g_factionTraits[low.substr(8)] = ReadTraits(Section(p));
            else if (low.rfind(L"role.", 0) == 0)
            {
                Traits rt = ReadTraits(Section(p));
                DefaultReaction(low.substr(5), rt, false);
                g_roleTraits[low.substr(5)] = rt;
            }
            else if (low.rfind(L"personality.", 0) == 0)
            {
                Personality pers;
                pers.name = low.substr(12);
                const auto kv = Section(p);
                pers.t = ReadTraits(kv);
                DefaultReaction(pers.name, pers.t, true);
                for (const auto& [k, v] : kv)
                    if (Lower(k) == L"weight")
                        swscanf_s(v.c_str(), L"%f , %f , %f", &pers.weight[0], &pers.weight[1], &pers.weight[2]);
                g_personalities.push_back(pers);
            }
        }
        for (const auto& [k, v] : Section(L"Factions")) g_factions.emplace_back(Lower(k), Lower(v));
        for (const auto& [k, v] : Section(L"Roles")) g_roles.emplace_back(Lower(k), Lower(v));
        // Longest prefix / suffix first.
        auto longer = [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); };
        std::sort(g_factions.begin(), g_factions.end(), longer);
        std::sort(g_roles.begin(), g_roles.end(), longer);
        for (const auto& [k, v] : Section(L"Exclude"))
            if (Lower(k) == L"classes")
            {
                std::wstring list = v + L",", cur;
                for (wchar_t c : list)
                    if (c == L',') { if (!Trim(cur).empty()) g_exclude.push_back(Lower(Trim(cur))); cur.clear(); }
                    else cur += c;
            }
        for (const auto& [k, v] : Section(L"General"))
            if (Lower(k) == L"logguards") g_log = _wtoi(v.c_str()) != 0;
    }

    uint32_t Hash(const std::wstring& s, uint32_t salt)
    {
        uint32_t h = 2166136261u ^ salt;
        for (wchar_t c : s) { h ^= static_cast<uint32_t>(towlower(c)); h *= 16777619u; }
        h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
        return h;
    }
    float Unit(const std::wstring& s, uint32_t salt) { return float(Hash(s, salt) & 0xFFFFFF) / float(0x1000000); }

    bool SafeFullName(void* obj, wchar_t* buf)
    {
        __try { Game::GetFullName(obj, nullptr, buf); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Class chain names, most-derived first (empty on failure).
    std::vector<std::wstring> Chain(void* actor)
    {
        wchar_t names[24][64];
        const int n = Game::ClassChain(actor, names, 24);
        std::vector<std::wstring> out;
        for (int i = 0; i < n; ++i) out.emplace_back(names[i]);
        return out;
    }

    uint64_t Key(void* obj, void* cls)
    {
        return uint64_t(reinterpret_cast<uintptr_t>(obj)) | (uint64_t(uint32_t(At<int>(cls ? cls : obj, 0x20))) << 32);
    }

    bool IsGuardClass(void* actor, const std::vector<std::wstring>& chain)
    {
        if (chain.empty()) return false;
        if (std::find(g_exclude.begin(), g_exclude.end(), Lower(chain[0])) != g_exclude.end()) return false;
        for (const auto& c : chain)
            if (c == L"EInexperienced" || c == L"EExperienced" || c == L"EElite") return true;
        return false;
    }

    bool Sane(float v, float lo, float hi) { return v == v && v > lo && v <= hi; }

    // EAIPawn.DifficultyLevel (script, at spawn): Health and Gun/Min/MaxGunAccuracy x0.75 / x1 / x1.5 (the
    // Expert accuracies capped at 1). A guard initialised under another difficulty than the level ends up
    // with (savegame loads: the map initialises at difficulty 1 before the save is read) is corrected from
    // its class defaults (UClass::Defaults data +29Ch / count +2A0h). Only exact matches are corrected, so a
    // damaged guard or one a designer gave custom values is left alone.
    float DiffMul(int d) { return d == 1 ? 0.75f : d == 3 ? 1.5f : 1.0f; }
    float DiffAcc(float def, int d) { const float v = def * DiffMul(d); return d == 3 ? std::min(v, 1.0f) : v; }

    void FixDifficultyStats(void* pawn, int diff, const std::wstring& seed)
    {
        void* cls = At<void*>(pawn, 0x24);
        if (!cls)
            return;
        const uintptr_t data = At<uintptr_t>(cls, 0x29C);
        const int num = At<int>(cls, 0x2A0);
        if (!data || num < int(kGunAccMax + 4) || num > 0x100000)
            return;
        const int defH = *reinterpret_cast<const int*>(data + kHealth);
        const float defA = *reinterpret_cast<const float*>(data + kGunAcc);
        const float defMin = *reinterpret_cast<const float*>(data + kGunAccMin);
        const float defMax = *reinterpret_cast<const float*>(data + kGunAccMax);
        if (defH <= 0 || defH > 100000 || !(defA >= 0 && defA <= 2))
            return;
        int& health = At<int>(pawn, kHealth);
        float& acc = At<float>(pawn, kGunAcc);
        float& accMin = At<float>(pawn, kGunAccMin);
        float& accMax = At<float>(pawn, kGunAccMax);
        for (int d = 1; d <= 3; ++d)
        {
            if (d == diff)
                continue;
            if (health == static_cast<int>(float(defH) * DiffMul(d)) && std::fabs(acc - DiffAcc(defA, d)) < 1e-4f)
            {
                const int oldH = health;
                const float oldA = acc;
                health = static_cast<int>(float(defH) * DiffMul(diff));
                acc = DiffAcc(defA, diff);
                accMin = DiffAcc(defMin, diff);
                accMax = DiffAcc(defMax, diff);
                if (g_log)
                    LOG("[GUARD] %ls: had difficulty %d stats, corrected to difficulty %d (health %d->%d, aim %.2f->%.2f)",
                        seed.c_str(), d, diff, oldH, health, oldA, acc);
                return;
            }
        }
    }

    void Apply(void* pawn)
    {
        const auto chain = Chain(pawn);
        if (chain.empty()) return;
        const std::wstring cls = Lower(chain[0]);
        std::wstring faction = L"default", role = L"soldier";
        std::wstring rest = cls.size() > 1 ? cls.substr(1) : cls;   // without the leading 'e'
        for (const auto& [prefix, f] : g_factions)
            if (cls.rfind(prefix, 0) == 0) { faction = f; rest = cls.substr(prefix.size()); break; }
        for (const auto& [suffix, r] : g_roles)
            if (rest.size() >= suffix.size() && rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) == 0) { role = r; break; }

        wchar_t full[256] = L"";
        if (!SafeFullName(pawn, full)) return;
        std::wstring seed(full);
        if (const auto sp = seed.find(L' '); sp != std::wstring::npos) seed = seed.substr(sp + 1);

        void* level = At<void*>(pawn, kActorLevel);
        int diff = level ? At<uint8_t>(level, kLevelDifficulty) : 2;
        diff = std::clamp(diff, 1, 3);

        // Weighted personality roll. CLUSTERFUCK: only the dangerous ones (paranoid, sharp-eyed, veteran), and
        // every guard gets the elite role's traits on top of his own.
        const bool cf = Clusterfuck::Active();
        auto weight = [&](const Personality& p)
        {
            if (cf && p.name != L"paranoid" && p.name != L"sharpeye" && p.name != L"veteran") return 0.0f;
            return std::max(p.weight[diff - 1], 0.0f);
        };
        const Personality* pers = nullptr;
        float total = 0;
        for (const auto& p : g_personalities) total += weight(p);
        if (total > 0)
        {
            float r = Unit(seed, 0x51A7) * total;
            for (const auto& p : g_personalities)
            {
                r -= weight(p);
                if (r < 0) { pers = &p; break; }
            }
            if (!pers) pers = &g_personalities.back();
        }
        Traits t;
        auto mul = [&](const Traits& o)
        {
            t.sight *= o.sight; t.cone *= o.cone; t.perception *= o.perception; t.search *= o.search;
            t.aim *= o.aim; t.health *= o.health; t.thermal = std::max(t.thermal, o.thermal); t.deaf = std::max(t.deaf, o.deaf);
            t.shadow *= o.shadow;
            t.hearing *= o.hearing;
            if (o.hasReact) { t.reactLo += o.reactLo; t.reactHi += o.reactHi; t.hasReact = true; }
        };
        if (auto it = g_factionTraits.find(faction); it != g_factionTraits.end()) mul(it->second);
        if (auto it = g_roleTraits.find(role); it != g_roleTraits.end()) mul(it->second);
        if (cf && role != L"elite")
            if (auto it = g_roleTraits.find(L"elite"); it != g_roleTraits.end()) mul(it->second);
        if (pers) mul(pers->t);
        const std::wstring label = faction + L"-" + role + L"-" + (pers ? pers->name : L"none");
        t.shadow = std::clamp(t.shadow, 0.0f, 3.0f);
        t.hearing = std::clamp(t.hearing, 0.0f, 3.0f);
        // Reaction time above the minimum, rolled from the guard's name (the same every load).
        const float reaction = t.hasReact
            ? std::clamp(t.reactLo + (t.reactHi - t.reactLo) * Unit(seed, 0x2EAC), 0.0f, 5.0f) : -1.0f;
        char reactDesc[40] = "";
        if (reaction >= 0) snprintf(reactDesc, sizeof(reactDesc), " reaction +%.2f s", reaction);
        // Not stored in the guard, so it is recorded on every load (savegames included).
        g_info[pawn] = { t.shadow, t.hearing, label, reaction };

        float& vert = At<float>(pawn, kConeVertical);
        uint32_t vbits;
        std::memcpy(&vbits, &vert, 4);
        if ((vbits & kMarkerMask) == kMarker)
        {
            if (g_log)
                LOG("[GUARD] %ls: %ls (restored from a save, already applied; shadow sense x%.2f, hearing x%.2f%s)", seed.c_str(),
                    label.c_str(), t.shadow, t.hearing, reactDesc);
            return;
        }

        float& range = At<float>(pawn, kRange);
        float& cone = At<float>(pawn, kCone);
        float& perception = At<float>(pawn, kPerception);
        float& search = At<float>(pawn, kSearch);
        float& acc = At<float>(pawn, kGunAcc);
        float& accMin = At<float>(pawn, kGunAccMin);
        float& accMax = At<float>(pawn, kGunAccMax);
        int& health = At<int>(pawn, kHealth);
        uint32_t& flags = At<uint32_t>(pawn, kFlags748);
        if (!(Sane(range, 0, 200000) && Sane(cone, 0, 360) && Sane(vert, 0, 360) && Sane(perception, -0.001f, 100) &&
              Sane(search, -0.001f, 100) && Sane(acc, -0.001f, 2) && Sane(accMax, -0.001f, 2) && health > 0 && health < 100000))
        {
            LOG("[GUARD] %ls: unexpected values (range %.1f cone %.1f vert %.1f perception %.3f search %.3f aim %.3f health %d); skipped",
                seed.c_str(), range, cone, vert, perception, search, acc, health);
            return;
        }
        FixDifficultyStats(pawn, diff, seed);
        const float oRange = range, oCone = cone, oPerc = perception, oSearch = search, oAcc = acc;
        const int oHealth = health;
        range *= t.sight;
        cone = std::min(cone * t.cone, 360.0f);
        vert = std::min(vert * t.cone, 360.0f);
        perception *= t.perception;
        search *= t.search;
        acc = std::min(acc * t.aim, 1.0f);
        accMin = std::min(accMin * t.aim, 1.0f);
        accMax = std::min(accMax * t.aim, 1.0f);
        health = std::max(1, static_cast<int>(std::lround(health * t.health)));
        const bool thermal = t.thermal > 0 && Unit(seed, 0x7E41) < t.thermal;
        const bool deaf = t.deaf > 0 && Unit(seed, 0xDEAF) < t.deaf;
        if (thermal) flags |= kThermal;
        if (deaf) flags |= kNoHearing;
        std::memcpy(&vbits, &vert, 4);
        vbits = (vbits & ~kMarkerMask) | kMarker;
        std::memcpy(&vert, &vbits, 4);

        if (g_log)
            LOG("[GUARD] %ls: %ls | sight %.0f->%.0f cone %.1f->%.1f perception %.2f->%.2f search %.2f->%.2f aim %.2f->%.2f health %d->%d shadow sense x%.2f hearing x%.2f%s%s%s",
                seed.c_str(), label.c_str(), oRange, range, oCone, cone, oPerc, perception, oSearch, search, oAcc, acc, oHealth, health,
                t.shadow, t.hearing, reactDesc, thermal ? " +thermal" : "", deaf ? " +deaf" : "");
    }

    // ---- Solo difficulty fix ([Gameplay] FixSoloDifficulty) ------------------------------------------
    // The map loader (in UGameEngine::LoadMap, ~0x10A41FC5) sets LevelInfo.m_eMissionDifficulty (+454h) and
    // the mission mode (+455h) from the URL options MISSIONDIFF= / MISSIONMODE= (solo saves record
    // "MISSIONMODE=1 ... MISSIONDIFF=3" for Expert). Right after, if a network session object exists
    // (global at 0x111F55AC, settings struct at 0x111F5630: difficulty +460h, mode +468h), its values
    // overwrite the URL's. That session apparently survives into solo play (e.g. after visiting the co-op
    // menus) with its own difficulty, so a solo Expert mission could run on another difficulty (seen: the
    // guards got the lowest difficulty's health and aim in an Expert save). Hooked at the
    // 'mov eax,[ebx+0A8h]' that guards that block: in a solo mission (URL mode 1) the session is ignored.
    uintptr_t g_sessionValid = 0, g_sessionBase = 0;

    // FURL (UE2): options TArray<FString> at +28h (data) / +2Ch (count); FString = { wchar_t*, num, max }.
    // ULevel::URL is at ULevel +40h (after Actors TTransArray +28h, NetDriver, Engine).
    constexpr uintptr_t kUrlOps = 0x28, kLevelUrl = 0x40;

    // Copies "?opt1?opt2..." into buf (no C++ objects here: SEH-guarded).
    void UrlOptionsRaw(uintptr_t url, wchar_t* buf, size_t size)
    {
        size_t pos = 0;
        buf[0] = 0;
        __try
        {
            const uintptr_t data = *reinterpret_cast<const uintptr_t*>(url + kUrlOps);
            const int n = *reinterpret_cast<const int*>(url + kUrlOps + 4);
            for (int i = 0; i < n && i < 32 && data; ++i)
            {
                const wchar_t* str = *reinterpret_cast<wchar_t* const*>(data + i * 12);
                const int len = *reinterpret_cast<const int*>(data + i * 12 + 4);
                if (!str || len <= 0 || len > 255)
                    continue;
                if (pos + 2 >= size)
                    break;
                buf[pos++] = L'?';
                for (int c = 0; c < len && str[c] && pos + 1 < size; ++c)
                    buf[pos++] = str[c];
                buf[pos] = 0;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { buf[pos < size ? pos : size - 1] = 0; }
    }

    std::wstring UrlOptions(uintptr_t url)
    {
        wchar_t buf[1024];
        UrlOptionsRaw(url, buf, 1024);
        return buf;
    }

    // FURL.Map (FString at +1Ch).
    void UrlMapRaw(uintptr_t url, wchar_t* buf, size_t size)
    {
        buf[0] = 0;
        __try
        {
            const wchar_t* str = *reinterpret_cast<wchar_t* const*>(url + 0x1C);
            const int len = *reinterpret_cast<const int*>(url + 0x20);
            if (str && len > 0 && len < 512)
                wcsncpy_s(buf, size, str, _TRUNCATE);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = 0; }
    }

    // Savegames (*.sav) the game opens for reading, tracked through its CreateFileA/W imports.
    std::wstring g_lastSave;
    DWORD g_lastSaveTick = 0;
    uintptr_t g_pendingInfo = 0;   // LevelInfo of a map loaded without a difficulty, waiting for its savegame

    // MISSIONDIFF / MISSIONMODE / map from a savegame's "MAPNAME=<map>?MISSIONMODE=n?MISSIONDIFF=n" text.
    bool ReadSaveHeader(const std::wstring& path, int& diff, int& mode, std::wstring& map)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        std::string data;
        if (size.QuadPart > 0 && size.QuadPart < (64 << 20))
        {
            data.resize(static_cast<size_t>(size.QuadPart));
            DWORD got = 0;
            ReadFile(f, data.data(), DWORD(data.size()), &got, nullptr);
            data.resize(got);
        }
        CloseHandle(f);
        const auto p = data.rfind("MISSIONDIFF=");
        if (p == std::string::npos || p + 12 >= data.size())
            return false;
        diff = data[p + 12] - '0';
        const auto m = data.rfind("MISSIONMODE=");
        mode = (m != std::string::npos && m + 12 < data.size()) ? data[m + 12] - '0' : -1;
        map.clear();
        const auto n = data.rfind("MAPNAME=");
        if (n != std::string::npos)
            for (size_t i = n + 8; i < data.size() && i < n + 72 && data[i] != '?' && data[i] != ' ' && data[i] != 0; ++i)
                map += static_cast<wchar_t>(static_cast<unsigned char>(data[i]));
        return diff >= 1 && diff <= 3;
    }

    // ---- Save/load audit (Sam's state around saves) ----------------------------------------------------
    // ESam.PostBeginPlay sets Sam's Health by difficulty (450 / 300 / 150) and EPawn.PostBeginPlay copies it to
    // InitialHealth (+1530h, the maximum). A savegame load spawns Sam while the map still has difficulty 1.
    constexpr uintptr_t kInitialHealth = 0x1530;
    void* g_sam = nullptr;
    int g_samHealth = INT_MIN, g_samInitial = INT_MIN;

    // Inventory: EPawn.FullInventory +6DCh -> EInventory (UObject): InventoryList TArray<InvItemInfo> at +2Ch;
    // InvItemInfo = { TArray<EInventoryItem*> Items; int SlotId } (16 bytes). EInventoryItem: Quantity +40Ch,
    // PackQuantity +410h; EWeapon: AmmoPool { ClipAmmo, ClipCapacity, TotalAmmo } at +434h.
    // EchelonLevelInfo: AlarmStage +9CCh, AlarmModifier +9D0h.
    constexpr uintptr_t kFullInventory = 0x6DC, kInvList = 0x2C, kItemQuantity = 0x40C, kAmmoPool = 0x434;
    constexpr uintptr_t kAlarmStage = 0x738;   // LevelInfo.CurrentAlertLevel (-1 none, 3 base alert); +9CCh AlarmStage is unused
    struct ItemSnap { void* item; int quantity, clip, capacity, total; };

    int ReadInventory(void* sam, ItemSnap* out, int max)
    {
        int n = 0;
        __try
        {
            const uintptr_t inv = At<uintptr_t>(sam, kFullInventory);
            if (!inv) return 0;
            const uintptr_t slots = *reinterpret_cast<const uintptr_t*>(inv + kInvList);
            const int nslots = *reinterpret_cast<const int*>(inv + kInvList + 4);
            for (int s = 0; s < nslots && s < 64 && slots; ++s)
            {
                const uintptr_t items = *reinterpret_cast<const uintptr_t*>(slots + s * 16);
                const int nitems = *reinterpret_cast<const int*>(slots + s * 16 + 4);
                for (int i = 0; i < nitems && i < 64 && items && n < max; ++i)
                {
                    void* item = *reinterpret_cast<void* const*>(items + i * 4);
                    if (!item) continue;
                    ItemSnap& o = out[n++];
                    o.item = item;
                    o.quantity = At<int>(item, kItemQuantity);
                    o.clip = At<int>(item, kAmmoPool);
                    o.capacity = At<int>(item, kAmmoPool + 4);
                    o.total = At<int>(item, kAmmoPool + 8);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return n;
    }

    void LogSam(const char* when)
    {
        if (!g_sam)
            return;
        void* level = At<void*>(g_sam, kActorLevel);
        std::string line;
        char buf[160];
        sprintf_s(buf, "[SAM] %s: health %d / max %d, difficulty %d, alarms %d", when, At<int>(g_sam, kHealth),
                  At<int>(g_sam, kInitialHealth), level ? int(At<uint8_t>(level, kLevelDifficulty)) : -1,
                  level ? At<int>(level, kAlarmStage) : -1);
        line = buf;
        ItemSnap items[96];
        const int n = ReadInventory(g_sam, items, 96);
        for (int i = 0; i < n; ++i)
        {
            const auto chain = Chain(items[i].item);
            if (chain.empty()) continue;
            bool weapon = false;
            for (const auto& c : chain) weapon |= c == L"EWeapon";
            char name[64];
            WideCharToMultiByte(CP_UTF8, 0, chain[0].c_str(), -1, name, sizeof(name), nullptr, nullptr);
            if (weapon)
                sprintf_s(buf, " | %s x%d ammo %d/%d +%d", name, items[i].quantity, items[i].clip, items[i].capacity, items[i].total);
            else
                sprintf_s(buf, " | %s x%d", name, items[i].quantity);
            line += buf;
        }
        LOG("%s", line.c_str());
    }

    void OnSaveOpened(const std::wstring& path, DWORD access)
    {
        if (path.size() < 4 || _wcsicmp(path.c_str() + path.size() - 4, L".sav") != 0)
            return;
        if (access & GENERIC_WRITE)
        {
            if (g_settings.saveAudit)
            {
                LOG("Difficulty: savegame opened for writing: %ls", path.c_str());
                LogSam("when saving");
            }
            return;
        }
        if (!(access & GENERIC_READ))
            return;
        g_lastSave = path;
        g_lastSaveTick = GetTickCount();
        if (g_pendingInfo)
        {
            LOG("Difficulty: loading savegame %ls", path.c_str());
            Clusterfuck::OnSaveLoaded(path.c_str());
        }
        if (g_pendingInfo)
        {
            int d = -1, m = -1;
            std::wstring map;
            if (ReadSaveHeader(path, d, m, map))
            {
                const int cur = *reinterpret_cast<const uint8_t*>(g_pendingInfo + kLevelDifficulty);
                LOG("Difficulty: it was started with MISSIONDIFF=%d (level has %d)%s", d, cur, d != cur ? ", restored" : "");
                *reinterpret_cast<uint8_t*>(g_pendingInfo + kLevelDifficulty) = static_cast<uint8_t>(d);
                g_restoreDiff = d;
                if (m >= 1 && m <= 3)
                    *reinterpret_cast<uint8_t*>(g_pendingInfo + kLevelDifficulty + 1) = static_cast<uint8_t>(m);
            }
            g_pendingInfo = 0;
        }
    }

    using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    CreateFileW_t oCreateFileW = nullptr;
    CreateFileA_t oCreateFileA = nullptr;

    HANDLE WINAPI hkCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
    {
        if (name)
            OnSaveOpened(name, access);
        return oCreateFileW(name, access, share, sa, disp, flags, tmpl);
    }

    HANDLE WINAPI hkCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
    {
        if (name)
        {
            wchar_t wide[MAX_PATH * 2];
            if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 2))
                OnSaveOpened(wide, access);
        }
        return oCreateFileA(name, access, share, sa, disp, flags, tmpl);
    }

    // Finds MISSIONDIFF=n / MISSIONMODE=n in a savegame file (it stores the URL its mission started with).
    bool DifficultyFromSave(const std::wstring& map, int& diff, int& mode, std::wstring& file)
    {
        std::vector<std::wstring> candidates{ map, map + L".sav" };
        wchar_t profiles[MAX_PATH];
        if (GetEnvironmentVariableW(L"ProgramData", profiles, MAX_PATH))
        {
            const std::wstring base = std::wstring(profiles) + L"\\Ubisoft\\Tom Clancy's Splinter Cell Chaos Theory\\Profiles\\";
            const auto slash = map.find_last_of(L"\\/");
            const std::wstring name = slash == std::wstring::npos ? map : map.substr(slash + 1);
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((base + L"*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
                    {
                        candidates.push_back(base + fd.cFileName + L"\\Save\\" + name);
                        candidates.push_back(base + fd.cFileName + L"\\Save\\" + name + L".sav");
                    }
                while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        for (const auto& c : candidates)
        {
            HANDLE f = CreateFileW(c.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE)
                continue;
            LARGE_INTEGER size{};
            GetFileSizeEx(f, &size);
            std::string data;
            if (size.QuadPart > 0 && size.QuadPart < (64 << 20))
            {
                data.resize(static_cast<size_t>(size.QuadPart));
                DWORD got = 0;
                ReadFile(f, data.data(), DWORD(data.size()), &got, nullptr);
                data.resize(got);
            }
            CloseHandle(f);
            const auto p = data.rfind("MISSIONDIFF=");
            if (p == std::string::npos || p + 12 >= data.size())
                continue;
            diff = data[p + 12] - '0';
            const auto m = data.rfind("MISSIONMODE=");
            mode = (m != std::string::npos && m + 12 < data.size()) ? data[m + 12] - '0' : -1;
            file = c;
            return diff >= 1 && diff <= 3;
        }
        return false;
    }

    int UrlInt(const std::wstring& opts, const wchar_t* key)
    {
        std::wstring low = Lower(opts), k = L"?" + Lower(key) + L"=";
        const auto p = low.find(k);
        return p == std::wstring::npos ? -1 : _wtoi(low.c_str() + p + k.size());
    }

    void __cdecl OnSessionOverride(Mem::Regs* r)
    {
        const uint32_t field = *reinterpret_cast<const uint32_t*>(r->ebx + 0xA8);
        r->eax = field;   // emulates mov eax,[ebx+0A8h]
        const uintptr_t level = *reinterpret_cast<const uintptr_t*>(r->ebx + 0x17C);
        const uintptr_t actors = level ? *reinterpret_cast<const uintptr_t*>(level + 0x28) : 0;
        const uintptr_t info = actors ? *reinterpret_cast<const uintptr_t*>(actors) : 0;
        if (!info)
            return;
        g_restoreDiff = -1;   // per map load
        g_pendingInfo = 0;
        // URL this map is being loaded with (LoadMap's first argument) and the URL stored in the level itself
        // (a savegame keeps the URL its mission was started with).
        const uintptr_t loadUrl = *reinterpret_cast<const uintptr_t*>(r->ebp + 8);
        const std::wstring loadOpts = loadUrl ? UrlOptions(loadUrl) : L"";
        const std::wstring levelOpts = UrlOptions(level + kLevelUrl);
        wchar_t loadMap[512] = L"", levelMap[512] = L"";
        if (loadUrl) UrlMapRaw(loadUrl, loadMap, 512);
        UrlMapRaw(level + kLevelUrl, levelMap, 512);
        LOG("Difficulty: loading '%ls' with options '%ls'; level's own URL '%ls' options '%ls'", loadMap, loadOpts.c_str(),
            levelMap, levelOpts.c_str());
        Clusterfuck::OnMapLoad(loadMap, loadOpts.c_str());
        Scripting::OnMapLoad(loadMap, loadOpts.c_str());
        // A save loads without MISSIONDIFF: restore the difficulty its mission was started with.
        if (g_settings.fixSoloDifficulty && UrlInt(loadOpts, L"MISSIONDIFF") < 0)
        {
            int saved = UrlInt(levelOpts, L"MISSIONDIFF");
            int savedMode = UrlInt(levelOpts, L"MISSIONMODE");
            std::wstring file;
            const bool mission = loadMap[0] && Lower(loadMap).find(L"menu") == std::wstring::npos;
            if (saved < 1 && mission && DifficultyFromSave(loadMap, saved, savedMode, file))
                LOG("Difficulty: %ls was started with MISSIONDIFF=%d", file.c_str(), saved);
            // A savegame read just before this map load (Continue / Load Game) for the same map.
            if (saved < 1 && mission && !g_lastSave.empty() && GetTickCount() - g_lastSaveTick < 120000)
            {
                int d = -1, m = -1;
                std::wstring map;
                if (ReadSaveHeader(g_lastSave, d, m, map) && (map.empty() || !_wcsicmp(map.c_str(), loadMap)))
                {
                    saved = d;
                    savedMode = m;
                    LOG("Difficulty: savegame %ls (read before this load) was started with MISSIONDIFF=%d", g_lastSave.c_str(), d);
                }
            }
            // The savegame actually being loaded is read during this load; it has the final say (the guess above
            // comes from the last savegame the menu read, and the Load Game list reads all of them).
            g_pendingInfo = mission ? info : 0;
            g_restoreDiff = (saved >= 1 && saved <= 3) ? saved : -1;
            if (saved >= 1 && saved <= 3 && saved != *reinterpret_cast<const uint8_t*>(info + kLevelDifficulty))
            {
                LOG("Difficulty: restored difficulty %d from the level's URL (was %d)", saved,
                    int(*reinterpret_cast<const uint8_t*>(info + kLevelDifficulty)));
                *reinterpret_cast<uint8_t*>(info + kLevelDifficulty) = static_cast<uint8_t>(saved);
            }
            if (savedMode >= 1 && savedMode <= 3)
                *reinterpret_cast<uint8_t*>(info + kLevelDifficulty + 1) = static_cast<uint8_t>(savedMode);
        }
        const int urlDiff = *reinterpret_cast<const uint8_t*>(info + kLevelDifficulty);
        const int urlMode = *reinterpret_cast<const uint8_t*>(info + kLevelDifficulty + 1);
        const bool session = *reinterpret_cast<const uint32_t*>(g_sessionValid) != 0;
        const int sDiff = *reinterpret_cast<const int*>(g_sessionBase + 0x460);
        const int sMode = *reinterpret_cast<const int*>(g_sessionBase + 0x468);
        const bool wouldOverride = field && session && ((sDiff >= 1 && sDiff <= 3 && sDiff != urlDiff) || (sMode >= 1 && sMode <= 3 && sMode != urlMode));
        const bool keep = wouldOverride && urlMode == 1 && g_settings.fixSoloDifficulty;
        LOG("Difficulty: level difficulty %d, mode %d; network session %s (difficulty %d, mode %d)%s", urlDiff, urlMode,
            session ? "present" : "none", sDiff, sMode,
            keep ? " -> session ignored, solo difficulty kept" : wouldOverride ? " -> session overrides the map" : "");
        if (keep)
            r->eax = 0;   // test eax,eax / jz past the session block
    }

    // Detection baseline patches: each table is (pattern, offset, bytes for baseline 1, 2, 3).
    struct Patch { const char* name; const char* pattern; size_t offset; std::vector<uint8_t> bytes[3]; };

    void PatchDetection(int baseline)
    {
        const Patch patches[] = {
            // sight check limit: dec eax; jz lowest(125); sub eax,2; jz expert(200); fld 150
            { "sight limit", "8B 56 78 0F B6 82 54 04 00 00 48 74 15 83 E8 02 74 08 D9 05", 0x0B,
              { { 0xEB, 0x15, 0x83, 0xE8, 0x02, 0x74, 0x08 },     // always lowest
                { 0x90, 0x90, 0x83, 0xE8, 0x02, 0x90, 0x90 },     // always middle
                { 0x90, 0x90, 0x83, 0xE8, 0x02, 0xEB, 0x08 } } }, // always Expert
            // visibility multiplier: jz lowest(x0.75); sub eax,2; jnz none; x1.33
            { "visibility multiplier", "8B 56 78 D8 8C 24 9C 00 00 00 0F B6 82 54 04 00 00 48 D8 8D 10 08 00 00 D9 5C 24 10 74 11 83 E8 02 75 1A", 0x1C,
              { { 0xEB, 0x11 }, { 0xEB, 0x1F }, { 0xEB, 0x05 } } },
            // awareness distance: dec; jz lowest; dec; jz middle; dec; mov al,..; jz expert
            { "awareness distance", "8B 46 78 0F B6 80 54 04 00 00 48 0F 84 A9 00 00 00 48 0F 84 80 00 00 00 48 8A 81 10 04 00 00 74 31", 0x0B,
              { { 0xE9, 0xAA, 0x00, 0x00, 0x00, 0x90 },
                { 0xE9, 0x88, 0x00, 0x00, 0x00, 0x90 },
                { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x8A, 0x81, 0x10, 0x04, 0x00, 0x00, 0xEB, 0x31 } } },
        };
        int ok = 0;
        for (const auto& p : patches)
            if (const uintptr_t a = Mem::Find(p.name, p.pattern))
            {
                const auto& b = p.bytes[baseline - 1];
                Mem::WriteBytes(a + p.offset, b.data(), b.size());
                ++ok;
            }
        LOG("Guards: detection uses the %s difficulty's values on every difficulty (%d/3 patches)",
            baseline == 1 ? "lowest" : baseline == 2 ? "middle" : "Expert", ok);
    }

    // ---- Stuck-in-mid-fall rescue ([Gameplay] RescueStuckNPCs) -----------------------------------------
    // A character knocked outside the level stays in PHYS_Falling (Physics +4Ch = 2) without ever dropping (no
    // gravity out there); he keeps ticking and can even walk about in mid-air. Seen in Lighthouse: the mission script's performance mover
    // 'WhateverIWant' (Mover7, carrying the lighthouse light's EShaftFan) drops 6917 units in 0.1 s, and the
    // guard on top of the lighthouse (tag HighFaller), if he is walking around up there, ends up hanging in the
    // air over the sea for the rest of the mission (the mission's finish check needs him taken out).
    // Each AI pawn's last standing spot (PHYS_Walking with a Base +A8h) is recorded; one that has been "falling"
    // for 3 s without dropping 64 units (he can still walk about: no gravity out there) is put back there with ULevel::FarMoveActor (vtable +88h), or, if
    // he was never seen standing (loaded already stuck), where the map file placed him (OriginalLocation), at his
    // navigation Anchor (+2E4h) or his controller's home actor (EAIController +32Ch).
    struct V3 { float x, y, z; };
    struct RescueState { V3 good{}; bool hasGood = false, noted = false; DWORD goodTick = 0, fallTick = 0, lastCheck = 0; V3 fallAt{}; int rescues = 0;
                         V3 lastPos{}; DWORD movedTick = 0, doorFixTick = 0; bool tracked = false; };
    std::unordered_map<void*, RescueState> g_rescue;
    void* g_rescueLevel = nullptr;

    using FarMoveActor_t = int(__fastcall*)(void* level, void* edx, void* actor, float x, float y, float z,
                                            int bTest, int bNoCheck, int bAttachedMove);

    int SafeFarMove(void* level, void* actor, const V3& p)
    {
        __try
        {
            const auto f = reinterpret_cast<FarMoveActor_t>((*reinterpret_cast<void***>(level))[0x88 / 4]);
            return f(level, nullptr, actor, p.x, p.y, p.z, 0, 0, 0) ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    // Location of the actor 'holder' points to at 'off' (SEH-guarded: the pointer comes from script data).
    bool SafeActorLocation(void* holder, uintptr_t off, V3& out)
    {
        __try
        {
            void* a = At<void*>(holder, off);
            if (!a) return false;
            out = At<V3>(a, 0xE8);
            return out.x == out.x && std::fabs(out.x) < 1e6f && std::fabs(out.y) < 1e6f && std::fabs(out.z) < 1e6f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    float Dist(const V3& a, const V3& b)
    {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    }

    // Where the level designer placed an actor, read from the map package on disk (Data\Maps\<map>.scl, ~4 MB,
    // loaded once per map when first needed): the export with the actor's name and, in its data, its Location
    // property tag (name 'Location', info 3Ah = a 12-byte struct, struct name 'Vector', then 3 floats).
    // UE2 package v100 (licensee 124): header name count/offset +10h/+14h, export count/offset +18h/+1Ch; a name
    // is a compact-index length, the bytes, a 0 and a flags dword; an export is class and super (compact index),
    // package (int32), name (compact index), flags (dword), size (compact index) and, if size > 0, offset.
    struct MapPackage
    {
        std::wstring map;
        bool ok = false;
        std::vector<uint8_t> d;
        std::vector<std::string> names;
        struct Export { int name, size, off; };
        std::vector<Export> exports;
    };
    MapPackage g_mapPkg;

    bool ReadCI(const std::vector<uint8_t>& d, size_t& p, int& out)
    {
        if (p >= d.size()) return false;
        const uint8_t b0 = d[p++];
        int v = b0 & 0x3F;
        if (b0 & 0x40)
            for (int sh = 6; ; sh += 7)
            {
                if (p >= d.size()) return false;
                const uint8_t b = d[p++];
                v |= (b & 0x7F) << sh;
                if (!(b & 0x80) || sh > 27) break;
            }
        out = (b0 & 0x80) ? -v : v;
        return true;
    }

    void WriteCI(std::vector<uint8_t>& out, int v)
    {
        unsigned a = static_cast<unsigned>(v < 0 ? -v : v);
        uint8_t b0 = a & 0x3F;
        a >>= 6;
        if (a) b0 |= 0x40;
        if (v < 0) b0 |= 0x80;
        out.push_back(b0);
        while (a)
        {
            uint8_t b = a & 0x7F;
            a >>= 7;
            if (a) b |= 0x80;
            out.push_back(b);
        }
    }

    bool LoadMapPackage(const std::wstring& map)
    {
        if (g_mapPkg.map == map) return g_mapPkg.ok;
        g_mapPkg = {};
        g_mapPkg.map = map;
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"..\\Data\\Maps\\" + map + L".scl";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size <= 0x40 || size > 256 * 1024 * 1024) { fclose(f); return false; }
        auto& d = g_mapPkg.d;
        d.resize(static_cast<size_t>(size));
        const size_t got = fread(d.data(), 1, d.size(), f);
        fclose(f);
        if (got != d.size()) return false;
        auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &d[o], 4); return v; };
        if (u32(0) != 0x9E2A83C1u) return false;
        const uint32_t nameCount = u32(0x10), nameOff = u32(0x14), expCount = u32(0x18), expOff = u32(0x1C);
        if (nameCount > 200000 || expCount > 200000 || nameOff >= d.size() || expOff >= d.size()) return false;
        size_t p = nameOff;
        for (uint32_t i = 0; i < nameCount; ++i)
        {
            int len = 0;
            if (!ReadCI(d, p, len) || len < 0 || p + len + 5 > d.size()) return false;
            g_mapPkg.names.emplace_back(reinterpret_cast<const char*>(&d[p]), static_cast<size_t>(len));
            p += len + 1 + 4;
        }
        p = expOff;
        for (uint32_t i = 0; i < expCount; ++i)
        {
            int cls, sup, name, size2, off = 0;
            if (!ReadCI(d, p, cls) || !ReadCI(d, p, sup)) return false;
            p += 4;
            if (!ReadCI(d, p, name)) return false;
            p += 4;
            if (!ReadCI(d, p, size2)) return false;
            if (size2 > 0 && !ReadCI(d, p, off)) return false;
            g_mapPkg.exports.push_back({ name, size2, off });
        }
        g_mapPkg.ok = true;
        return true;
    }

    int NameIndex(const char* s)
    {
        for (size_t i = 0; i < g_mapPkg.names.size(); ++i)
            if (_stricmp(g_mapPkg.names[i].c_str(), s) == 0) return static_cast<int>(i);
        return -1;
    }

    // fullName: "Class Package.Object" from GetFullName.
    bool OriginalLocation(const wchar_t* fullName, V3& out)
    {
        const wchar_t* path = wcschr(fullName, L' ');
        path = path ? path + 1 : fullName;
        const wchar_t* dot = wcschr(path, L'.');
        const wchar_t* last = wcsrchr(path, L'.');
        if (!dot || !last) return false;
        const std::wstring map(path, dot);
        char object[128];
        if (WideCharToMultiByte(CP_ACP, 0, last + 1, -1, object, sizeof(object), nullptr, nullptr) <= 0) return false;
        if (!LoadMapPackage(map)) return false;
        const int obj = NameIndex(object), loc = NameIndex("Location"), vec = NameIndex("Vector");
        if (obj < 0 || loc < 0 || vec < 0) return false;
        std::vector<uint8_t> tag;
        WriteCI(tag, loc);
        tag.push_back(0x3A);
        WriteCI(tag, vec);
        const auto& d = g_mapPkg.d;
        for (const auto& e : g_mapPkg.exports)
        {
            if (e.name != obj || e.size <= 0 || e.off <= 0 || static_cast<size_t>(e.off) + e.size > d.size()) continue;
            const auto begin = d.begin() + e.off, end = begin + e.size;
            const auto it = std::search(begin, end, tag.begin(), tag.end());
            if (it == end || it + tag.size() + 12 > end) continue;
            std::memcpy(&out, &*(it + tag.size()), 12);
            return out.x == out.x && std::fabs(out.x) < 1e6f && std::fabs(out.y) < 1e6f && std::fabs(out.z) < 1e6f;
        }
        return false;
    }

    void Rescue(void* pawn);
    void DoorFix(void* pawn);

    // Run from the player's tick, so it doesn't depend on the stuck character's own tick (or on the logic-rate
    // throttle): every 250 ms, every other pawn (Actor.bIsPawn +2Ch bit 10h, not the player bit 20h) with a
    // controller (+2B4h) is checked.
    void RescueScan(void* sam)
    {
        static DWORD last = 0;
        const DWORD now = GetTickCount();
        if (now - last < 250) return;
        last = now;
        void* xlevel = At<void*>(sam, 0x7C);
        if (!xlevel) return;
        if (xlevel != g_rescueLevel)
        {
            g_rescueLevel = xlevel;
            g_rescue.clear();
        }
        void** actors = At<void**>(xlevel, 0x28);
        const int n = At<int>(xlevel, 0x2C);
        if (!actors || n <= 0 || n > 500000) return;
        for (int i = 0; i < n; ++i)
        {
            void* a = actors[i];
            if (!a || a == sam || (At<uint32_t>(a, 0x28) & 4)) continue;
            const uint32_t f = At<uint32_t>(a, 0x2C);
            if (!(f & 0x10) || (f & 0x20) || !At<void*>(a, 0x2B4)) continue;
            if (g_settings.rescueStuckNPCs) Rescue(a);
            if (g_settings.fixStuckDoors) DoorFix(a);
        }
    }

    // ---- Stuck-at-door fix ([Gameplay] FixStuckDoors) --------------------------------------------------
    // EAIController.s_OpenDoor.NotifyAction (the moment the guard's hand reaches the door) sets bDoorOpen and, if
    // the door (baseDoor +814h) isn't DST_Closed (EBaseDoor.eStatus +41Ch: 0 closed, 1 opened, 2 opening,
    // 3 closing), assumes it's being opened (bUseDoorOpener) and walks on without opening it. A door that was
    // still closing then shuts and he walks into it for good (seen: a guard behind a closed sliding door with
    // "will use, opener, door open"). Fix: a walking guard (Physics 1) whose controller has bDoorOpen (+86Ch bit
    // 20h) while his door is closed, within 150 units of it and not having moved 24 units for 1.5 s, gets the door
    // opened the normal way: the door's script Trigger(Other, EventInstigator) (EBaseDoor.s_Interpolating:
    // CanDoTrigger, so locks still apply, then 'Open'), called through Script::Call.
    int g_nameTrigger = -1;   // FName "Trigger": -1 = not looked up yet, 0 = not found

    bool CallTrigger(void* door, void* pawn)
    {
        if (g_nameTrigger < 0) g_nameTrigger = Script::Name(L"Trigger");
        struct { void* other; void* instigator; int eventId; int spare[5]; } parms{ pawn, pawn, 0, {} };
        return g_nameTrigger && Script::Call(door, g_nameTrigger, &parms);
    }

    void DoorFix(void* pawn)
    {
        void* ctrl = At<void*>(pawn, 0x2B4);
        if (!ctrl || At<void*>(ctrl, 0x41C) != pawn) return;   // EAIController
        const DWORD now = GetTickCount();
        RescueState& s = g_rescue[pawn];
        const V3 loc = At<V3>(pawn, 0xE8);
        const float dx = loc.x - s.lastPos.x, dy = loc.y - s.lastPos.y;
        if (!s.tracked || dx * dx + dy * dy > 24.0f * 24.0f) { s.lastPos = loc; s.movedTick = now; s.tracked = true; return; }
        if (At<uint8_t>(pawn, 0x4C) != 1 || !(At<uint32_t>(ctrl, 0x86C) & 0x20)) return;
        void* door = At<void*>(ctrl, 0x814);
        if (!door || At<uint8_t>(door, 0x41C) != 0) return;   // not DST_Closed
        if (now - s.movedTick < 1500 || now - s.doorFixTick < 5000) return;
        const V3 dl = At<V3>(door, 0xE8);
        if (Dist(loc, dl) > 150.0f) return;
        s.doorFixTick = now;
        const bool ok = CallTrigger(door, pawn);
        wchar_t pn[256] = L"", dn[256] = L"";
        SafeFullName(pawn, pn);
        SafeFullName(door, dn);
        LOG("[GUARD] %ls: stuck %.1f s against %ls, which his AI thinks is open but is closed; %s", wcschr(pn, L' ') ? wcschr(pn, L' ') + 1 : pn,
            (now - s.movedTick) / 1000.0f, wcschr(dn, L' ') ? wcschr(dn, L' ') + 1 : dn,
            ok ? "triggered it to open for him (it stays shut if locked)" : "couldn't call the door");
    }

    void Rescue(void* pawn)
    {
        void* xlevel = At<void*>(pawn, 0x7C);
        if (!xlevel) return;
        if (xlevel != g_rescueLevel)
        {
            g_rescueLevel = xlevel;
            g_rescue.clear();
        }
        const DWORD now = GetTickCount();
        RescueState& s = g_rescue[pawn];
        if (now - s.lastCheck < 100) return;
        s.lastCheck = now;
        const uint8_t phys = At<uint8_t>(pawn, 0x4C);
        const V3 loc = At<V3>(pawn, 0xE8);
        if (phys == 1 && At<void*>(pawn, 0xA8))
        {
            if (!s.hasGood || now - s.goodTick > 500)
            {
                s.good = loc;
                s.hasGood = true;
                s.goodTick = now;
            }
            s.fallTick = 0;
            return;
        }
        if (phys != 2)
        {
            s.fallTick = 0;
            return;
        }
        // A real fall drops quickly; a character "falling" where there is no gravity can still walk around
        // (air control), so only the height counts: 3 s in PHYS_Falling without dropping 64 units.
        if (!s.fallTick || s.fallAt.z - loc.z > 64.0f || loc.z - s.fallAt.z > 256.0f)
        {
            s.fallTick = now;
            s.fallAt = loc;
            return;
        }
        if (!s.noted && now - s.fallTick >= 1000)
        {
            s.noted = true;
            wchar_t nm[256] = L"";
            SafeFullName(pawn, nm);
            LOG("[GUARD] %ls: \"falling\" without dropping at (%.0f %.0f %.0f); rescuing after 3 s", wcschr(nm, L' ') ? wcschr(nm, L' ') + 1 : nm,
                loc.x, loc.y, loc.z);
        }
        if (now - s.fallTick < 3000 || s.rescues >= 5) return;
        s.fallTick = now;   // try again in 3 s at the earliest

        wchar_t full[256] = L"";
        SafeFullName(pawn, full);
        const wchar_t* name = wcschr(full, L' ') ? wcschr(full, L' ') + 1 : full;

        V3 spots[4];
        const char* what[4];
        int n = 0;
        if (s.hasGood) { spots[n] = s.good; what[n++] = "where he last stood"; }
        if (OriginalLocation(full, spots[n]) && Dist(spots[n], loc) > 100.0f) what[n++] = "where the map placed him";
        if (SafeActorLocation(pawn, 0x2E4, spots[n])) what[n++] = "his navigation anchor";
        if (void* ctrl = At<void*>(pawn, 0x2B4))
            if (SafeActorLocation(ctrl, 0x32C, spots[n])) what[n++] = "his home point";
        for (int i = 0; i < n; ++i)
            for (float up : { 8.0f, 48.0f, 96.0f })
            {
                const V3 p{ spots[i].x, spots[i].y, spots[i].z + up };
                const int r = SafeFarMove(xlevel, pawn, p);
                if (r < 0)
                {
                    s.rescues = 99;
                    LOG("[GUARD] %ls: stuck in mid-fall, but moving him failed; rescue off for him", name);
                    return;
                }
                if (r == 1)
                {
                    At<V3>(pawn, 0x100) = { 0, 0, 0 };   // Velocity
                    ++s.rescues;
                    LOG("[GUARD] %ls: stuck in mid-air at (%.0f %.0f %.0f) for 3 s (knocked out of the level); put back at "
                        "(%.0f %.0f %.0f), %s", name, loc.x, loc.y, loc.z, p.x, p.y, p.z, what[i]);
                    return;
                }
            }
        ++s.rescues;
        LOG("[GUARD] %ls: stuck in mid-air at (%.0f %.0f %.0f), no known spot to put him back (%d tried; map file %ls)", name, loc.x,
            loc.y, loc.z, n, g_mapPkg.ok ? L"read" : L"not readable");
    }
}

void Guards::Init()
{
    const auto& s = g_settings;
    if (s.fixStuckDoors)
        LOG("Guards: stuck-door fix %s", Script::Ready() ? "on" : "unavailable (script calls unavailable)");
    if (s.detectionBaseline >= 1 && s.detectionBaseline <= 3)
        PatchDetection(s.detectionBaseline);

    // Solo difficulty: hook the session override (always logs the difficulty a map was loaded with).
    if (const uintptr_t a = Mem::Find("difficulty session override",
            "85 F6 74 2D 56 E8 ? ? ? ? 8B 93 7C 01 00 00 8B 4A 28 8B 09 8B 91 58 04 00 00 33 D0 8B 81 58 04 00 00 83 E2 01 83 C4 04 33 C2 89 81 58 04 00 00 8B 83 A8 00 00 00 85 C0 0F 84 ? ? ? ? E8 ? ? ? ? 85 C0 74 ? E8"))
    {
        const uintptr_t hook = a + 0x31;
        auto target = [](uintptr_t call) { return call + 5 + *reinterpret_cast<const int32_t*>(call + 1); };
        const uintptr_t validFn = target(hook + 0x0E), baseFn = target(hook + 0x17);
        if (*reinterpret_cast<const uint16_t*>(validFn) == 0x0D8B && *reinterpret_cast<const uint8_t*>(baseFn) == 0xB8)
        {
            g_sessionValid = *reinterpret_cast<const uint32_t*>(validFn + 2);
            g_sessionBase = *reinterpret_cast<const uint32_t*>(baseFn + 1);
            Mem::MidHook("difficulty session override", hook, 6, &OnSessionOverride, Mem::MidMode::Replace);
            if (s.fixSoloDifficulty)
            {
                const HMODULE exe = GetModuleHandleW(nullptr);
                oCreateFileW = reinterpret_cast<CreateFileW_t>(Mem::PatchIAT(exe, "KERNEL32.dll", "CreateFileW", reinterpret_cast<void*>(&hkCreateFileW)));
                oCreateFileA = reinterpret_cast<CreateFileA_t>(Mem::PatchIAT(exe, "KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(&hkCreateFileA)));
                if (!oCreateFileW) oCreateFileW = &CreateFileW;
                if (!oCreateFileA) oCreateFileA = &CreateFileA;
            }
            LOG("Guards: solo difficulty fix %s (session %08X, settings %08X)", s.fixSoloDifficulty ? "on" : "off (logging only)",
                unsigned(g_sessionValid), unsigned(g_sessionBase));
        }
    }
    if (!s.guardPersonalities)
        return;
    if (!Game::GetFullName)
    {
        LOG("Guards: GetFullName not found, personalities unavailable");
        return;
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    g_iniPath = path;
    g_iniPath = g_iniPath.substr(0, g_iniPath.find_last_of(L"\\/") + 1) + L"SCCTRemaster_Guards.ini";
    if (GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        LOG("Guards: %ls not found, personalities off", g_iniPath.c_str());
        return;
    }
    Load();
    g_ready = !g_personalities.empty();
    LOG("Guards: %zu factions, %zu roles, %zu personalities, %zu excluded classes%s", g_factionTraits.size(),
        g_roleTraits.size(), g_personalities.size(), g_exclude.size(), g_ready ? "" : " (no personalities: off)");
}

void Guards::OnPawnTick(void* pawn)
{
    if (!pawn)
        return;
    if (!g_ready)
        return;
    void* level = At<void*>(pawn, kActorLevel);
    if (level != g_level)
    {
        g_level = level;
        g_done.clear();
        g_guardClass.clear();
        g_info.clear();
        g_levelStart = GetTickCount();
        g_levelLogged = false;
    }
    void* cls = At<void*>(pawn, 0x24);
    if (!cls)
        return;
    const uint64_t ck = Key(cls, nullptr);
    auto it = g_guardClass.find(ck);
    if (it == g_guardClass.end())
        it = g_guardClass.emplace(ck, IsGuardClass(pawn, Chain(pawn))).first;
    if (!it->second)
        return;
    int& ticks = g_done[Key(pawn, nullptr)];
    if (ticks < 0 || ++ticks < kApplyAfterTicks || GetTickCount() - g_levelStart < kApplyAfterMs)
        return;
    ticks = -1;
    if (!g_levelLogged && level)
    {
        g_levelLogged = true;
        uint8_t& diff = At<uint8_t>(level, kLevelDifficulty);
        if (g_restoreDiff >= 1 && g_restoreDiff <= 3 && diff != g_restoreDiff)
        {
            LOG("Difficulty: the savegame restore set difficulty %d; putting back %d", int(diff), g_restoreDiff);
            diff = static_cast<uint8_t>(g_restoreDiff);
        }
        LOG("Guards: this level runs on difficulty %d (1 lowest, 3 Expert)", int(diff));
    }
    Apply(pawn);
}

float Guards::ShadowSense(void* pawn)
{
    if (!g_ready || !pawn) return 1.0f;
    const auto it = g_info.find(pawn);
    return it == g_info.end() ? 1.0f : it->second.shadow;
}

float Guards::HearingSense(void* pawn)
{
    if (!g_ready || !pawn) return 1.0f;
    const auto it = g_info.find(pawn);
    return it == g_info.end() ? 1.0f : it->second.hearing;
}

float Guards::ReactionExtra(void* pawn)
{
    if (!g_ready || !pawn) return -1.0f;
    const auto it = g_info.find(pawn);
    return it == g_info.end() ? -1.0f : it->second.reaction;
}

const wchar_t* Guards::Label(void* pawn)
{
    if (!g_ready || !pawn) return nullptr;
    const auto it = g_info.find(pawn);
    return it == g_info.end() ? nullptr : it->second.label.c_str();
}

void Guards::OnPlayerTick(void* pawn)
{
    if (pawn && (g_settings.rescueStuckNPCs || g_settings.fixStuckDoors))
        RescueScan(pawn);
    if (!pawn || !g_settings.saveAudit)
        return;
    static void* level = nullptr;
    static DWORD levelStart = 0;
    static bool snapped = false;
    void* lv = At<void*>(pawn, kActorLevel);
    if (pawn != g_sam || lv != level)
    {
        g_sam = pawn;
        level = lv;
        levelStart = GetTickCount();
        snapped = false;
        g_samHealth = g_samInitial = INT_MIN;
        LogSam("Sam spawned");
    }
    // A second in: after a savegame has been applied.
    if (!snapped && GetTickCount() - levelStart > 1000)
    {
        snapped = true;
        LogSam("level running");
    }
    const int h = At<int>(pawn, kHealth), m = At<int>(pawn, kInitialHealth);
    if (h != g_samHealth || m != g_samInitial)
    {
        if (g_samHealth != INT_MIN)
            LOG("[SAM] health %d/%d -> %d/%d", g_samHealth, g_samInitial, h, m);
        g_samHealth = h;
        g_samInitial = m;
    }
}
