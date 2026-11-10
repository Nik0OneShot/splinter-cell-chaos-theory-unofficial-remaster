#include "pch.h"

Settings g_settings;

namespace
{
    std::wstring g_path;

    std::wstring Trim(std::wstring s)
    {
        // Strip trailing comments ("; ..." or "// ...") and whitespace.
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == L';' || (s[i] == L'/' && i + 1 < s.size() && s[i + 1] == L'/'))
            {
                s.resize(i);
                break;
            }
        }
        const auto first = s.find_first_not_of(L" \t\r\n\"");
        if (first == std::wstring::npos)
            return L"";
        const auto last = s.find_last_not_of(L" \t\r\n\"");
        return s.substr(first, last - first + 1);
    }

    std::wstring Raw(const wchar_t* sec, const wchar_t* key)
    {
        wchar_t buf[512] = {};
        GetPrivateProfileStringW(sec, key, L"\x01", buf, 512, g_path.c_str());
        if (buf[0] == 1)
            return L"\x01";
        return Trim(buf);
    }

    int GetInt(const wchar_t* sec, const wchar_t* key, int def)
    {
        auto v = Raw(sec, key);
        if (v == L"\x01" || v.empty())
            return def;
        return static_cast<int>(wcstol(v.c_str(), nullptr, 0));
    }

    float GetFloat(const wchar_t* sec, const wchar_t* key, float def)
    {
        auto v = Raw(sec, key);
        if (v == L"\x01" || v.empty())
            return def;
        return static_cast<float>(_wtof(v.c_str()));
    }

    bool GetBool(const wchar_t* sec, const wchar_t* key, bool def)
    {
        auto v = Raw(sec, key);
        if (v == L"\x01" || v.empty())
            return def;
        if (_wcsicmp(v.c_str(), L"true") == 0 || _wcsicmp(v.c_str(), L"yes") == 0 || _wcsicmp(v.c_str(), L"on") == 0)
            return true;
        if (_wcsicmp(v.c_str(), L"false") == 0 || _wcsicmp(v.c_str(), L"no") == 0 || _wcsicmp(v.c_str(), L"off") == 0)
            return false;
        return wcstol(v.c_str(), nullptr, 0) != 0;
    }

    std::wstring GetString(const wchar_t* sec, const wchar_t* key, const wchar_t* def)
    {
        auto v = Raw(sec, key);
        if (v == L"\x01")
            return def;
        return v;
    }
}

