#pragma once

// Every value that affects what the toy looks like or how it feels. Stage 1 fills these from the
// defaults below; stage 2 replaces the defaults with tuning.ini and drives them from the Tuner
// window. Nothing here is const on purpose -- the sliders write straight into g_tuning.
struct Tuning
{
  // [camera]
  float cameraPitchDeg = 52.0f;      // orbit pitch above the horizon
  float cameraYawDeg = 0.0f;         // orbit yaw; 0 looks north (+Z)
  float cameraDistance = 190.0f;     // orbit radius, i.e. the zoom level
  float cameraMinZoom = 40.0f;
  float cameraMaxZoom = 900.0f;
  float cameraTargetHeight = 3.0f;   // how far above the ground the look-at point sits
  float cameraFovDeg = 45.0f;
  float cameraNearPlane = 0.5f;
  float cameraFarPlane = 8000.0f;

  // [scene]
  float skyColourR = 0.043f;
  float skyColourG = 0.051f;
  float skyColourB = 0.063f;
  float groundColourR = 0.075f;
  float groundColourG = 0.082f;
  float groundColourB = 0.094f;
  float gridColourR = 0.28f;
  float gridColourG = 0.36f;
  float gridColourB = 0.44f;
  float gridStrength = 0.55f;        // how far the grid line pulls away from the ground colour
  float gridSpacing = 20.0f;
  float gridLineWidthPx = 1.3f;
  float gridFadeDistance = 900.0f;   // grid fades out this far from the camera
  float groundSize = 4000.0f;        // the ground quad follows the camera, so this only has to outrun the fade
  float lightDirX = -0.42f;          // direction *towards* the light; normalised in the shader
  float lightDirY = 0.78f;
  float lightDirZ = -0.46f;
  float ambientLevel = 0.30f;
  float shipColourR = 0.55f;
  float shipColourG = 0.60f;
  float shipColourB = 0.66f;
  float selectedColourR = 0.35f;
  float selectedColourG = 0.95f;
  float selectedColourB = 0.55f;
  float shipMaterialMix = 0.55f;     // 0 = flat ship colour, 1 = the colours authored in the .mtl
  float shipScale = 1.0f;
  float startSpacing = 55.0f;        // gap between the three ships at startup
};

inline Tuning g_tuning;
