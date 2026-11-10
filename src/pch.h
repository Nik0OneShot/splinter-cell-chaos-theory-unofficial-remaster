#pragma once

// Target: 32-bit Windows, Splinter Cell Chaos Theory 1.05 (Steam build, image base 0x10900000).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <climits>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "log.h"
#include "config.h"
#include "memory.h"
