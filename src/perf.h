#pragma once

#include <cstdint>

namespace Perf
{
    void Init();
    bool Enabled();
    int64_t Begin();              // returns a timestamp (0 when profiling is off)
    void SetTarget(double ms);    // frame time the limiter aims for
    void FrameStart();            // after the limiter's wait
    void FrameEnd();              // before the limiter's wait
    void EngineTickBegin();
    void EngineTickEnd();
    void AddActors(int64_t start);
    void AddPresent(int64_t start);
    void AddQueryWait(int64_t start, bool busy);
    void AddReadback(int64_t start);
    void CountDraw();
    void CountCreate();
    void CountShader();
}
