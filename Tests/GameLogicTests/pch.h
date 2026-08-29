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
