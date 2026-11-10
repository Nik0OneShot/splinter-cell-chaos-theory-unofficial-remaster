// The CLUSTERFUCK victory ceremony ("YOU ACTUALLY DID IT!") and the mission toasts.
//
// Drawn into the level editor's Dear ImGui overlay: editor.cpp sets ImGui up (and calls AddFonts when it does) and
// calls Draw every frame while Wants() is true, editor open or not. Everything is drawn straight into the foreground
// draw list, no windows and no input, over whatever the game shows (the final mission's fade-out, the debrief, the
// credits). The big text is Windows' Impact, rasterised once at its final size; the slam-in scales the vertices, so
// the animation doesn't bake a new font size every frame. The fanfare and the chime are synthesised here and played
// with PlaySound from memory.
#include "pch.h"
#include "features.h"
#include "victory_text.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <mmsystem.h>

namespace
{
    // ----- helpers -----

    struct Rng
    {
        uint32_t s = 0x2545F491u;
        float f() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return float(s & 0xFFFFFF) / float(0x1000000); }
        float r(float a, float b) { return a + (b - a) * f(); }
    };
    Rng g_rng;
    ImFont* g_big = nullptr;   // Impact (or the default font if it's missing)

    constexpr float kPi = 3.14159265f;
    float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
    float EaseOut(float t) { t = Clamp01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }
    float Bounce(float t)
    {
        t = Clamp01(t);
        const float n = 7.5625f, d = 2.75f;
        if (t < 1 / d) return n * t * t;
        if (t < 2 / d) { t -= 1.5f / d; return n * t * t + 0.75f; }
        if (t < 2.5f / d) { t -= 2.25f / d; return n * t * t + 0.9375f; }
        t -= 2.625f / d; return n * t * t + 0.984375f;
    }
    ImU32 Rgba(float r, float g, float b, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, Clamp01(a))); }
    ImU32 Hsv(float h, float s, float v, float a)
    {
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(h - std::floor(h), s, v, r, g, b);
        return Rgba(r, g, b, a);
    }
    ImFont* Big() { return g_big ? g_big : ImGui::GetFont(); }
    ImVec2 Measure(ImFont* f, float size, const char* s, const char* e = nullptr) { return f->CalcTextSizeA(size, FLT_MAX, 0.0f, s, e); }

    void Outlined(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* s, float o, ImU32 oc, const char* e = nullptr)
    {
        if (o > 0)
            for (int k = 0; k < 8; ++k)
            {
                const float a = k * kPi / 4;
                dl->AddText(f, size, ImVec2(p.x + std::cos(a) * o, p.y + std::sin(a) * o), oc, s, e);
            }
        dl->AddText(f, size, p, col, s, e);
    }
    void Centered(ImDrawList* dl, ImFont* f, float size, float cx, float y, ImU32 col, const char* s, float o = 0, ImU32 oc = 0)
    {
        const ImVec2 sz = Measure(f, size, s);
        Outlined(dl, f, size, ImVec2(cx - sz.x * 0.5f, y), col, s, o, oc ? oc : Rgba(0, 0, 0, 0.9f));
    }

    // Scale/rotate/move the vertices added since 'start' around 'pivot', and multiply their alpha.
    void Transform(ImDrawList* dl, int start, ImVec2 pivot, float scale, float angle, ImVec2 move, float alpha = 1.0f)
    {
        const float c = std::cos(angle) * scale, s = std::sin(angle) * scale;
        for (int i = start; i < dl->VtxBuffer.Size; ++i)
        {
            ImDrawVert& v = dl->VtxBuffer[i];
            const float x = v.pos.x - pivot.x, y = v.pos.y - pivot.y;
            v.pos = ImVec2(pivot.x + x * c - y * s + move.x, pivot.y + x * s + y * c + move.y);
            if (alpha < 1.0f)
            {
                const uint32_t a = uint32_t(float((v.col >> IM_COL32_A_SHIFT) & 0xFF) * Clamp01(alpha));
                v.col = (v.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
            }
        }
    }

    // A triangle with one colour at 'a' and another at 'b' and 'c' (glows, sunburst rays).
    void GradTri(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 c, ImU32 ca, ImU32 cbc)
    {
        const ImVec2 uv = dl->_Data->TexUvWhitePixel;
        dl->PrimReserve(3, 3);
        dl->PrimVtx(a, uv, ca);
        dl->PrimVtx(b, uv, cbc);
        dl->PrimVtx(c, uv, cbc);
    }
    void Glow(ImDrawList* dl, ImVec2 c, float r, ImU32 inner)
    {
        const int n = 48;
        const ImU32 outer = inner & ~IM_COL32_A_MASK;
        for (int i = 0; i < n; ++i)
        {
            const float a0 = i * 2 * kPi / n, a1 = (i + 1) * 2 * kPi / n;
            GradTri(dl, c, ImVec2(c.x + std::cos(a0) * r, c.y + std::sin(a0) * r), ImVec2(c.x + std::cos(a1) * r, c.y + std::sin(a1) * r), inner, outer);
        }
    }
    void Star(ImDrawList* dl, ImVec2 c, float ro, float ri, float rot, ImU32 col)
    {
        ImVec2 p[10];
        for (int i = 0; i < 10; ++i)
        {
            const float a = rot - kPi / 2 + i * kPi / 5, r = i % 2 ? ri : ro;
            p[i] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
        }
        for (int i = 0; i < 10; ++i) dl->AddTriangleFilled(c, p[i], p[(i + 1) % 10], col);
    }
    void Sparkle(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
    {
        const float w = r * 0.18f;
        dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + w, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - w, c.y), col);
        dl->AddQuadFilled(ImVec2(c.x - r, c.y), ImVec2(c.x, c.y - w), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + w), col);
    }

    std::string U8(const std::wstring& w) { return VictoryText::Utf8(w); }

    // ----- sound -----

    struct Note { float t, d, f, v; int kind; };   // kind: 0 brass, 1 bell, 2 timpani, 3 cymbal
    float Midi(int m) { return 440.0f * std::pow(2.0f, (m - 69) / 12.0f); }

    std::vector<uint8_t> Render(const std::vector<Note>& notes, float length)
    {
        const int rate = 22050, n = int(length * rate);
        std::vector<float> mix(size_t(n), 0.0f);
        uint32_t noise = 0x1234567u;
        for (const Note& no : notes)
        {
            const int s0 = int(no.t * rate);
            const float tail = no.kind == 0 ? 0.25f : no.kind == 3 ? 2.2f : 1.6f;
            const int s1 = std::min(n, int((no.t + no.d + tail) * rate));
            float prev = 0.0f;
            for (int i = std::max(0, s0); i < s1; ++i)
            {
                const float t = float(i - s0) / rate;
                float v = 0.0f;
                if (no.kind == 0)   // brass: a bright stack of harmonics, opening on the attack, a little vibrato
                {
                    float env = t < 0.03f ? t / 0.03f : t < 0.18f ? 1.0f - (t - 0.03f) / 0.15f * 0.25f : 0.75f;
                    if (t > no.d) env *= std::max(0.0f, 1.0f - (t - no.d) / 0.22f);
                    const float vib = 1.0f + (t > 0.25f ? 0.0045f * std::sin(2 * kPi * 5.5f * t) : 0.0f);
                    const float bright = 0.45f + 0.55f * std::min(1.0f, t / 0.06f);
                    for (int h = 1; h <= 12 && no.f * h < 9000.0f; ++h)
                        v += std::sin(2 * kPi * no.f * h * vib * t) * (h == 1 ? 1.0f : bright / std::pow(float(h), 1.1f));
                    v *= env * 0.5f;
                }
                else if (no.kind == 1)   // bell
                {
                    const float a = std::min(1.0f, t / 0.004f);
                    v = a * (std::sin(2 * kPi * no.f * t) * std::exp(-3.0f * t) + 0.5f * std::sin(2 * kPi * no.f * 2.76f * t) * std::exp(-5.0f * t) +
                             0.25f * std::sin(2 * kPi * no.f * 5.4f * t) * std::exp(-8.0f * t));
                }
                else
                {
                    noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
                    const float wn = float(noise & 0xFFFF) / 32768.0f - 1.0f;
                    if (no.kind == 2)   // timpani: a falling sine and a thump
                        v = std::sin(2 * kPi * no.f * (1.0f + 0.15f * std::exp(-8.0f * t)) * t) * std::exp(-3.0f * t) + wn * std::exp(-30.0f * t) * 0.3f;
                    else   // cymbal: high-passed noise
                    {
                        const float hp = wn - prev;
                        prev = wn;
                        v = hp * std::min(1.0f, t / 0.005f) * std::exp(-2.2f * t) * 0.5f;
                    }
                }
                mix[size_t(i)] += v * no.v;
            }
        }
        float peak = 0.0001f;
        for (float v : mix) peak = std::max(peak, std::fabs(v));
        const float gain = 0.6f / peak;
        std::vector<uint8_t> wav(44 + size_t(n) * 2);
        auto put32 = [&](size_t at, uint32_t v) { memcpy(&wav[at], &v, 4); };
        auto put16 = [&](size_t at, uint16_t v) { memcpy(&wav[at], &v, 2); };
        memcpy(&wav[0], "RIFF", 4); put32(4, uint32_t(36 + n * 2)); memcpy(&wav[8], "WAVEfmt ", 8);
        put32(16, 16); put16(20, 1); put16(22, 1); put32(24, rate); put32(28, rate * 2); put16(32, 2); put16(34, 16);
        memcpy(&wav[36], "data", 4); put32(40, uint32_t(n * 2));
        for (int i = 0; i < n; ++i)
        {
            const float fade = i > n - 2000 ? float(n - i) / 2000.0f : 1.0f;
            put16(44 + size_t(i) * 2, uint16_t(int16_t(std::clamp(mix[size_t(i)] * gain * fade, -1.0f, 1.0f) * 32000.0f)));
        }
        return wav;
    }

    // An original fanfare: a run up, a held chord, two stabs on the flat seventh, the big final chord.
    const std::vector<uint8_t>& Fanfare()
    {
        static std::vector<uint8_t> wav;
        if (!wav.empty()) return wav;
        std::vector<Note> n;
        const int run[] = { 72, 76, 79, 84, 88 };
        for (int i = 0; i < 5; ++i) n.push_back({ i * 0.08f, 0.12f, Midi(run[i]), 0.5f, 0 });
        for (int m : { 60, 64, 67, 72 }) n.push_back({ 0.42f, 0.85f, Midi(m), 0.32f, 0 });
        n.push_back({ 0.42f, 0.85f, Midi(91), 0.45f, 0 });
        n.push_back({ 0.42f, 0.5f, Midi(36), 0.9f, 2 });
        n.push_back({ 0.42f, 0.5f, 0, 0.5f, 3 });
        for (float t : { 1.32f, 1.57f })
        {
            for (int m : { 58, 62, 65, 70 }) n.push_back({ t, 0.18f, Midi(m), 0.32f, 0 });
            n.push_back({ t, 0.18f, Midi(t < 1.4f ? 82 : 86), 0.45f, 0 });
        }
        n.push_back({ 1.57f, 0.2f, Midi(43), 0.6f, 2 });
        for (int m : { 48, 55, 60, 64, 67, 72, 76, 79 }) n.push_back({ 1.82f, 1.7f, Midi(m), 0.3f, 0 });
        n.push_back({ 1.82f, 1.7f, Midi(84), 0.5f, 0 });
        n.push_back({ 1.82f, 1.0f, Midi(36), 1.0f, 2 });
        n.push_back({ 1.82f, 1.0f, 0, 0.8f, 3 });
        const int sparkle[] = { 84, 88, 91, 96 };
        for (int i = 0; i < 4; ++i) n.push_back({ 1.95f + i * 0.1f, 0.1f, Midi(sparkle[i]), 0.22f, 1 });
        wav = Render(n, 4.4f);
        return wav;
    }

    const std::vector<uint8_t>& Chime()
    {
        static std::vector<uint8_t> wav;
        if (!wav.empty()) return wav;
        std::vector<Note> n;
        const int notes[] = { 79, 84, 88, 91 };
        for (int i = 0; i < 4; ++i) n.push_back({ i * 0.11f, 0.1f, Midi(notes[i]), i == 3 ? 0.5f : 0.38f, 1 });
        wav = Render(n, 2.0f);
        return wav;
    }

    void Play(const std::vector<uint8_t>& wav)
    {
        if (!wav.empty()) PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }

    // ----- the ceremony -----

    struct Confetti { float x, y, vx, vy, rot, spin, flip, flipSpeed, w, h, phase; ImU32 col; };
    struct Spark { float x, y, vx, vy, life, max; ImU32 col; };

    struct Ceremony
    {
        bool on = false, spawned = false;
        float t = 0, fade = -1, nextBurst = 0;
        bool keyWas = true;
        VictoryText::Data data;
        std::wstring certificate, desktop;
        std::vector<Confetti> confetti;
        std::vector<Spark> sparks;
        std::vector<std::pair<std::string, std::string>> stats;
    } g_c;

    struct ToastState { bool on = false; float t = 0, dur = 7.0f; std::string title, line; } g_toast;

    constexpr float kAutoEnd = 90.0f, kFadeTime = 1.2f;

    void AddConfetti(float W, float H, float s, bool cannon, float x0)
    {
        Confetti c;
        if (cannon)
        {
            c.x = x0; c.y = H + 10;
            c.vx = (x0 < W * 0.5f ? 1 : -1) * g_rng.r(250, 900) * s;
            c.vy = -g_rng.r(900, 1700) * s;
        }
        else
        {
            c.x = g_rng.r(0, W); c.y = g_rng.r(-H * 0.6f, -10);
            c.vx = g_rng.r(-60, 60) * s; c.vy = g_rng.r(60, 160) * s;
        }
        c.rot = g_rng.r(0, 2 * kPi); c.spin = g_rng.r(-6, 6);
        c.flip = g_rng.r(0, 2 * kPi); c.flipSpeed = g_rng.r(4, 10);
        c.w = g_rng.r(9, 18) * s; c.h = g_rng.r(4, 8) * s; c.phase = g_rng.r(0, 2 * kPi);
        c.col = Hsv(g_rng.f(), 0.75f, 1.0f, 1.0f);
        g_c.confetti.push_back(c);
    }

    void Burst(float W, float H, float s)
    {
        const float x = g_rng.r(W * 0.08f, W * 0.92f), y = g_rng.r(H * 0.06f, H * 0.45f), hue = g_rng.f();
        for (int i = 0; i < 56; ++i)
        {
            const float a = g_rng.r(0, 2 * kPi), sp = g_rng.r(140, 430) * s, life = g_rng.r(0.9f, 1.6f);
            g_c.sparks.push_back({ x, y, std::cos(a) * sp, std::sin(a) * sp, life, life, Hsv(hue + g_rng.r(-0.06f, 0.06f), 0.6f, 1.0f, 1.0f) });
        }
    }

    void UpdateParticles(float dt, float W, float H, float s, bool spawn)
    {
        const float gravity = 900 * s, terminal = 170 * s;
        for (size_t i = 0; i < g_c.confetti.size();)
        {
            Confetti& c = g_c.confetti[i];
            c.vy = std::min(c.vy + gravity * dt, terminal);
            c.vx -= c.vx * 1.4f * dt;
            c.x += (c.vx + std::sin(c.phase + g_c.t * 2.2f) * 40 * s) * dt;
            c.y += c.vy * dt;
            c.rot += c.spin * dt;
            c.flip += c.flipSpeed * dt;
            if (c.y > H + 30)
            {
                if (spawn) { c.x = g_rng.r(0, W); c.y = g_rng.r(-60, -10); c.vy = g_rng.r(60, 160) * s; c.vx = g_rng.r(-60, 60) * s; ++i; }
                else { c = g_c.confetti.back(); g_c.confetti.pop_back(); }
            }
            else ++i;
        }
        for (size_t i = 0; i < g_c.sparks.size();)
        {
            Spark& p = g_c.sparks[i];
            p.vy += 220 * s * dt;
            p.vx -= p.vx * 0.9f * dt;
            p.vy -= p.vy * 0.9f * dt;
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            p.life -= dt;
            if (p.life <= 0) { p = g_c.sparks.back(); g_c.sparks.pop_back(); }
            else ++i;
        }
        if (spawn && g_c.t > 0.8f && g_c.t >= g_c.nextBurst)
        {
            Burst(W, H, s);
            g_c.nextBurst = g_c.t + g_rng.r(0.3f, 0.8f);
        }
    }

    void DrawParticles(ImDrawList* dl, float s)
    {
        for (const Spark& p : g_c.sparks)
        {
            const float k = p.life / p.max;
            const ImU32 col = (p.col & ~IM_COL32_A_MASK) | (uint32_t(255 * Clamp01(k)) << IM_COL32_A_SHIFT);
            dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x - p.vx * 0.05f, p.y - p.vy * 0.05f), col, 1.6f * s);
            dl->AddCircleFilled(ImVec2(p.x, p.y), (1.0f + 2.2f * k) * s, col, 8);
        }
        for (const Confetti& c : g_c.confetti)
        {
            const float face = std::fabs(std::cos(c.flip));
            const float hw = c.w * 0.5f * std::max(face, 0.12f), hh = c.h * 0.5f;
            const float cs = std::cos(c.rot), sn = std::sin(c.rot);
            auto P = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
            ImVec4 col = ImGui::ColorConvertU32ToFloat4(c.col);
            const float shade = 0.55f + 0.45f * face;
            dl->AddQuadFilled(P(-hw, -hh), P(hw, -hh), P(hw, hh), P(-hw, hh), Rgba(col.x * shade, col.y * shade, col.z * shade, 1.0f));
        }
    }

    void DrawTitle(ImDrawList* dl, float W, float H, float T)
    {
        const char* text = "YOU ACTUALLY DID IT!";
        ImFont* f = Big();
        const float size = std::floor(std::min(W * 0.085f, H * 0.15f));
        const float cy = H * 0.22f;
        const float total = Measure(f, size, text).x;
        float x = W * 0.5f - total * 0.5f;
        const float y = cy - size * 0.5f;
        const float p = Clamp01((T - 0.25f) / 0.45f);
        const float scale = 1.0f + 3.0f * (1 - p) * (1 - p) * (1 - p);
        const float shakeAmp = T > 0.7f ? size * 0.04f * std::exp(-(T - 0.7f) * 4.0f) : 0.0f;
        const ImVec2 shake(g_rng.r(-shakeAmp, shakeAmp), g_rng.r(-shakeAmp, shakeAmp));
        const float settle = Clamp01((T - 0.9f) / 0.6f);
        const int start = dl->VtxBuffer.Size;
        int i = 0;
        for (const char* c = text; *c; ++c, ++i)
        {
            const float w = Measure(f, size, c, c + 1).x;
            if (*c != ' ')
            {
                const float bob = std::sin(T * 3.4f + i * 0.5f) * size * 0.045f * settle;
                const ImVec2 at(x, y + bob);
                dl->AddText(f, size, ImVec2(at.x + size * 0.05f, at.y + size * 0.07f), Rgba(0, 0, 0, 0.55f), c, c + 1);
                Outlined(dl, f, size, at, Hsv(T * 0.22f + i * 0.045f, 0.7f, 1.0f, 1.0f), c, size * 0.04f, Rgba(0.05f, 0.02f, 0.0f, 1.0f), c + 1);
                // A white shine sweeping over the letters.
                const float sweep = std::fmod(T * 0.45f, 1.6f) * 1.6f - 0.3f;
                const float d = std::fabs(float(i) / 20.0f - sweep);
                if (d < 0.08f && T > 1.0f) dl->AddText(f, size, at, Rgba(1, 1, 1, (0.08f - d) / 0.08f * 0.65f), c, c + 1);
            }
            x += w;
        }
        Transform(dl, start, ImVec2(W * 0.5f, cy), scale, 0.0f, shake, p);
    }

    void DrawMedal(ImDrawList* dl, float W, float H, float T, float s)
    {
        if (T < 2.0f) return;
        const float R = H * 0.085f, cx = W * 0.5f, cyFinal = H * 0.58f;
        const float drop = Bounce((T - 2.0f) / 0.7f);
        const float cy = -H * 0.3f + (cyFinal + H * 0.3f) * drop;
        const float top = cy - R * 2.15f;
        const float swing = T > 2.7f ? 0.2f * std::exp(-(T - 2.7f) * 1.2f) * std::sin((T - 2.7f) * 5.5f) : 0.0f;
        Glow(dl, ImVec2(cx, cy), R * 2.6f, Rgba(1.0f, 0.8f, 0.25f, 0.35f + 0.1f * std::sin(T * 3)));
        const int start = dl->VtxBuffer.Size;
        // Ribbon: two black-and-red strips meeting at the medal.
        for (int side = -1; side <= 1; side += 2)
        {
            const float sx = float(side);
            const ImVec2 a(cx + sx * R * 0.95f, top), b(cx + sx * R * 0.2f, top), c(cx - sx * R * 0.18f, cy - R * 0.78f), d(cx + sx * R * 0.48f, cy - R * 0.7f);
            dl->AddQuadFilled(a, b, c, d, Rgba(0.72f, 0.06f, 0.08f, 1));
            auto lerp = [](ImVec2 p, ImVec2 q, float k) { return ImVec2(p.x + (q.x - p.x) * k, p.y + (q.y - p.y) * k); };
            dl->AddQuadFilled(lerp(a, b, 0.38f), lerp(a, b, 0.62f), lerp(d, c, 0.62f), lerp(d, c, 0.38f), Rgba(0.04f, 0.04f, 0.04f, 1));
        }
        dl->AddRectFilled(ImVec2(cx - R * 1.08f, top - R * 0.2f), ImVec2(cx + R * 1.08f, top + R * 0.1f), Rgba(0.85f, 0.65f, 0.15f, 1), R * 0.05f);
        dl->AddRect(ImVec2(cx - R * 1.08f, top - R * 0.2f), ImVec2(cx + R * 1.08f, top + R * 0.1f), Rgba(0.45f, 0.3f, 0.05f, 1), R * 0.05f, 0, 2 * s);
        // The disc.
        const ImVec2 c(cx, cy);
        dl->AddCircleFilled(c, R, Rgba(0.5f, 0.33f, 0.04f, 1), 64);
        dl->AddCircleFilled(c, R * 0.9f, Rgba(0.95f, 0.74f, 0.2f, 1), 64);
        for (int i = 0; i < 40; ++i)
        {
            const float a = i * 2 * kPi / 40;
            dl->AddLine(ImVec2(cx + std::cos(a) * R * 0.78f, cy + std::sin(a) * R * 0.78f), ImVec2(cx + std::cos(a) * R * 0.86f, cy + std::sin(a) * R * 0.86f),
                        Rgba(0.6f, 0.42f, 0.08f, 1), 1.5f * s);
        }
        dl->AddCircleFilled(c, R * 0.74f, Rgba(1.0f, 0.84f, 0.32f, 1), 64);
        Star(dl, c, R * 0.62f, R * 0.26f, 0, Rgba(0.62f, 0.42f, 0.05f, 1));
        Star(dl, c, R * 0.53f, R * 0.22f, 0, Rgba(1.0f, 0.93f, 0.5f, 1));
        ImFont* f = Big();
        const float cf = R * 0.34f;
        const ImVec2 cfs = Measure(f, cf, "CF");
        dl->AddText(f, cf, ImVec2(cx - cfs.x * 0.5f, cy - cfs.y * 0.52f), Rgba(0.45f, 0.28f, 0.02f, 1), "CF");
        // A glint running round the rim.
        const float ga = T * 1.7f;
        dl->PathArcTo(c, R * 0.82f, ga, ga + 0.7f, 16);
        dl->PathStroke(Rgba(1, 1, 1, 0.75f), 0, R * 0.06f);
        Sparkle(dl, ImVec2(cx + std::cos(ga + 0.7f) * R * 0.82f, cy + std::sin(ga + 0.7f) * R * 0.82f), R * (0.18f + 0.06f * std::sin(T * 9)),
                Rgba(1, 1, 1, 0.9f));
        Transform(dl, start, ImVec2(cx, top), 1.0f, swing, ImVec2(0, 0));

        if (T > 2.6f)
        {
            const float a = Clamp01((T - 2.6f) / 0.5f);
            Centered(dl, f, std::floor(R * 0.42f), cx, cyFinal + R * 1.12f, Rgba(1.0f, 0.82f, 0.3f, a), "THE ORDER OF THE CLUSTERFUCK", 2.5f * s, Rgba(0, 0, 0, a));
            const std::string who = "Awarded to Sam Fisher, and to " + (g_c.data.profile.empty() ? std::string("you") : U8(g_c.data.profile)) +
                                    ", who held the controller";
            Centered(dl, f, std::floor(R * 0.22f), cx, cyFinal + R * 1.62f, Rgba(1, 1, 1, a * 0.9f), who.c_str(), 1.5f * s, Rgba(0, 0, 0, a));
        }
    }

    void DrawSides(ImDrawList* dl, float W, float H, float T, float s)
    {
        if (T < 3.2f) return;
        ImFont* f = Big();
        const float R = H * 0.085f, cx = W * 0.5f, cy = H * 0.58f;
        const float fs = std::floor(std::max(18.0f, H * 0.027f));
        const float gap = R * 2.9f, lineH = fs * 1.35f;   // clear of the medal's caption
        // Left: the numbers.
        const float rx = cx - gap, lx = rx - fs * 11.0f;
        float y = cy - R * 2.0f;
        for (size_t i = 0; i < g_c.stats.size(); ++i)
        {
            const float a = Clamp01((T - 3.2f - i * 0.22f) / 0.3f);
            if (a <= 0) break;
            const auto& [label, value] = g_c.stats[i];
            const float slide = (1 - EaseOut(a)) * -40 * s;
            Outlined(dl, f, fs * 0.8f, ImVec2(lx + slide, y + fs * 0.15f), Rgba(1.0f, 0.8f, 0.35f, a), label.c_str(), 1.5f * s, Rgba(0, 0, 0, a));
            const ImVec2 vs = Measure(f, fs, value.c_str());
            Outlined(dl, f, fs, ImVec2(rx - vs.x + slide, y), Rgba(1, 1, 1, a), value.c_str(), 2.0f * s, Rgba(0, 0, 0, a));
            y += lineH;
        }
        // Right: the rank and the badges.
        const float bx = cx + gap;
        y = cy - R * 2.0f;
        const float ra = Clamp01((T - 3.4f) / 0.4f);
        if (ra <= 0) return;
        Outlined(dl, f, fs * 0.8f, ImVec2(bx, y), Rgba(1.0f, 0.8f, 0.35f, ra), "RANK", 1.5f * s, Rgba(0, 0, 0, ra));
        y += fs * 0.95f;
        const float rs = fs * (1.25f + 0.15f * (1 - EaseOut(ra)));
        Outlined(dl, f, rs, ImVec2(bx, y), Hsv(0.12f + 0.03f * std::sin(T * 2), 0.65f, 1.0f, ra), U8(g_c.data.rank).c_str(), 2.5f * s, Rgba(0, 0, 0, ra));
        y += rs * 1.45f;
        for (size_t i = 0; i < g_c.data.badges.size(); ++i)
        {
            const float a = Clamp01((T - 3.8f - i * 0.25f) / 0.3f);
            if (a <= 0) break;
            const auto& [name, why] = g_c.data.badges[i];
            Star(dl, ImVec2(bx + fs * 0.4f, y + fs * 0.5f), fs * 0.42f * (0.8f + 0.2f * EaseOut(a)), fs * 0.17f, T * 0.8f + i, Rgba(1.0f, 0.82f, 0.25f, a));
            const std::string n = U8(name);
            Outlined(dl, f, fs, ImVec2(bx + fs * 1.0f, y), Rgba(1, 1, 1, a), n.c_str(), 2.0f * s, Rgba(0, 0, 0, a));
            const float nw = Measure(f, fs, n.c_str()).x;
            Outlined(dl, f, fs * 0.7f, ImVec2(bx + fs * 1.3f + nw, y + fs * 0.24f), Rgba(0.82f, 0.82f, 0.82f, a), U8(why).c_str(), 1.5f * s, Rgba(0, 0, 0, a));
            y += lineH;
        }
    }

    void DrawCeremony(ImDrawList* dl, float dt, float W, float H)
    {
        Ceremony& c = g_c;
        c.t += dt;
        const float T = c.t, s = H / 1080.0f;
        // Enter, Space or Esc (after the first seconds) ends it; so does time.
        const bool front = GetForegroundWindow() == Game::hWnd;
        const bool key = front && ((GetAsyncKeyState(VK_RETURN) | GetAsyncKeyState(VK_SPACE) | GetAsyncKeyState(VK_ESCAPE)) & 0x8000);
        if (c.fade < 0 && ((key && !c.keyWas && T > 4.0f) || T > kAutoEnd)) c.fade = 0;
        c.keyWas = key;
        float alpha = 1.0f;
        if (c.fade >= 0)
        {
            c.fade += dt;
            alpha = 1 - c.fade / kFadeTime;
            if (alpha <= 0)
            {
                c.on = false;
                c.confetti.clear();
                c.sparks.clear();
                if (!c.data.preview) Clusterfuck::Celebrated(c.data.id.c_str());
                LOG("[CLUSTERFUCK] the victory ceremony is over");
                return;
            }
        }
        const int start = dl->VtxBuffer.Size;

        dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), Rgba(0.0f, 0.0f, 0.02f, 0.78f * Clamp01(T / 0.6f)));
        // Sunburst behind the title.
        {
            const ImVec2 o(W * 0.5f, H * 0.24f);
            const float rad = std::sqrt(W * W + H * H), rot = T * 0.12f;
            const float a = 0.22f * Clamp01((T - 0.6f) / 0.8f);
            for (int i = 0; i < 28; ++i)
            {
                const float a0 = rot + i * 2 * kPi / 28, a1 = a0 + kPi / 28;
                GradTri(dl, o, ImVec2(o.x + std::cos(a0) * rad, o.y + std::sin(a0) * rad), ImVec2(o.x + std::cos(a1) * rad, o.y + std::sin(a1) * rad),
                        i % 2 ? Rgba(1.0f, 0.75f, 0.2f, a) : Rgba(1.0f, 0.45f, 0.1f, a), Rgba(1.0f, 0.6f, 0.1f, 0));
            }
            Glow(dl, o, H * 0.45f, Rgba(1.0f, 0.7f, 0.25f, 0.3f * Clamp01((T - 0.6f) / 0.8f)));
        }

        if (T > 0.65f && !c.spawned)   // when the title lands: a rain from the top and two cannons from the corners
        {
            c.spawned = true;
            for (int i = 0; i < 160; ++i) AddConfetti(W, H, s, false, 0);
            for (int i = 0; i < 90; ++i) AddConfetti(W, H, s, true, i % 2 ? 0.0f : W);
        }
        UpdateParticles(dt, W, H, s, c.fade < 0);
        DrawParticles(dl, s);

        DrawTitle(dl, W, H, T);
        ImFont* f = Big();
        const float titleSize = std::floor(std::min(W * 0.085f, H * 0.15f));
        if (T > 1.3f)
        {
            const char* line = "ALL 11 MISSIONS ON CLUSTERFUCK.   ONE LIFE.   NO MEDKITS.   NO MERCY.";
            const int n = std::min(int(strlen(line)), int((T - 1.3f) * 45));
            const float fs = std::floor(std::max(20.0f, titleSize * 0.2f));
            const std::string shown(line, size_t(n));
            const ImVec2 full = Measure(f, fs, line);
            Outlined(dl, f, fs, ImVec2(W * 0.5f - full.x * 0.5f, H * 0.22f + titleSize * 0.62f), Rgba(1, 1, 1, 1), shown.c_str(), 2.0f * s, Rgba(0, 0, 0, 1));
        }
        DrawMedal(dl, W, H, T, s);
        DrawSides(dl, W, H, T, s);

        if (T > 5.0f)
        {
            const float a = Clamp01((T - 5.0f) / 0.6f);
            const float fs = std::floor(std::max(16.0f, H * 0.02f));
            if (!c.certificate.empty())
            {
                std::string where = "Your certificate: " + U8(c.certificate);
                if (!c.desktop.empty()) where += "   (and a copy on your desktop)";
                Centered(dl, f, fs, W * 0.5f, H * 0.885f, Rgba(0.9f, 0.9f, 0.9f, a), where.c_str(), 1.5f * s);
            }
            if (c.data.preview)
                Centered(dl, f, fs, W * 0.5f, H * 0.885f - fs * 1.4f, Rgba(1.0f, 0.55f, 0.3f, a), "PREVIEW: nobody won anything (yet)", 1.5f * s);
            const float blink = 0.55f + 0.45f * std::sin(T * 4.0f);
            Centered(dl, f, std::floor(fs * 1.3f), W * 0.5f, H * 0.925f, Rgba(1.0f, 0.85f, 0.4f, a * blink), "PRESS ENTER", 2.0f * s);
        }

        // Flashes: white at the start, gold when the title lands.
        if (T < 0.45f) dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), Rgba(1, 1, 1, (1 - T / 0.45f) * 0.85f));
        if (T > 0.7f && T < 1.4f) dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), Rgba(1.0f, 0.85f, 0.4f, 0.35f * std::exp(-(T - 0.7f) * 6.0f)));

        if (alpha < 1.0f) Transform(dl, start, ImVec2(0, 0), 1.0f, 0.0f, ImVec2(0, 0), alpha);
    }

    void DrawToast(ImDrawList* dl, float dt, float W, float H)
    {
        ToastState& t = g_toast;
        t.t += dt;
        if (t.t >= t.dur) { t.on = false; return; }
        const float s = H / 1080.0f;
        ImFont* f = Big();
        const float ts = std::floor(std::max(24.0f, H * 0.045f)), ls = std::floor(std::max(15.0f, H * 0.022f));
        const ImVec2 a = Measure(f, ts, t.title.c_str()), b = Measure(f, ls, t.line.c_str());
        const float w = std::max(a.x, b.x) + 60 * s, h = a.y + b.y + 34 * s;
        const float in = EaseOut(t.t / 0.35f), out = EaseOut((t.dur - t.t) / 0.4f);
        const float y = -h + (H * 0.06f + h) * std::min(in, out);
        const float x = W * 0.5f - w * 0.5f;
        const int start = dl->VtxBuffer.Size;
        Glow(dl, ImVec2(W * 0.5f, y + h * 0.5f), w * 0.6f, Rgba(1.0f, 0.75f, 0.2f, 0.25f));
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), Rgba(0.04f, 0.04f, 0.06f, 0.9f), 8 * s);
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), Rgba(1.0f, 0.78f, 0.25f, 1), 8 * s, 0, 2.5f * s);
        Outlined(dl, f, ts, ImVec2(W * 0.5f - a.x * 0.5f, y + 12 * s), Hsv(0.11f + 0.02f * std::sin(t.t * 3), 0.7f, 1.0f, 1), t.title.c_str(), 2 * s, Rgba(0, 0, 0, 1));
        Outlined(dl, f, ls, ImVec2(W * 0.5f - b.x * 0.5f, y + 18 * s + a.y), Rgba(1, 1, 1, 0.95f), t.line.c_str(), 1.5f * s, Rgba(0, 0, 0, 1));
        for (int i = 0; i < 2; ++i)
        {
            const float sx = i ? x + w - 22 * s : x + 22 * s;
            Star(dl, ImVec2(sx, y + h * 0.5f), 12 * s, 5 * s, t.t * (i ? -1.5f : 1.5f), Rgba(1.0f, 0.82f, 0.25f, 1));
        }
        const float fade = std::min(1.0f, (t.dur - t.t) / 0.4f);
        if (fade < 1.0f) Transform(dl, start, ImVec2(0, 0), 1, 0, ImVec2(0, 0), fade);
    }
}

