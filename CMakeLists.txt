# Ship feel-tuning toy. Throwaway: one executable, flat source list, no install rules.
cmake_minimum_required(VERSION 3.25)
project(ShipFeel LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "ShipFeel is MSVC/x64/Windows only.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ShipFeel WIN32
  src/Main.cpp
  src/Gfx.cpp
  src/Gfx.h
  src/Scene.cpp
  src/Scene.h
  src/Tuning.cpp
  src/Tuning.h
)

target_compile_definitions(ShipFeel PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# /utf-8 so the source literals are read as UTF-8; /permissive- to match the rest of the tree.
target_compile_options(ShipFeel PRIVATE /W4 /permissive- /utf-8 /MP /Zc:preprocessor)

# Windows SDK only: comctl32 for the Tuner window, windowscodecs and ole32 for PNG capture.
target_link_libraries(ShipFeel PRIVATE d3d12 dxgi d3dcompiler dxguid gdi32 user32 comctl32 windowscodecs ole32)

# tuning.ini lives at the repository root and the exe walks up to find it, but debugging from the
# root keeps captures/ and replay files there too.
set_target_properties(ShipFeel PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
