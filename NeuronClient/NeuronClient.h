#pragma once

// The NeuronClient umbrella. The presenting half of the engine: window, input, camera, D3D12
// renderer, and the mesh library that feeds it. It knows nothing about the game -- no ship, no
// order, no world -- and nothing about the server half.
//
// Everything game-shaped that has to reach in comes in through a callback or a small interface the
// executable implements: AppWindow's handlers, PointerListener, Camera::Desc.

#include "NeuronCore.h"

#include <winrt/Windows.ApplicationModel.Activation.h>

#include "RenderTypes.h"
#include "MeshData.h"
#include "Noise3.h"
#include "CubeSphere.h"
#include "BodyDesc.h"
#include "BodyParams.h"
#include "BodyField.h"
#include "ColourRamp.h"
#include "BodyMeshBuilder.h"
#include "FxVertex.h"
#include "SkyVertex.h"
#include "SkyField.h"
#include "MeshShatter.h"
#include "SpriteParticles.h"
#include "ObjParser.h"
#include "DdsImage.h"
#include "GpuHelpers.h"
#include "GpuDevice.h"
#include "SceneRenderer.h"
#include "BodyRenderer.h"
#include "FxRenderer.h"
#include "SkyRenderer.h"
#include "BitmapFont.h"
#include "ScreenImage.h"
#include "TextRenderer.h"
#include "MeshLibrary.h"
#include "Camera.h"
#include "PointerEvent.h"
#include "PointerTracker.h"
#include "AppWindow.h"

// Shaders are compiled at build time (Shaders/, AGENTS.md 3), so d3dcompiler is not linked and
// d3dcompiler_47.dll is not a runtime dependency.
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
