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
#include "GpuHelpers.h"
#include "GpuDevice.h"
#include "SceneRenderer.h"
#include "TextRenderer.h"
#include "MeshLibrary.h"
#include "Camera.h"
#include "PointerEvent.h"
#include "PointerTracker.h"
#include "AppWindow.h"

#pragma comment(lib, "Gdi32.lib")       // the font atlas is baked with GDI
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib") // shaders are compiled from source at startup