void Config::Load(const std::wstring& iniPath)
{
    g_path = iniPath;
    auto& s = g_settings;

    s.width               = GetInt  (L"Display", L"Width", s.width);
    s.height              = GetInt  (L"Display", L"Height", s.height);
    s.widescreen          = GetBool (L"Display", L"Widescreen", s.widescreen);
    s.hudWidescreenMode   = GetBool (L"Display", L"HudWidescreenMode", s.hudWidescreenMode);
    s.hudEdgeOffset       = GetInt  (L"Display", L"HudEdgeOffset", s.hudEdgeOffset);
    s.fovMultiplier       = GetFloat(L"Display", L"FOVMultiplier", s.fovMultiplier);
    s.unlockResolutions   = GetBool (L"Display", L"UnlockResolutions", s.unlockResolutions);
    s.highResVideos       = GetBool (L"Display", L"HighResVideos", s.highResVideos);
    s.fmvAspect           = GetInt  (L"Display", L"FMVAspect", s.fmvAspect);
    s.skipIntro           = GetBool (L"Display", L"SkipIntro", s.skipIntro);
    s.disableSafeMode     = GetBool (L"Display", L"DisableSafeMode", s.disableSafeMode);
    s.windowedAltTabFix   = GetBool (L"Display", L"WindowedAltTabFix", s.windowedAltTabFix);
    s.steamOverlayFix     = GetBool (L"Display", L"SteamOverlayFix", s.steamOverlayFix);
    s.shadowMapResolution = GetInt  (L"Display", L"ShadowMapResolution", s.shadowMapResolution);
    s.shadowFiltering     = GetBool (L"Display", L"ShadowFiltering", s.shadowFiltering);
    s.gameLanguage        = GetInt  (L"Display", L"GameLanguage", s.gameLanguage);
    s.refreshRate         = GetInt  (L"Display", L"RefreshRate", s.refreshRate);
    s.windowMode          = GetInt  (L"Display", L"WindowMode", s.windowMode);
    s.skippableVideos     = GetBool (L"Display", L"SkippableVideos", s.skippableVideos);
    s.menuCursorSpeed     = GetFloat(L"Mouse", L"MenuCursorSpeed", s.menuCursorSpeed);
    s.recaptureMouse      = GetBool (L"Mouse", L"RecaptureOnFocus", s.recaptureMouse);
    s.opticCableSpeed     = GetFloat(L"Mouse", L"OpticCableSpeed", s.opticCableSpeed);
    s.aimAccuracyFix      = GetBool (L"Mouse", L"AimAccuracyFix", s.aimAccuracyFix);
    s.perfectStillAim     = GetBool (L"Mouse", L"PerfectStillAim", s.perfectStillAim);
    s.aimFreeTurnSpeed    = std::clamp(GetFloat(L"Mouse", L"AimFreeTurnSpeed", s.aimFreeTurnSpeed), 0.0f, 10000.0f);
    s.aimFullTurnSpeed    = std::clamp(GetFloat(L"Mouse", L"AimFullTurnSpeed", s.aimFullTurnSpeed), 1.0f, 20000.0f);
    s.aimLog              = GetBool (L"Mouse", L"AimLog", s.aimLog);
    s.stretchLoadingScreens = GetBool(L"Display", L"StretchLoadingScreens", s.stretchLoadingScreens);
    s.vsync               = GetInt  (L"Framerate", L"VSync", s.vsync);

    s.fpsLimit             = GetInt  (L"Framerate", L"FPSLimit", s.fpsLimit);
    s.logicRate            = GetInt  (L"Framerate", L"LogicRate", s.logicRate);
    s.throttleAI           = GetBool (L"Framerate", L"ThrottleAI", s.throttleAI);
    s.throttleNPCs         = GetBool (L"Framerate", L"ThrottleNPCs", s.throttleNPCs);
    s.throttlePhysicsProps = GetBool (L"Framerate", L"ThrottlePhysicsProps", s.throttlePhysicsProps);
    s.maxFrameTime         = GetFloat(L"Framerate", L"MaxFrameTime", s.maxFrameTime);
    s.preciseLimiter       = GetBool (L"Framerate", L"PreciseLimiter", s.preciseLimiter);
    s.refreshSync          = GetBool (L"Framerate", L"RefreshSync", s.refreshSync);

    s.rawInput       = GetBool (L"Mouse", L"RawInput", s.rawInput);
    s.rawSensitivity = GetFloat(L"Mouse", L"Sensitivity", s.rawSensitivity);

    s.amdThermalFix     = GetBool  (L"Graphics", L"AMDThermalFix", s.amdThermalFix);
    s.shaderReplaceMask = GetInt   (L"Graphics", L"ShaderReplaceMask", s.shaderReplaceMask);
    s.spoofVendor       = GetInt   (L"Graphics", L"SpoofGPUVendor", s.spoofVendor);
    s.shaderModel       = GetInt   (L"Graphics", L"ShaderModel", s.shaderModel);
    s.shaderPack        = GetBool  (L"Graphics", L"ShaderPack", s.shaderPack);
    s.shaderPackToggleKey = GetInt (L"Graphics", L"ShaderPackToggleKey", s.shaderPackToggleKey);
    s.shaderHighlightKey  = GetInt (L"Graphics", L"ShaderHighlightKey", s.shaderHighlightKey);
    s.d3d9Chain         = GetString(L"Graphics", L"D3D9Chain", L"");
    s.logShaders        = GetBool  (L"Graphics", L"LogShaders", s.logShaders);
    s.logRenderTargets  = GetBool  (L"Graphics", L"LogRenderTargets", s.logRenderTargets);

    s.eaxFix  = GetBool(L"Audio", L"EAXFix", s.eaxFix);
    s.useXidi = GetBool(L"Controller", L"UseXidi", s.useXidi);
    s.unstickPlayer = GetBool(L"Gameplay", L"UnstickPlayer", s.unstickPlayer);
    s.rescueStuckNPCs = GetBool(L"Gameplay", L"RescueStuckNPCs", s.rescueStuckNPCs);
    s.fixStuckDoors = GetBool(L"Gameplay", L"FixStuckDoors", s.fixStuckDoors);
    s.wallBounce = GetBool(L"Gameplay", L"WallBounce", s.wallBounce);
    s.wallBounceUp = GetFloat(L"Gameplay", L"WallBounceUp", s.wallBounceUp);
    s.wallBounceOut = GetFloat(L"Gameplay", L"WallBounceOut", s.wallBounceOut);
    s.wallBounceReach = GetFloat(L"Gameplay", L"WallBounceReach", s.wallBounceReach);
    s.wallBounceMax = GetInt(L"Gameplay", L"WallBounceMax", s.wallBounceMax);
    s.wallBounceSplit = GetBool(L"Gameplay", L"WallBounceSplit", s.wallBounceSplit);
    s.splitFromFall = GetBool(L"Gameplay", L"SplitFromFall", s.splitFromFall);
    s.wallBounceSplitHold = GetFloat(L"Gameplay", L"WallBounceSplitHold", s.wallBounceSplitHold);
    s.splitCatchDamage = GetFloat(L"Gameplay", L"SplitCatchDamage", s.splitCatchDamage);
    s.wallBounceAnim = GetString(L"Gameplay", L"WallBounceAnim", L"SpltStSpBg1");
    s.wallBounceAnimTime = GetFloat(L"Gameplay", L"WallBounceAnimTime", s.wallBounceAnimTime);
    {
        const std::wstring leg = GetString(L"Gameplay", L"WallBounceLeg", L"Auto");
        s.wallBounceLeg = !_wcsicmp(leg.c_str(), L"Swap") ? 1 : !_wcsicmp(leg.c_str(), L"Both") ? 2 : 0;
    }
    s.wallBounceKickStrength = GetFloat(L"Gameplay", L"WallBounceKickStrength", s.wallBounceKickStrength);
    s.wallBounceFrontAnimR = GetString(L"Gameplay", L"WallBounceFrontAnimRight", L"ledgcralftr");
    s.wallBounceFrontAnimL = GetString(L"Gameplay", L"WallBounceFrontAnimLeft", L"ledgcralftl");
    s.wallBounceFrontFrame = GetFloat(L"Gameplay", L"WallBounceFrontFrame", s.wallBounceFrontFrame);
    s.wallBounceFrontStrength = GetFloat(L"Gameplay", L"WallBounceFrontStrength", s.wallBounceFrontStrength);
    s.wallBounceBoost = GetFloat(L"Gameplay", L"WallBounceBoost", s.wallBounceBoost);
    s.wallBounceSteerTime = GetFloat(L"Gameplay", L"WallBounceSteerTime", s.wallBounceSteerTime);
    s.wallBounceMinSpeed = GetFloat(L"Gameplay", L"WallBounceMinSpeed", s.wallBounceMinSpeed);
    s.wallBounceStanding = GetBool(L"Gameplay", L"WallBounceStanding", s.wallBounceStanding);
    s.wallBounceStandingUp = GetFloat(L"Gameplay", L"WallBounceStandingUp", s.wallBounceStandingUp);
    s.rollOnlyWhenCrouching = GetBool(L"Gameplay", L"RollOnlyWhenCrouching", s.rollOnlyWhenCrouching);
    s.climbOverRailings = GetBool(L"Gameplay", L"ClimbOverRailings", s.climbOverRailings);
    s.crouchNoiseFix = GetBool(L"Gameplay", L"CrouchNoiseFix", s.crouchNoiseFix);
    s.quietStandingSteps = GetBool(L"Gameplay", L"QuietStandingSteps", s.quietStandingSteps);
    s.crouchInstantSwitch = GetBool(L"Gameplay", L"CrouchInstantSwitch", s.crouchInstantSwitch);
    s.logCrouch = GetBool(L"Debug", L"LogCrouch", s.logCrouch);
    s.logCursor = GetBool(L"Debug", L"LogCursor", s.logCursor);
    s.hackingClickFix = GetBool(L"Mouse", L"HackingClickFix", s.hackingClickFix);
    s.wallBounceNoise = GetBool(L"Gameplay", L"WallBounceNoise", s.wallBounceNoise);
    s.evenDiagonalSpeed = GetBool(L"Gameplay", L"EvenDiagonalSpeed", s.evenDiagonalSpeed);
    s.speedStages   = GetInt (L"Gameplay", L"SpeedStages", s.speedStages);
    s.guardPersonalities = GetBool(L"Gameplay", L"GuardPersonalities", s.guardPersonalities);
    s.detectionBaseline  = GetInt (L"Gameplay", L"DetectionBaseline", s.detectionBaseline);
    s.fixSoloDifficulty  = GetBool(L"Gameplay", L"FixSoloDifficulty", s.fixSoloDifficulty);
    s.bathhouseEndingDelay = GetFloat(L"Gameplay", L"BathhouseEndingDelay", s.bathhouseEndingDelay);
    s.fairHacking = GetBool(L"Gameplay", L"FairHacking", s.fairHacking);
    s.clusterfuck = GetBool(L"Gameplay", L"Clusterfuck", s.clusterfuck);
    s.clusterfuckDesktop = GetBool(L"Gameplay", L"ClusterfuckCertificateOnDesktop", s.clusterfuckDesktop);
    s.nikoPlush = GetInt(L"Gameplay", L"NikoPlush", s.nikoPlush);
    s.nikoPlushVolume = GetInt(L"Gameplay", L"NikoPlushVolume", s.nikoPlushVolume);
    s.nikoPlushNoise = GetFloat(L"Gameplay", L"NikoPlushNoise", s.nikoPlushNoise);
    s.nikoPlushMinLight = GetFloat(L"Gameplay", L"NikoPlushMinLight", s.nikoPlushMinLight);
    s.nikoPlushFullLight = std::max(1.0f, GetFloat(L"Gameplay", L"NikoPlushFullLight", s.nikoPlushFullLight));
    s.nikoPlushHdrScale = GetFloat(L"Gameplay", L"NikoPlushHdrScale", s.nikoPlushHdrScale);
    s.nikoPlushDebug = GetInt(L"Debug", L"NikoPlushDebug", s.nikoPlushDebug);
    s.bathhouseSpawnClearance = GetFloat(L"Gameplay", L"BathhouseSpawnClearance", s.bathhouseSpawnClearance);

    s.mods        = GetBool(L"Mods", L"Mods", s.mods);
    s.logModFiles = GetBool(L"Mods", L"LogModFiles", s.logModFiles);
    s.texturePacks = GetBool(L"Mods", L"TexturePacks", s.texturePacks);
    s.levelChanges = GetBool(L"Mods", L"LevelChanges", s.levelChanges);
    s.levelDumpKey = GetInt(L"Mods", L"LevelDumpKey", s.levelDumpKey);
    s.editorKey = GetInt(L"Editor", L"EditorKey", s.editorKey);
    s.editorPause = GetBool(L"Editor", L"EditorPause", s.editorPause);
    s.editorScale = GetFloat(L"Editor", L"UIScale", s.editorScale);
    s.editorFolder = GetString(L"Editor", L"SaveFolder", s.editorFolder.c_str());
    s.dumpTextures = GetBool(L"Mods", L"DumpTextures", s.dumpTextures);
    s.dumpTexturesKey = GetInt(L"Mods", L"DumpTexturesKey", s.dumpTexturesKey);
    {
        wchar_t fmt[16] = {};
        GetPrivateProfileStringW(L"Mods", L"DumpFormat", L"png", fmt, 16, g_path.c_str());
        s.dumpTexturesDDS = _wcsnicmp(fmt, L"dds", 3) == 0;
    }
    s.scripts     = GetBool(L"Mods", L"Scripts", s.scripts);
    s.reloadScriptsKey = GetInt(L"Mods", L"ReloadScriptsKey", s.reloadScriptsKey);

    s.logLevel        = GetInt (L"Debug", L"LogLevel", s.logLevel);
    s.logActorClasses = GetBool(L"Debug", L"LogActorClasses", s.logActorClasses);
    s.captureKey      = GetInt (L"Debug", L"CaptureKey", s.captureKey);
    s.autoCaptureAt   = GetFloat(L"Debug", L"AutoCaptureAt", s.autoCaptureAt);
    s.logShaderInputs = GetBool(L"Debug", L"LogShaderInputs", s.logShaderInputs);
    s.perfLog         = GetBool(L"Debug", L"PerfLog", s.perfLog);
    s.stuckDiagnostics = GetBool(L"Debug", L"StuckDiagnostics", s.stuckDiagnostics);
    s.logHudImages    = GetBool(L"Debug", L"LogHudImages", s.logHudImages);
    s.watchVisCrouchMul = GetBool(L"Debug", L"WatchVisCrouchMul", s.watchVisCrouchMul);
    s.saveAudit       = GetBool(L"Debug", L"SaveAudit", s.saveAudit);
    s.slowKey         = GetInt (L"Debug", L"SlowKey", s.slowKey);

    // Sanity limits
    s.logicRate = std::clamp(s.logicRate, 0, 1000);
    if (s.logicRate > 0 && s.logicRate < 20)
        s.logicRate = 20;
    s.maxFrameTime = std::clamp(s.maxFrameTime, 0.02f, 0.5f);
    s.fovMultiplier = std::clamp(s.fovMultiplier, 0.5f, 2.0f);
    s.rawSensitivity = std::clamp(s.rawSensitivity, 0.01f, 100.0f);
    s.hudEdgeOffset = std::clamp(s.hudEdgeOffset, 0, 400);
    s.windowMode = std::clamp(s.windowMode, 0, 2);
    s.menuCursorSpeed = std::clamp(s.menuCursorSpeed, 0.0f, 10.0f);
    s.opticCableSpeed = std::clamp(s.opticCableSpeed, 0.0f, 10.0f);
    s.detectionBaseline = std::clamp(s.detectionBaseline, 0, 3);
    if (s.speedStages != 0)
        s.speedStages = std::clamp(s.speedStages, 2, 41);
}

