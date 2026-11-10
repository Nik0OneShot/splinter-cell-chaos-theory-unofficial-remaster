#pragma once
// The CLUSTERFUCK victory certificate: rank, badges, seal and the text file. No game dependencies (only Windows and
// the standard library), so tests can build it on its own.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace VictoryText
{
    // The solo campaign, in order: map file name, mission name.
    inline const wchar_t* const kMaps[11][2] = {
        { L"01_Lighthouse", L"Lighthouse" }, { L"02_CargoShip", L"Cargo Ship" }, { L"03_Bank", L"Bank" },
        { L"04_Penthouse", L"Penthouse" },   { L"05_Displace01", L"Displace" },  { L"06_Hokkaido", L"Hokkaido" },
        { L"07_Battery", L"Battery" },       { L"08_SeoulOne", L"Seoul One" },   { L"09_SeoulTwo", L"Seoul Two" },
        { L"10_Bathhouse", L"Bathhouse" },   { L"11_KokuboSosho", L"Kokubo Sosho" } };
    constexpr int kMissionCount = 11;

    // Mission stat counters (EMissionManager.MissionStats action types).
    enum { Knocked, Injured, Killed, CivKnocked, CivInjured, CivKilled, BodiesFound, Identified, Alarms, StatCount };

    struct Mission
    {
        std::wstring map, name;
        bool reached = false, won = false;
        float time = -1.0f;   // seconds, -1 = not known
        int c[StatCount] = {};
    };

    struct Data
    {
        std::wstring id, profile, started, finished;
        std::vector<Mission> missions;
        int lives = 1;        // this one included
        bool preview = false;
        // Filled by Compute:
        int tot[StatCount] = {};
        float totalTime = 0.0f;
        bool timeKnown = true;
        std::wstring rank, span, seal;
        std::vector<std::pair<std::wstring, std::wstring>> badges;   // name, why
    };

    inline int IndexOf(const std::wstring& map)
    {
        for (int i = 0; i < kMissionCount; ++i)
            if (!_wcsicmp(map.c_str(), kMaps[i][0])) return i;
        return -1;
    }

    inline std::wstring Clock(float s)
    {
        if (s < 0) return L"?";
        const int t = int(s + 0.5f);
        wchar_t b[32];
        swprintf_s(b, L"%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
        return b;
    }

    // "YYYY-MM-DD HH:MM:SS" -> 100 ns ticks (0 if it doesn't parse).
    inline uint64_t Ticks(const std::wstring& s)
    {
        SYSTEMTIME st = {};
        int y, mo, d, h, mi, se;
        if (swscanf_s(s.c_str(), L"%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
        st.wYear = WORD(y); st.wMonth = WORD(mo); st.wDay = WORD(d); st.wHour = WORD(h); st.wMinute = WORD(mi); st.wSecond = WORD(se);
        FILETIME ft;
        if (!SystemTimeToFileTime(&st, &ft)) return 0;
        return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    inline std::wstring Plural(int n, const wchar_t* one, const wchar_t* many)
    {
        return std::to_wstring(n) + L" " + (n == 1 ? one : many);
    }

    inline std::wstring Span(const std::wstring& a, const std::wstring& b)
    {
        const uint64_t ta = Ticks(a), tb = Ticks(b);
        if (!ta || !tb || tb < ta) return L"";
        const uint64_t min = (tb - ta) / 600000000ull;
        const int days = int(min / 1440), hours = int(min / 60 % 24), mins = int(min % 60);
        if (days) return Plural(days, L"day", L"days") + (hours ? L", " + Plural(hours, L"hour", L"hours") : L"");
        if (hours) return Plural(hours, L"hour", L"hours") + (mins ? L", " + Plural(mins, L"minute", L"minutes") : L"");
        return Plural(mins, L"minute", L"minutes");
    }

    inline void Compute(Data& d)
    {
        for (int k = 0; k < StatCount; ++k) d.tot[k] = 0;
        d.totalTime = 0.0f;
        d.timeKnown = true;
        for (const Mission& m : d.missions)
        {
            for (int k = 0; k < StatCount; ++k) d.tot[k] += m.c[k];
            if (m.time >= 0) d.totalTime += m.time; else d.timeKnown = false;
        }
        const int kills = d.tot[Killed], kos = d.tot[Knocked], civKills = d.tot[CivKilled];
        const int spotted = d.tot[Identified], bodies = d.tot[BodiesFound];

        if (kills == 0 && civKills == 0) d.rank = L"THE GHOST OF THE CLUSTERFUCK";
        else if (kills >= 200) d.rank = L"THE ONE-MAN CLUSTERFUCK";
        else if (kos >= 30 && kos >= kills * 2) d.rank = L"THE SANDMAN";
        else if (spotted == 0) d.rank = L"THE RUMOUR";
        else if (kills >= 100) d.rank = L"THE BUTCHER OF HOKKAIDO";
        else d.rank = L"CLUSTERFUCK SURVIVOR, FIRST CLASS";

        d.badges.clear();
        d.badges.push_back({ L"IRONMAN", L"one save per mission, one life, eleven missions" });
        if (d.lives <= 1) d.badges.push_back({ L"FLAWLESS", L"on the very first life. Nobody will believe you." });
        else if (d.lives >= 10) d.badges.push_back({ L"STUBBORN", L"it took " + std::to_wstring(d.lives) + L" lives. Worth it." });
        if (kills == 0 && civKills == 0) d.badges.push_back({ L"PACIFIST", L"not one life taken" });
        else if (civKills == 0) d.badges.push_back({ L"MERCIFUL", L"no civilian was killed" });
        if (kills >= 150) d.badges.push_back({ L"BUTCHER", std::to_wstring(kills) + L" enemies killed" });
        if (kos >= 100) d.badges.push_back({ L"SANDMAN", std::to_wstring(kos) + L" guards put to sleep" });
        if (spotted == 0) d.badges.push_back({ L"PHANTOM", L"never once identified" });
        if (bodies == 0) d.badges.push_back({ L"JANITOR", L"no guard ever found a body" });
        if (d.timeKnown && d.totalTime > 0 && d.totalTime < 4 * 3600) d.badges.push_back({ L"SPEED DEMON", L"the whole game in under 4 hours" });
        const uint64_t ta = Ticks(d.started), tb = Ticks(d.finished);
        if (ta && tb > ta && (tb - ta) / 864000000000ull >= 30) d.badges.push_back({ L"MARATHON", L"over a month from start to finish" });

        d.span = Span(d.started, d.finished);

        // The seal: FNV-1a over the campaign and its numbers. It proves nothing; it looks official.
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const std::wstring& s) { for (wchar_t c : s) { h ^= uint16_t(c); h *= 1099511628211ull; } h ^= 0xFF; h *= 1099511628211ull; };
        mix(d.id); mix(d.finished); mix(std::to_wstring(d.lives));
        for (int k = 0; k < StatCount; ++k) mix(std::to_wstring(d.tot[k]));
        wchar_t seal[32];
        swprintf_s(seal, L"%04X-%04X-%04X-%04X", unsigned(h >> 48) & 0xFFFF, unsigned(h >> 32) & 0xFFFF, unsigned(h >> 16) & 0xFFFF,
                   unsigned(h) & 0xFFFF);
        d.seal = d.preview ? L"PREVIEW-NOT-A-VICTORY" : seal;
    }

    constexpr int kWidth = 78;

    inline std::wstring Center(const std::wstring& s, int width = kWidth)
    {
        const int pad = (width - int(s.size())) / 2;
        return (pad > 0 ? std::wstring(size_t(pad), L' ') : L"") + s;
    }

    // Big block letters (5 rows).
    inline std::vector<std::wstring> Banner(const wchar_t* text)
    {
        struct Glyph { wchar_t c; const char* rows[5]; };
        static const Glyph glyphs[] = {
            { L'Y', { "#...#", ".#.#.", "..#..", "..#..", "..#.." } }, { L'O', { ".###.", "#...#", "#...#", "#...#", ".###." } },
            { L'U', { "#...#", "#...#", "#...#", "#...#", ".###." } }, { L'A', { ".###.", "#...#", "#####", "#...#", "#...#" } },
            { L'C', { ".####", "#....", "#....", "#....", ".####" } }, { L'T', { "#####", "..#..", "..#..", "..#..", "..#.." } },
            { L'L', { "#....", "#....", "#....", "#....", "#####" } }, { L'I', { "###", ".#.", ".#.", ".#.", "###" } },
            { L'D', { "####.", "#...#", "#...#", "#...#", "####." } }, { L'!', { "#", "#", "#", ".", "#" } },
            { L' ', { "..", "..", "..", "..", ".." } } };
        std::vector<std::wstring> rows(5);
        for (const wchar_t* p = text; *p; ++p)
            for (const Glyph& g : glyphs)
                if (g.c == *p)
                {
                    for (int r = 0; r < 5; ++r)
                    {
                        if (p != text) rows[r] += L' ';
                        for (const char* q = g.rows[r]; *q; ++q) rows[r] += *q == '#' ? L'\x2588' : L' ';
                    }
                    break;
                }
        for (auto& r : rows) r = Center(r);
        return rows;
    }

    inline std::wstring Certificate(const Data& d)
    {
        std::wstring t;
        auto line = [&](const std::wstring& s = L"") { t += s; t += L"\r\n"; };
        const std::wstring rule(kWidth, L'=');
        auto fmt = [](const wchar_t* f, auto... a) { wchar_t b[512]; swprintf_s(b, f, a...); return std::wstring(b); };

        if (d.preview)
        {
            line(L"PREVIEW: this is what the certificate looks like. Nobody won anything (yet).");
            line();
        }
        line(rule);
        line();
        for (const wchar_t* w : { L"YOU", L"ACTUALLY", L"DID IT!" })
        {
            for (const auto& r : Banner(w)) line(r);
            line();
        }
        line(rule);
        line();
        line(Center(L"CERTIFICATE OF COMPLETELY UNREASONABLE ACHIEVEMENT"));
        line();
        const std::wstring who = d.profile.empty() ? L"" : L" (profile \"" + d.profile + L"\")";
        line(L"  This certifies that SAM FISHER, Third Echelon, Splinter Cell, and the human");
        line(L"  holding the controller" + who + L", went through every mission of");
        line(L"  Splinter Cell Chaos Theory on CLUSTERFUCK, the difficulty named for what it");
        line(L"  is, and lived.");
        line();
        static const wchar_t* const medal[] = {
            LR"(   \\\\\\\\\\      //////////)",
            LR"(    \\\\\\\\\\    //////////)",
            LR"(     \\\\\\\\\\  //////////)",
            LR"(      \\\\\\\\\\//////////)",
            LR"(      .-''''''''''''''''-.)",
            LR"(    .'         /\         '.)",
            LR"(   /      ____/  \____      \)",
            LR"(  |       \   C  F   /       |)",
            LR"(  |        \        /        |)",
            LR"(  |        /  /  \  \        |)",
            LR"(   \      /__/    \__\      /)",
            LR"(    '.                    .')",
            LR"(      '-.______________.-')" };
        const int artWidth = 32, pad = (kWidth - artWidth) / 2;
        for (const wchar_t* m : medal) line(std::wstring(size_t(pad), L' ') + m);
        line();
        line(Center(L"THE ORDER OF THE CLUSTERFUCK"));
        line(Center(L"RANK: " + d.rank));
        line();
        line(L"  What they lived through:");
        line(L"    - 50 HP, no medkits, half the ammo, no pickups, no light or sound meter");
        line(L"    - elite guards with night vision, flashlights and endless ammo, all one party");
        line(L"    - base alert from the start of every mission, and every body found an alarm");
        line(L"    - knocked-out guards waking up, searches that never end, instant cameras");
        line(L"    - every hack the hardest there is, wall mines that always go off");
        line(L"    - one save per mission and one life: every Sam who died took his saves along");
        line();
        line(L"  The campaign:");
        line(fmt(L"    Campaign       %ls", d.id.c_str()));
        line(fmt(L"    Started        %ls", d.started.c_str()));
        line(fmt(L"    Finished       %ls%ls", d.finished.c_str(), d.span.empty() ? L"" : (L"   (" + d.span + L" later)").c_str()));
        line(fmt(L"    Lives it took  %d%ls", d.lives,
                 d.lives <= 1 ? L" (the first one!)" : (L" (" + Plural(d.lives - 1, L"Sam", L"Sams") + L" died so this one could live)").c_str()));
        line(fmt(L"    Mission time   %ls%ls", Clock(d.totalTime).c_str(), d.timeKnown ? L"" : L" (some missions weren't timed)"));
        line();
        line(L"   #  Mission          Time     Kills   KOs  Injured  Bodies found  Spotted");
        line(L"  --  ---------------  -------  -----  ----  -------  ------------  -------");
        for (size_t i = 0; i < d.missions.size(); ++i)
        {
            const Mission& m = d.missions[i];
            if (m.won || m.time >= 0)
                line(fmt(L"  %2d  %-15ls  %7ls  %5d  %4d  %7d  %12d  %7d", int(i + 1), m.name.c_str(), Clock(m.time).c_str(), m.c[Killed],
                         m.c[Knocked], m.c[Injured], m.c[BodiesFound], m.c[Identified]));
            else
                line(fmt(L"  %2d  %-15ls  %ls", int(i + 1), m.name.c_str(), m.reached ? L"survived (not counted)" : L"-"));
        }
        line(L"  --  ---------------  -------  -----  ----  -------  ------------  -------");
        line(fmt(L"      %-15ls  %7ls  %5d  %4d  %7d  %12d  %7d", L"TOTAL", Clock(d.timeKnown ? d.totalTime : -1).c_str(), d.tot[Killed],
                 d.tot[Knocked], d.tot[Injured], d.tot[BodiesFound], d.tot[Identified]));
        line(fmt(L"      Civilians: %d killed, %d knocked out, %d injured. Alarms: %d.", d.tot[CivKilled], d.tot[CivKnocked],
                 d.tot[CivInjured], d.tot[Alarms]));
        line();
        line(L"  Badges:");
        for (const auto& [name, why] : d.badges) line(fmt(L"    [*] %-12ls %ls", name.c_str(), why.c_str()));
        line();
        line(fmt(L"  Seal of authenticity: %ls", d.seal.c_str()));
        line(L"  (it proves nothing, but it looks very official)");
        line();
        line(rule);
        line(Center(L"Third Echelon would like to remind you that none of this ever happened."));
        line(rule);
        return t;
    }

    inline std::string Utf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    // A sample campaign for the preview.
    inline Data Sample()
    {
        Data d;
        d.preview = true;
        d.id = L"PREVIEW";
        d.profile = L"YOU";
        d.started = L"2026-11-01 20:00:00";
        d.finished = L"2026-11-09 23:41:00";
        d.lives = 7;
        static const float times[11] = { 2471, 2894, 2215, 2650, 2380, 3190, 2760, 2520, 2905, 3330, 3640 };
        for (int i = 0; i < kMissionCount; ++i)
        {
            Mission m;
            m.map = kMaps[i][0];
            m.name = kMaps[i][1];
            m.reached = m.won = true;
            m.time = times[i];
            m.c[Killed] = 3 + i * 2 % 7;
            m.c[Knocked] = 9 + i * 3 % 11;
            m.c[Injured] = i % 3;
            m.c[BodiesFound] = i % 4 == 1 ? 1 : 0;
            m.c[Identified] = 2 + i % 5;
            m.c[Alarms] = 4 + i % 3;
            d.missions.push_back(m);
        }
        Compute(d);
        return d;
    }
}
