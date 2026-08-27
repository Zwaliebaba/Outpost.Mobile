#pragma once

#include <windows.h>

// Every value that changes what the toy looks like or how it feels. The list is written once and
// expanded twice -- into the fields of Tuning below, and into the table in Tuning.cpp that both
// parses tuning.ini and builds the Tuner window -- so the two can never drift apart.
//
//   X(section, ini key, field, default, slider min, slider max)
//
// The slider bounds only bound the slider. A value hand-edited into tuning.ini is taken as written,
// even outside them.
#define TUNING_FIELDS(X)                                                                                                   \
  X("camera",      "pitchDeg",                cameraPitchDeg,             52.f,   5.f,    89.f)    \
  X("camera",      "yawDeg",                  cameraYawDeg,               0.f,    -180.f, 180.f)   \
  X("camera",      "distance",                cameraDistance,             190.f,  10.f,   1200.f)  \
  X("camera",      "minZoom",                 cameraMinZoom,              40.f,   5.f,    400.f)   \
  X("camera",      "maxZoom",                 cameraMaxZoom,              900.f,  50.f,   3000.f)  \
  X("camera",      "targetHeight",            cameraTargetHeight,         3.f,    0.f,    120.f)   \
  X("camera",      "fovDeg",                  cameraFovDeg,               45.f,   15.f,   100.f)   \
  X("camera",      "nearPlane",               cameraNearPlane,            0.5f,   0.05f,  10.f)    \
  X("camera",      "farPlane",                cameraFarPlane,             8000.f, 500.f,  30000.f) \
  X("camera",      "panSpeed",                cameraPanSpeed,             1.f,    0.05f,  6.f)     \
  X("camera",      "followHalfLife",          cameraFollowHalfLife,       0.18f,  0.01f,  2.f)     \
  X("camera",      "leadFactor",              cameraLeadFactor,           0.35f,  0.f,    3.f)     \
  X("camera",      "shakeAmplitude",          cameraShakeAmplitude,       2.5f,   0.f,    30.f)    \
  X("camera",      "shakeDecayHalfLife",      cameraShakeDecayHalfLife,   0.15f,  0.02f,  2.f)     \
  X("camera",      "rotateSpeedDegPerPx",     cameraRotateSpeedDegPerPx,  0.35f,  0.02f,  2.f)     \
  X("camera",      "zoomStepFactor",          cameraZoomStepFactor,       1.12f,  1.01f,  2.f)     \
                                                                                                   \
  X("selection",   "ringFadeInMs",            selRingFadeInMs,            140.f,  10.f,   1500.f)  \
  X("selection",   "ringFadeOutMs",           selRingFadeOutMs,           180.f,  10.f,   1500.f)  \
  X("selection",   "ringScaleOvershoot",      selRingScaleOvershoot,      1.35f,  1.f,    3.f)     \
  X("selection",   "overshootSettleHalfLife", selOvershootSettleHalfLife, 0.09f,  0.01f,  1.f)     \
  X("selection",   "hoverHighlightStrength",  selHoverHighlightStrength,  0.45f,  0.f,    2.f)     \
  X("selection",   "hoverRingAlpha",          selHoverRingAlpha,          0.35f,  0.f,    1.f)     \
  X("selection",   "ringThickness",           selRingThickness,           0.16f,  0.01f,  0.6f)    \
  X("selection",   "ringRadiusScale",         selRingRadiusScale,         1.25f,  0.5f,   4.f)     \
  X("selection",   "ringColourR",             selRingColourR,             0.35f,  0.f,    1.f)     \
  X("selection",   "ringColourG",             selRingColourG,             0.95f,  0.f,    1.f)     \
  X("selection",   "ringColourB",             selRingColourB,             0.55f,  0.f,    1.f)     \
  X("selection",   "ringColourA",             selRingColourA,             0.9f,   0.f,    1.f)     \
                                                                                                   \
  X("orderMarker", "expandMs",                markerExpandMs,             160.f,  10.f,   1500.f)  \
  X("orderMarker", "pulseCount",              markerPulseCount,           3.f,    0.f,    10.f)    \
  X("orderMarker", "pulsePeriodMs",           markerPulsePeriodMs,        320.f,  40.f,   2000.f)  \
  X("orderMarker", "lifetimeMs",              markerLifetimeMs,           1400.f, 100.f,  8000.f)  \
  X("orderMarker", "fadeOutMs",               markerFadeOutMs,            260.f,  10.f,   2000.f)  \
  X("orderMarker", "radius",                  markerRadius,               9.f,    1.f,    60.f)    \
  X("orderMarker", "thickness",               markerThickness,            0.18f,  0.01f,  0.6f)    \
  X("orderMarker", "colourR",                 markerColourR,              0.95f,  0.f,    1.f)     \
  X("orderMarker", "colourG",                 markerColourG,              0.78f,  0.f,    1.f)     \
  X("orderMarker", "colourB",                 markerColourB,              0.28f,  0.f,    1.f)     \
  X("orderMarker", "colourA",                 markerColourA,              0.9f,   0.f,    1.f)     \
                                                                                                   \
  X("shipMotion",  "maxSpeed",                motionMaxSpeed,             34.f,   1.f,    300.f)   \
  X("shipMotion",  "acceleration",            motionAcceleration,         26.f,   1.f,    400.f)   \
  X("shipMotion",  "deceleration",            motionDeceleration,         34.f,   1.f,    400.f)   \
  X("shipMotion",  "turnRateDegPerSec",       motionTurnRateDegPerSec,    70.f,   5.f,    720.f)   \
  X("shipMotion",  "turnAcceleration",        motionTurnAcceleration,     240.f,  10.f,   3000.f)  \
  X("shipMotion",  "arrivalRadius",           motionArrivalRadius,        3.5f,   0.2f,   60.f)    \
  X("shipMotion",  "stopDampingHalfLife",     motionStopDampingHalfLife,  0.16f,  0.01f,  2.f)     \
  X("shipMotion",  "formationSpacing",        motionFormationSpacing,     34.f,   5.f,    250.f)   \
  X("shipMotion",  "formationShape",          motionFormationShape,       1.f,    0.f,    3.f)     \
                                                                                                   \
  X("banking",     "maxBankAngleDeg",         bankMaxAngleDeg,            28.f,   0.f,    80.f)    \
  X("banking",     "bankResponseHalfLife",    bankResponseHalfLife,       0.14f,  0.01f,  1.5f)    \
  X("banking",     "bankReturnHalfLife",      bankReturnHalfLife,         0.3f,   0.01f,  2.f)     \
                                                                                                   \
  X("thrusters",   "idleIntensity",           thrusterIdleIntensity,      0.12f,  0.f,    1.f)     \
  X("thrusters",   "maxIntensity",            thrusterMaxIntensity,       1.f,    0.f,    4.f)     \
  X("thrusters",   "responseHalfLife",        thrusterResponseHalfLife,   0.1f,   0.01f,  1.f)     \
  X("thrusters",   "trailLength",             thrusterTrailLength,        18.f,   0.f,    120.f)   \
  X("thrusters",   "trailFade",               thrusterTrailFade,          0.55f,  0.01f,  3.f)     \
                                                                                                   \
  X("input",       "dragThresholdPx",         inputDragThresholdPx,       6.f,    1.f,    60.f)    \
  X("input",       "tapMaxDurationMs",        inputTapMaxDurationMs,      320.f,  50.f,   1200.f)  \
  X("input",       "doubleTapWindowMs",       inputDoubleTapWindowMs,     300.f,  80.f,   900.f)   \
  X("input",       "pickPadding",             inputPickPadding,           1.15f,  1.f,    3.f)     \
                                                                                                   \
  X("scene",       "skyColourR",              skyColourR,                 0.043f, 0.f,    1.f)     \
  X("scene",       "skyColourG",              skyColourG,                 0.051f, 0.f,    1.f)     \
  X("scene",       "skyColourB",              skyColourB,                 0.063f, 0.f,    1.f)     \
  X("scene",       "groundColourR",           groundColourR,              0.075f, 0.f,    1.f)     \
  X("scene",       "groundColourG",           groundColourG,              0.082f, 0.f,    1.f)     \
  X("scene",       "groundColourB",           groundColourB,              0.094f, 0.f,    1.f)     \
  X("scene",       "gridColourR",             gridColourR,                0.28f,  0.f,    1.f)     \
  X("scene",       "gridColourG",             gridColourG,                0.36f,  0.f,    1.f)     \
  X("scene",       "gridColourB",             gridColourB,                0.44f,  0.f,    1.f)     \
  X("scene",       "gridStrength",            gridStrength,               0.55f,  0.f,    1.f)     \
  X("scene",       "gridSpacing",             gridSpacing,                20.f,   1.f,    200.f)   \
  X("scene",       "gridLineWidthPx",         gridLineWidthPx,            1.3f,   0.2f,   8.f)     \
  X("scene",       "gridFadeDistance",        gridFadeDistance,           900.f,  50.f,   6000.f)  \
  X("scene",       "groundSize",              groundSize,                 4000.f, 200.f,  20000.f) \
  X("scene",       "lightDirX",               lightDirX,                  -0.42f, -1.f,   1.f)     \
  X("scene",       "lightDirY",               lightDirY,                  0.78f,  -1.f,   1.f)     \
  X("scene",       "lightDirZ",               lightDirZ,                  -0.46f, -1.f,   1.f)     \
  X("scene",       "ambientLevel",            ambientLevel,               0.3f,   0.f,    1.f)     \
  X("scene",       "shipColourR",             shipColourR,                0.55f,  0.f,    1.f)     \
  X("scene",       "shipColourG",             shipColourG,                0.6f,   0.f,    1.f)     \
  X("scene",       "shipColourB",             shipColourB,                0.66f,  0.f,    1.f)     \
  X("scene",       "selectedColourR",         selectedColourR,            0.35f,  0.f,    1.f)     \
  X("scene",       "selectedColourG",         selectedColourG,            0.95f,  0.f,    1.f)     \
  X("scene",       "selectedColourB",         selectedColourB,            0.66f,  0.f,    1.f)     \
  X("scene",       "shipMaterialMix",         shipMaterialMix,            0.55f,  0.f,    1.f)     \
  X("scene",       "shipScale",               shipScale,                  1.f,    0.1f,   5.f)     \
  X("scene",       "startSpacing",            startSpacing,               55.f,   5.f,    400.f)

struct Tuning
{
#define TUNING_DECLARE_FIELD(section, key, field, defaultValue, minValue, maxValue) float field = defaultValue;
  TUNING_FIELDS(TUNING_DECLARE_FIELD)
#undef TUNING_DECLARE_FIELD
};

inline Tuning g_tuning;

// Loads tuning.ini (writing it from the defaults if it is not there), starts watching it, and opens
// the Tuner window. Call once, after the main window exists.
void TuningInit(HINSTANCE _instance, HWND _mainWindow);

// Call once per frame: picks up an edited tuning.ini and refreshes the Tuner sliders.
void TuningPoll();

void TuningShutdown();

// Pushes the live values back onto the sliders, after something other than the Tuner moved them.
void TuningRefreshWindow();
void TuningToggleWindow();
void TuningSave();
