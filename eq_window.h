#pragma once

#include <atomic>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

constexpr int kParametricEqBandCount = 6;

extern const float g_parametricEqFrequenciesHz[kParametricEqBandCount];
extern std::atomic<float> g_parametricEqGainDb[kParametricEqBandCount];
extern std::atomic<float> g_parametricEqQ[kParametricEqBandCount];

void ShowParametricEqWindow(HWND owner);
void StoreParametricEqBand(int band, float gainDb, float q);
