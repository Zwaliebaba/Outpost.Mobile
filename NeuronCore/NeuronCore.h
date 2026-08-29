#pragma once

// The NeuronCore umbrella. Every project's pch.h reaches the engine through one of these three
// umbrella headers -- NeuronCore.h, NeuronClient.h, NeuronServer.h -- so the Windows headers are
// configured in exactly one place and in the one order that works.
//
// NeuronCore holds engine primitives with no game semantics in them: diagnostics, file IO, easing,
// timing, and the declared client/server transport seam. Nothing here knows what a ship is, and
// nothing here touches a graphics API or names a graphics type: the server is meant to run
// headless in a container, and this library goes with it (AGENTS.md 2). Content readers live with
// whatever consumes what they read (Design/Decisions/0002), which today is always the client.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Use the C++ standard templated min/max
#define NOMINMAX

// DirectX apps don't need GDI
//#define NODRAWTEXT
//#define NOGDI
//#define NOBITMAP

// Include <mcx.h> if you need this
#define NOMCX

// Include <winsvc.h> if you need this
#define NOSERVICE

// WinHelp is deprecated
#define NOHELP

#if !defined WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#endif

#include <WinSock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <unknwn.h>
#include <restrictederrorinfo.h>
#include <hstring.h>

// Undefine GetCurrentTime macro to prevent
// conflict with Storyboard::GetCurrentTime
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

using namespace winrt;

#include "Debug.h"
#include "NeuronHelper.h"
#include "Ease.h"
#include "Pcg32.h"
#include "FileSys.h"
#include "FrameClock.h"
#include "Transport.h"
#include "LoopbackTransport.h"

using namespace Neuron;

#pragma comment(lib, "ws2_32.lib")
