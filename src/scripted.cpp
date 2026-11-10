// Scripted sequences.
//
// Level scripts are pattern objects (EchelonPattern.u, e.g. P_10_Bathhouse_ShetlandFight) running state code.
// Characters are brought in with EPattern.Teleport (native 2265: (int character, name InfoPoint, bool, bool)),
// called from that state code through the extended-native token (68h D9h), so the VM calls GNatives[2265] with
// the pattern's own FStateFrame (UObject +0Ch: Node +4, Code +0Ch) and Code pointing just past the token. The
// pattern's characters are its Members[10] (+2C8h, controllers, by character number; pawn at controller +41Ch).
//
// Bathhouse ending ([Gameplay] BathhouseEndingDelay): while Sam defuses Shetland's three bombs in Furnace B, the
// script brings elite Displace guards in at the furnace entrance the instant a bomb is defused (after bomb 1:
// character 2; bomb 2: characters 3 and 4, to EInfoPoint_FurnaceB_EDG_Entrance_SpawnPoint01/02; bomb 3:
// character 5 at EDG_Exit_SpawnPoint). Each is a Teleport followed by Goal_Set (1439, walk to the start point)
// and Goal_Default (1440, patrol) for the same character. Until then they wait in a box under the level (z about
// -1700). Here, when the script reaches such a Teleport, the whole block (those statements, both guards for bomb
// 2) is skipped: Code moves past it and the script carries straight on with Shetland. A copy of each statement's
// arguments (all constants) is kept, and BathhouseEndingDelay seconds later the statements are run on the
// pattern, in order, as the VM would have run them.
// A save made during the wait has the script past the block and the guards still in their box: after a load the
// state code is scanned for EDG teleports it has passed whose guard is still in the box, and those blocks come
// in BathhouseEndingDelay seconds after the load.
// Every pattern teleport is logged ([SCRIPT]: which pattern, where, who moved), for other sequences like it.
#include "pch.h"
#include "features.h"