void Config::Save(const wchar_t* section, const wchar_t* key, const wchar_t* value)
{
    if (!g_path.empty()) WritePrivateProfileStringW(section, key, value, g_path.c_str());
}

void Config::Dump()
{
    const auto& s = g_settings;
    LOG("Config: Display %dx%d widescreen=%d hudMode=%d hudEdge=%d fovMul=%.2f unlockRes=%d bink=%d skipIntro=%d safeModeOff=%d altTab=%d shadowRes=%d shadowFilter=%d lang=%d",
        s.width, s.height, s.widescreen, s.hudWidescreenMode, s.hudEdgeOffset, s.fovMultiplier, s.unlockResolutions,
        s.highResVideos, s.skipIntro, s.disableSafeMode, s.windowedAltTabFix, s.shadowMapResolution, s.shadowFiltering, s.gameLanguage);
    LOG("Config: Framerate fpsLimit=%d logicRate=%d throttle(AI=%d NPC=%d props=%d) maxFrameTime=%.3f precise=%d vsync=%d refresh=%d",
        s.fpsLimit, s.logicRate, s.throttleAI, s.throttleNPCs, s.throttlePhysicsProps, s.maxFrameTime, s.preciseLimiter, s.vsync, s.refreshRate);
    LOG("Config: WindowMode=%d SkippableVideos=%d MenuCursorSpeed=%.2f UnstickPlayer=%d EvenDiagonalSpeed=%d SpeedStages=%d",
        s.windowMode, s.skippableVideos, s.menuCursorSpeed, s.unstickPlayer, s.evenDiagonalSpeed, s.speedStages);
    LOG("Config: Mouse raw=%d sens=%.3f | Graphics amdFix=%d mask=%d spoof=%d chain='%ls' | EAX=%d Xidi=%d",
        s.rawInput, s.rawSensitivity, s.amdThermalFix, s.shaderReplaceMask, s.spoofVendor, s.d3d9Chain.c_str(), s.eaxFix, s.useXidi);
}
