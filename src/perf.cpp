// Frame profiler (diagnostics): every second in which the frame rate dips, one [PERF] line is logged
// with where the slowest frame's time went - game logic, GPU waits (Present, occlusion query polling,
// render target readback), draw calls and resources created (streaming / shader compilation).
#include "pch.h"
#include "features.h"
#include "perf.h"

namespace
{
    LARGE_INTEGER g_freq{};
    double ToMs(int64_t ticks) { return double(ticks) * 1000.0 / double(g_freq.QuadPart); }
    int64_t Now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }

    struct Frame
    {
        int64_t total = 0, engineTick = 0, actors = 0, present = 0, queryWait = 0, readback = 0;
        uint32_t draws = 0, creates = 0, shaders = 0, queryPolls = 0, queryBusy = 0, readbacks = 0;
    };
    Frame g_cur, g_worst;
    int64_t g_frameStart = 0, g_secondStart = 0, g_tickStart = 0;
    uint32_t g_frames = 0, g_slow = 0;
    double g_targetMs = 0.0;
    bool g_enabled = false;
}

void Perf::Init()
{
    g_enabled = g_settings.perfLog;
    QueryPerformanceFrequency(&g_freq);
    if (g_enabled)
        LOG("Perf: frame profiler on (a [PERF] line is written for every second with frame drops)");
}

bool Perf::Enabled() { return g_enabled; }
int64_t Perf::Begin() { return g_enabled ? Now() : 0; }

void Perf::SetTarget(double ms) { g_targetMs = ms; }

void Perf::FrameStart() { if (g_enabled) g_frameStart = Now(); }

void Perf::FrameEnd()
{
    if (!g_enabled || !g_frameStart)
        return;
    const int64_t now = Now();
    g_cur.total = now - g_frameStart;
    if (g_cur.total > g_worst.total)
        g_worst = g_cur;
    ++g_frames;
    const double target = g_targetMs > 0.0 ? g_targetMs : 1000.0 / 60.0;
    if (ToMs(g_cur.total) > target * 1.5)
        ++g_slow;
    g_cur = Frame{};

    if (!g_secondStart)
        g_secondStart = now;
    const double elapsed = ToMs(now - g_secondStart);
    if (elapsed >= 1000.0)
    {
        const double fps = g_frames * 1000.0 / elapsed;
        const double expected = g_targetMs > 0.0 ? 1000.0 / g_targetMs : 0.0;
        if (g_slow > 0 && (expected == 0.0 || fps < expected * 0.95 || ToMs(g_worst.total) > target * 2.5))
        {
            const Frame& w = g_worst;
            LOG("[PERF] %.0f fps (%u slow of %u) | worst %.1f ms: engine %.1f (actors %.1f) present %.1f query-wait %.1f (%u polls, %u busy) readback %.1f (%u) | %u draws, %u new resources, %u new shaders",
                fps, g_slow, g_frames, ToMs(w.total), ToMs(w.engineTick), ToMs(w.actors), ToMs(w.present), ToMs(w.queryWait),
                w.queryPolls, w.queryBusy, ToMs(w.readback), w.readbacks, w.draws, w.creates, w.shaders);
        }
        g_frames = g_slow = 0;
        g_worst = Frame{};
        g_secondStart = now;
    }
}

void Perf::EngineTickBegin() { if (g_enabled) g_tickStart = Now(); }
void Perf::EngineTickEnd() { if (g_enabled && g_tickStart) g_cur.engineTick += Now() - g_tickStart; }
void Perf::AddActors(int64_t start) { if (start) g_cur.actors += Now() - start; }
void Perf::AddPresent(int64_t start) { if (start) g_cur.present += Now() - start; }
void Perf::AddQueryWait(int64_t start, bool busy) { if (start) { g_cur.queryWait += Now() - start; ++g_cur.queryPolls; g_cur.queryBusy += busy; } }
void Perf::AddReadback(int64_t start) { if (start) { g_cur.readback += Now() - start; ++g_cur.readbacks; } }
void Perf::CountDraw() { ++g_cur.draws; }
void Perf::CountCreate() { ++g_cur.creates; }
void Perf::CountShader() { ++g_cur.shaders; }
