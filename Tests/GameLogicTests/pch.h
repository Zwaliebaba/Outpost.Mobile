#pragma once

// Test projects link the library under test and see its umbrella header, nothing more. A test that
// needs another layer to compile is telling you the layers are not separate.
#include "GameLogic.h"

#include "CppUnitTest.h"

// Assert::AreEqual on an enum is a static assertion failure inside the framework without this:
// it needs a ToString for anything it might have to print. Specialising it here rather than
// avoiding the comparison keeps the assertions reading as what they mean, and a failure names the
// state the ship was actually in instead of an integer.
namespace Microsoft::VisualStudio::CppUnitTestFramework
{
template <> inline std::wstring ToString<Game::OrderState>(const Game::OrderState& _order)
{
  switch (_order)
  {
  case Game::OrderState::Idle:
    return L"Idle";
  case Game::OrderState::Moving:
    return L"Moving";
  case Game::OrderState::Aligning:
    return L"Aligning";
  }
  return L"OrderState(unknown)";
}
} // namespace Microsoft::VisualStudio::CppUnitTestFramework

// The universe coordinate of a position, for tests that want to say "the ship ended up 200 m east".
//
// Reading posUniverse.localX directly is what these replaced, and it stopped working the moment the
// sector pair landed: localX is an offset inside a sector, held in [0, SECTOR_SIZE_METRES), so a
// ship at x = -3 reads localX = 8189 and every "did it go negative" assertion silently inverts.
// Going through the offset is what production code does, and a test that measures the universe the
// same way the simulation does cannot drift from it (Design/Archive/Collision-slice-8.md 5.3).
[[nodiscard]] inline float UniverseX(const Game::UniversePos& _pos) noexcept
{
  return Game::OffsetX(Game::UniversePos{}, _pos);
}

[[nodiscard]] inline float UniverseZ(const Game::UniversePos& _pos) noexcept
{
  return Game::OffsetZ(Game::UniversePos{}, _pos);
}

// Bit-exact position equality, for the determinism tests. All four fields, because comparing the
// local offsets alone would pass two positions a whole sector apart.
[[nodiscard]] inline bool IsSamePosition(const Game::UniversePos& _a, const Game::UniversePos& _b) noexcept
{
  return _a.sectorX == _b.sectorX && _a.sectorZ == _b.sectorZ && _a.localX == _b.localX && _a.localZ == _b.localZ;
}
