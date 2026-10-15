#pragma once
#include "WindSystem.h"

namespace renderer
{
// Reader returns an integer for (key, default) in [Wind]. Shared by the live
// location-aware INI path and portable configuration/menu regressions.
template<class Reader> WindSettings ReadWindSettings(Reader read)
{
    WindSettings s;
    s.enabled = read(L"Enabled", 1) != 0;
    s.dynamic = read(L"Dynamic", 1) != 0;
    s.direction = static_cast<WindDirection>(std::clamp(read(L"Direction", 1), 0, 7));
    s.strength = std::clamp(read(L"StrengthPercent", 100), 0, 200) * .01f;
    s.gustAmount = std::clamp(read(L"GustPercent", 35), 0, 100) * .01f;
    s.gustSeconds = float(std::clamp(read(L"GustSeconds", 18), 2, 120));
    s.directionSeconds = float(std::clamp(read(L"DirectionChangeSeconds", 120), 15, 600));
    s.transitionSeconds = float(std::clamp(read(L"TransitionSeconds", 30), 1, 120));
    s.weatherResponse = std::clamp(read(L"WeatherResponsePercent", 100), 0, 200) * .01f;
    s.affectWaves = read(L"AffectWaves", 1) != 0;
    s.affectClouds = read(L"AffectClouds", 1) != 0;
    s.debugView = read(L"DebugView", 0) != 0;
    return s;
}
}