namespace
{
    template <class T> T& At(void* p, uintptr_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p) + off); }
    struct Vec { float x, y, z; };
    struct Frame { void** vtbl; void* node; void* object; const uint8_t* code; uint8_t* locals; };
    using Exec_t = void(__fastcall*)(void* obj, void* edx, void* frame, void* result);

    constexpr int kTeleport = 2265, kGoalSet = 1439, kGoalDefault = 1440;
    constexpr uintptr_t kLevel = 0x78, kXLevel = 0x7C, kTime = 0x430, kLocation = 0xE8, kStateFrame = 0xC;
    constexpr uintptr_t kMembers = 0x2C8, kEPawn = 0x41C, kHealth = 0x420;
    constexpr float kBoxZ = -1200.0f;   // below this a furnace guard is still in his box
    const wchar_t* const kEndingPattern = L"P_10_Bathhouse_ShetlandFight";

    Exec_t oTeleport = nullptr;
    bool g_tried = false;
    float g_delay = 15.0f;
    void* g_xlevel = nullptr;
    void* g_sam = nullptr;
    float g_lastTime = 0, g_scanAt = -1;

    struct Call { int native; std::vector<uint8_t> args; };
    struct Block
    {
        void* pattern; float at; std::vector<Call> calls; std::vector<int> chars;
        bool placed = false; std::vector<Vec> spawns;   // where its guards appear (looked up when due)
        bool waitLogged = false;
    };
    float g_clearance = 600.0f;   // a due guard waits while Sam is this close to his spawn point (units)
    std::vector<Block> g_blocks;   // guards held back, with the statements that bring them in

    float LevelTime(void* actor)
    {
        void* level = At<void*>(actor, kLevel);
        return level ? At<float>(level, kTime) : 0.0f;
    }

    void ShortName(void* obj, wchar_t* out, size_t len)
    {
        out[0] = 0;
        wchar_t buf[512] = {};
        __try { Game::GetFullName(obj, nullptr, buf); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        const wchar_t* dot = wcsrchr(buf, L'.');
        wcsncpy_s(out, len, dot ? dot + 1 : buf, _TRUNCATE);
    }

    bool ClassIs(void* obj, const wchar_t* name)
    {
        wchar_t chain[2][64] = {};
        return Game::ClassChain(obj, chain, 1) > 0 && wcscmp(chain[0], name) == 0;
    }

    // One constant argument: its length in bytes, 0 if it isn't one we know.
    size_t ConstLen(const uint8_t* p)
    {
        switch (p[0])
        {
        case 0x0B: case 0x25: case 0x26: case 0x27: case 0x28: case 0x2A: return 1;   // nothing, 0, 1, true, false, None
        case 0x24: case 0x2C: return 2;                                               // byte, small int
        case 0x1D: case 0x1E: case 0x20: case 0x21: return 5;                         // int, float, object, name
        case 0x22: case 0x23: return 13;                                              // rotator, vector
        case 0x1F: { size_t n = 1; while (p[n]) ++n; return n + 1; }                  // string
        default: return 0;
        }
    }

    // A native call statement at p (extended-native token, constant arguments, EX_EndFunctionParms): its native
    // number, its arguments (with the end marker) and where the next statement starts. False if it isn't one.
    bool ParseCall(const uint8_t* p, const uint8_t* limit, int& native, std::vector<uint8_t>* args, const uint8_t*& next)
    {
        if (p + 3 > limit || (p[0] & 0xF0) != 0x60) return false;
        native = ((p[0] & 0x0F) << 8) | p[1];
        const uint8_t* q = p + 2;
        for (int n = 0; n < 40 && q < limit; ++n)
        {
            if (q[0] == 0x16)
            {
                if (args) args->assign(p + 2, q + 1);
                next = q + 1;
                return true;
            }
            const size_t len = ConstLen(q);
            if (!len) return false;
            q += len;
        }
        return false;
    }

    // The first argument of a call (a character number), -1 if it isn't a constant int.
    int FirstInt(const std::vector<uint8_t>& a)
    {
        if (a.empty()) return -1;
        switch (a[0])
        {
        case 0x25: return 0;
        case 0x26: return 1;
        case 0x2C: return a.size() > 1 ? a[1] : -1;
        case 0x1D: return a.size() > 4 ? *reinterpret_cast<const int*>(&a[1]) : -1;
        default: return -1;
        }
    }

    // The InfoPoint of a Teleport's arguments (character, then EX_NameConst).
    bool TeleportPoint(const std::vector<uint8_t>& a, wchar_t* out, size_t len)
    {
        size_t i = 0;
        if (a.empty() || !(i = ConstLen(a.data())) || i + 5 > a.size() || a[i] != 0x21) return false;
        return Script::NameText(*reinterpret_cast<const int*>(&a[i + 1]), out, len);
    }

    int TeleportPointName(const std::vector<uint8_t>& a)
    {
        size_t i = 0;
        if (a.empty() || !(i = ConstLen(a.data())) || i + 5 > a.size() || a[i] != 0x21) return 0;
        return *reinterpret_cast<const int*>(&a[i + 1]);
    }

    // The spawn points of a block's teleports: the actors whose Tag (+54h) or name (+20h) is the InfoPoint.
    void PlaceSpawns(Block& b, void* sam)
    {
        b.placed = true;
        void* xlevel = At<void*>(sam, kXLevel);
        void** actors = xlevel ? At<void**>(xlevel, 0x28) : nullptr;
        const int n = xlevel ? At<int>(xlevel, 0x2C) : 0;
        if (!actors || n <= 0 || n > 500000) return;
        for (const Call& c : b.calls)
        {
            const int name = c.native == kTeleport ? TeleportPointName(c.args) : 0;
            if (!name) continue;
            for (int i = 0; i < n; ++i)
            {
                void* a = actors[i];
                if (!a || (At<uint32_t>(a, 0x28) & 4)) continue;
                if (At<int>(a, 0x54) == name || At<int>(a, 0x20) == name) { b.spawns.push_back(At<Vec>(a, kLocation)); break; }
            }
        }
    }

    // Distance from Sam to the nearest of the block's spawn points (units), a large value if none is known.
    float SamToSpawn(const Block& b, void* sam)
    {
        const Vec& s = At<Vec>(sam, kLocation);
        float best = 1e9f;
        for (const Vec& p : b.spawns)
        {
            const float dx = s.x - p.x, dy = s.y - p.y, dz = s.z - p.z;
            best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        return best;
    }

    bool IsGuardEntrance(const std::vector<uint8_t>& a)
    {
        wchar_t point[128];
        return TeleportPoint(a, point, 128) && wcsstr(point, L"EDG") && wcsstr(point, L"SpawnPoint");
    }

    // The block of statements bringing guards in, starting at a Teleport of one: the Teleport, the goals that
    // follow for the same character, and the next guard's Teleport and goals if one comes right after.
    bool ParseBlock(const uint8_t* p, const uint8_t* limit, Block& b, const uint8_t*& end)
    {
        b.calls.clear(); b.chars.clear();
        end = p;
        for (int n = 0; n < 12; ++n)
        {
            Call c;
            const uint8_t* next = nullptr;
            if (!ParseCall(end, limit, c.native, &c.args, next)) break;
            const int who = FirstInt(c.args);
            const bool tele = c.native == kTeleport && IsGuardEntrance(c.args);
            const bool goal = (c.native == kGoalSet || c.native == kGoalDefault) &&
                              std::find(b.chars.begin(), b.chars.end(), who) != b.chars.end();
            if (!tele && !goal) break;
            if (tele) b.chars.push_back(who);
            b.calls.push_back(std::move(c));
            end = next;
        }
        return !b.calls.empty() && b.calls[0].native == kTeleport;
    }

    void* CharacterPawn(void* pattern, int character)
    {
        if (character < 0 || character >= 10) return nullptr;
        void* ctrl = At<void*>(pattern, kMembers + character * 4);
        return ctrl ? At<void*>(ctrl, kEPawn) : nullptr;
    }

    bool InBox(void* pawn) { return pawn && At<int>(pawn, kHealth) > 0 && At<Vec>(pawn, kLocation).z < kBoxZ; }

    void CharList(const Block& b, char* out, size_t len)
    {
        out[0] = 0;
        for (size_t i = 0; i < b.chars.size(); ++i)
        {
            char one[48];
            wchar_t name[64] = L"?";
            if (void* pawn = CharacterPawn(b.pattern, b.chars[i])) ShortName(pawn, name, 64);
            _snprintf_s(one, _TRUNCATE, "%s%ls", i ? " and " : "", name);
            strncat_s(out, len, one, _TRUNCATE);
        }
    }

    // The script reached a guard entrance: skip the block now, bring it in later. True if held.
    bool Hold(void* obj, Frame* frame)
    {
        if (g_delay <= 0 || Clusterfuck::Active() || !ClassIs(obj, kEndingPattern)) return false;   // CLUSTERFUCK: at once
        const uint8_t* c = frame->code;
        if (At<void*>(obj, kStateFrame) != frame || c[-2] != (0x60 | (kTeleport >> 8)) || c[-1] != (kTeleport & 0xFF)) return false;
        Block b;
        const uint8_t* end = nullptr;
        if (!ParseBlock(c - 2, c + 4096, b, end)) return false;
        b.pattern = obj;
        b.at = LevelTime(obj) + g_delay;
        frame->code = end;   // the script goes on with what comes after
        char who[160]; CharList(b, who, sizeof(who));
        LOG("[SCRIPT] Bathhouse ending: a bomb is defused; %s held back %.0f s (%zu statements), Shetland carries on",
            who, g_delay, b.calls.size());
        g_blocks.push_back(std::move(b));
        return true;
    }

    void __fastcall hkTeleport(void* obj, void* edx, void* frameP, void* result);

    void Release(const Block& b)
    {
        char who[160]; CharList(b, who, sizeof(who));
        LOG("[SCRIPT] Bathhouse ending: %s come in now", who);
        for (const Call& c : b.calls)
        {
            std::vector<uint8_t> code = c.args;
            code.push_back(0); code.push_back(0);
            void* fn = c.native == kTeleport ? reinterpret_cast<void*>(&hkTeleport) : Script::Native(c.native);
            if (!fn || !Script::CallNative(b.pattern, fn, code.data()))
                LOG("[SCRIPT]   native %d failed", c.native);
        }
    }

    // After a load: blocks the script has passed whose guards are still in their box (a save made during a wait).
    void ScanAfterLoad(void* sam)
    {
        void* xlevel = At<void*>(sam, kXLevel);
        void** actors = xlevel ? At<void**>(xlevel, 0x28) : nullptr;
        const int n = xlevel ? At<int>(xlevel, 0x2C) : 0;
        if (!actors || n <= 0 || n > 500000) return;
        void* pattern = nullptr;
        for (int i = 0; i < n && !pattern; ++i)
        {
            void* a = actors[i];
            if (a && !(At<uint32_t>(a, 0x28) & 4) && !(At<uint32_t>(a, 0x2C) & 0x10) && ClassIs(a, kEndingPattern)) pattern = a;
        }
        if (!pattern) return;
        Frame* frame = At<Frame*>(pattern, kStateFrame);
        if (!frame || !frame->code || !frame->node) return;
        // The state's bytecode: the TArray in the UState holding Code.
        const uint8_t* base = nullptr;
        int size = 0;
        for (uintptr_t off = 0x20; off < 0x100 && !base; off += 4)
        {
            const uint8_t* d = At<const uint8_t*>(frame->node, off);
            const int num = At<int>(frame->node, off + 4);
            if (d && num > 16 && num < 1000000 && frame->code >= d && frame->code < d + num) { base = d; size = num; }
        }
        if (!base) return;
        const uint8_t* limit = base + size;
        for (const uint8_t* p = base; p + 8 < limit && p < frame->code; ++p)
        {
            if (p[0] != (0x60 | (kTeleport >> 8)) || p[1] != (kTeleport & 0xFF)) continue;
            Block b;
            const uint8_t* end = nullptr;
            if (!ParseBlock(p, limit, b, end) || end > frame->code) continue;
            b.pattern = pattern;
            bool waiting = true;
            for (int ch : b.chars) waiting = waiting && InBox(CharacterPawn(pattern, ch));
            if (waiting)
            {
                b.at = LevelTime(sam) + g_delay;
                char who[160]; CharList(b, who, sizeof(who));
                LOG("[SCRIPT] Bathhouse ending: loaded during a wait; %s come in %.0f s from now", who, g_delay);
                g_blocks.push_back(std::move(b));
            }
            p = end - 1;
        }
    }

    void __fastcall hkTeleport(void* obj, void* edx, void* frameP, void* result)
    {
        Frame* frame = static_cast<Frame*>(frameP);
        wchar_t cls[64] = L"?", point[128] = L"?";
        int character = -1;
        __try
        {
            wchar_t chain[2][64] = {};
            if (Game::ClassChain(obj, chain, 1) > 0) wcsncpy_s(cls, chain[0], _TRUNCATE);
            if (Hold(obj, frame)) return;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Who moved: pawn locations before and after.
        static void* pawns[512];
        static Vec before[512];
        int n = 0;
        void* xlevel = g_sam ? At<void*>(g_sam, kXLevel) : nullptr;
        __try
        {
            const uint8_t* p = frame->code;
            const size_t len = ConstLen(p);
            if (len)
            {
                character = p[0] == 0x26 ? 1 : p[0] == 0x25 ? 0 : p[0] == 0x2C ? p[1] : p[0] == 0x1D ? *reinterpret_cast<const int*>(p + 1) : -1;
                if (p[len] == 0x21) Script::NameText(*reinterpret_cast<const int*>(p + len + 1), point, 128);
            }
            if (xlevel)
            {
                void** actors = At<void**>(xlevel, 0x28);
                const int count = At<int>(xlevel, 0x2C);
                for (int i = 0; actors && i < count && i < 500000 && n < 512; ++i)
                {
                    void* a = actors[i];
                    if (!a || (At<uint32_t>(a, 0x28) & 4) || !(At<uint32_t>(a, 0x2C) & 0x10)) continue;
                    pawns[n] = a;
                    before[n] = At<Vec>(a, kLocation);
                    ++n;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
        oTeleport(obj, edx, frameP, result);
        __try
        {
            wchar_t who[160] = L"nobody moved";
            for (int i = 0; i < n; ++i)
            {
                const Vec& a = before[i];
                const Vec& b = At<Vec>(pawns[i], kLocation);
                const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                if (dx * dx + dy * dy + dz * dz < 100.0f * 100.0f) continue;
                wchar_t name[64];
                ShortName(pawns[i], name, 64);
                const wchar_t* label = Guards::Label(pawns[i]);
                const Vec& s = At<Vec>(g_sam, kLocation);
                const float sx = s.x - b.x, sy = s.y - b.y, sz = s.z - b.z;
                _snwprintf_s(who, _TRUNCATE, L"%ls%ls%ls%ls, %.1f m from Sam", name, label ? L" (" : L"", label ? label : L"",
                             label ? L")" : L"", std::sqrt(sx * sx + sy * sy + sz * sz) / 100.0f);
                break;
            }
            LOG("[SCRIPT] %ls teleports character %d to %ls: %ls", cls, character, point, who);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void ScanSafe(void* sam)
    {
        __try { ScanAfterLoad(sam); }
        __except (EXCEPTION_EXECUTE_HANDLER) { LOG("[SCRIPT] scan after load failed"); }
    }

    void ReleaseSafe(const Block& b)
    {
        __try { Release(b); }
        __except (EXCEPTION_EXECUTE_HANDLER) { LOG("[SCRIPT] bringing the held-back guards in failed"); }
    }

    // A due block's guards come in only once Sam isn't within BathhouseSpawnClearance of where they appear.
    bool ClearOfSam(Block& b, void* sam)
    {
        if (g_clearance <= 0) return true;
        __try
        {
            if (!b.placed) PlaceSpawns(b, sam);
            const float d = SamToSpawn(b, sam);
            if (d >= g_clearance) return true;
            if (!b.waitLogged)
            {
                b.waitLogged = true;
                char who[160]; CharList(b, who, sizeof(who));
                LOG("[SCRIPT] Bathhouse ending: %s wait%s: Sam is %.1f m from where they appear (needs %.0f m)", who,
                    b.chars.size() > 1 ? "" : "s", d / 100.0f, g_clearance / 100.0f);
            }
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    }
}

void Scripted::Init()
{
    g_delay = std::clamp(g_settings.bathhouseEndingDelay, 0.0f, 120.0f);
    g_clearance = std::clamp(g_settings.bathhouseSpawnClearance, 0.0f, 50.0f) * 100.0f;
    if (g_delay > 0)
        LOG("Scripted: Bathhouse ending: the elite guards come in %.0f s after each bomb is defused instead of at once "
            "(Shetland and the bombs keep their timing), and not while Sam is within %.0f m of where they appear",
            g_delay, g_clearance / 100.0f);
    else
        LOG("Scripted: Bathhouse ending as in the original (guards come in the instant a bomb is defused)");
}

void Scripted::OnPlayerTick(void* pawn)
{
    g_sam = pawn;
    const float now = LevelTime(pawn);
    void* xlevel = At<void*>(pawn, kXLevel);
    if (xlevel != g_xlevel || now + 0.5f < g_lastTime)   // another level, or time went back (a load)
    {
        g_xlevel = xlevel;
        g_blocks.clear();
        g_scanAt = now + 0.5f;
    }
    g_lastTime = now;
    if (!g_tried)
    {
        void** slot = Script::NativeSlot(kTeleport);
        if (slot && *slot)   // natives registered
        {
            g_tried = true;
            oTeleport = reinterpret_cast<Exec_t>(*slot);
            *slot = reinterpret_cast<void*>(&hkTeleport);
            LOG("Scripted: pattern teleports hooked (EPattern.Teleport)");
        }
    }
    if (!oTeleport || g_delay <= 0) return;
    if (g_scanAt >= 0 && now >= g_scanAt)
    {
        g_scanAt = -1;
        ScanSafe(pawn);
    }
    for (size_t i = 0; i < g_blocks.size();)
    {
        if (now >= g_blocks[i].at && ClearOfSam(g_blocks[i], pawn))
        {
            const Block b = std::move(g_blocks[i]);
            g_blocks.erase(g_blocks.begin() + i);
            ReleaseSafe(b);
        }
        else
            ++i;
    }
}
