#pragma once

#include "Gfx.h"

#include <DirectXMath.h>

#include <string>
#include <vector>

// Walks up from the executable looking for the folder that holds GameData and tuning.ini, so the
// toy runs from a build tree or from the repository root without a configured path.
std::wstring FindDataRoot();

struct Ship
{
  DirectX::XMFLOAT3 posWorld{0.0f, 0.0f, 0.0f};
  float headingRad = 0.0f; // 0 points north (+Z), the bow direction after the OBJ import flip
  UINT mesh = 0;
  float restY = 0.0f;      // lifts the hull so its lowest vertex rests on the ground plane
};

struct Scene
{
  void Init(Gfx& _gfx);
  void Render(Gfx& _gfx);

  std::vector<Ship> m_ships;
  UINT m_groundMesh = 0;

  DirectX::XMFLOAT3 m_cameraTarget{0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 m_cameraEye{0.0f, 0.0f, 0.0f};
  float m_yawRad = 0.0f;
  float m_pitchRad = 0.0f;
  float m_distance = 0.0f;
};