void Victory::AddFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault();   // stays the editor's font
    g_big = nullptr;
    wchar_t win[MAX_PATH] = L"C:\\Windows";
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring path = std::wstring(win) + L"\\Fonts\\impact.ttf";
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) g_big = io.Fonts->AddFontFromFileTTF(U8(path).c_str(), 48.0f);
}

bool Victory::Wants() { return g_c.on || g_toast.on; }

void Victory::Draw(float dt)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    if (size.x < 64 || size.y < 64) return;
    if (g_c.on) DrawCeremony(dl, dt, size.x, size.y);
    if (g_toast.on && !g_c.on) DrawToast(dl, dt, size.x, size.y);
}

void Victory::Celebrate(const VictoryText::Data& data, const std::wstring& certificate, const std::wstring& desktop)
{
    g_c = Ceremony();
    g_c.on = true;
    g_c.data = data;
    g_c.certificate = certificate;
    g_c.desktop = desktop;
    g_rng.s ^= GetTickCount();
    const auto& d = g_c.data;
    auto num = [](int n) { return std::to_string(n); };
    g_c.stats = {
        { "MISSION TIME", d.timeKnown ? U8(VictoryText::Clock(d.totalTime)) : std::string("?") },
        { "KILLS", num(d.tot[VictoryText::Killed]) },
        { "KNOCKOUTS", num(d.tot[VictoryText::Knocked]) },
        { "BODIES FOUND", num(d.tot[VictoryText::BodiesFound]) },
        { "TIMES SPOTTED", num(d.tot[VictoryText::Identified]) },
        { "LIVES IT TOOK", num(d.lives) } };
    if (!d.span.empty()) g_c.stats.push_back({ "START TO FINISH", U8(d.span) });
    g_toast.on = false;
    Play(Fanfare());
    LOG("[CLUSTERFUCK] the victory ceremony begins%s", d.preview ? " (preview)" : "");
}

void Victory::Toast(const std::wstring& title, const std::wstring& line, bool chime)
{
    g_toast = ToastState();
    g_toast.on = true;
    g_toast.title = U8(title);
    g_toast.line = U8(line);
    g_toast.dur = 7.0f;
    if (chime && !g_c.on) Play(Chime());
}
