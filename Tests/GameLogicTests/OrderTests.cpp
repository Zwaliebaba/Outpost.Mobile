#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
TEST_CLASS(OrderTests)
{
public:
  TEST_METHOD(AnArrivalRadiusFitsInsideItsSlot)
  {
    // The trap in scaling both with the hull. Arrival radius and slot spacing grow together, so if
    // one outruns the other a Carrier's arrival radius reaches past its own slot and into the next
    // one -- and ships "arrive" in each other's positions. The formation assembles into the wrong
    // shape and never corrects, because every ship believes it is done (Design/Collision.md 13).
    for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
    {
      const Game::HullSpec& spec = Game::HULL_SPECS[hull];
      const float arrival = Game::ArrivalRadiusMetres(spec);
      const float halfSlot = 0.5f * Game::SlotSpacingMetres(spec.BoundingRadiusMetres());
      Assert::IsTrue(
        arrival < halfSlot,
        std::format(L"hull {} arrives within {:.2f} m of a slot only {:.2f} m from its neighbour's", hull, arrival, halfSlot).c_str());
    }
  }

  TEST_METHOD(AFormationIsNotBornInCollision)
  {
    // FORMATION_SPACING was 34 m against a Carrier's 107 m bounding radius, so a formation of
    // capitals was born with every hull deeply inside its neighbours -- and separation would have
    // spent the rest of the match pushing them apart while the order pushed them back together.
    for (const Game::HullId hull :
         {Game::HullId::Interceptor, Game::HullId::Corvette, Game::HullId::Frigate, Game::HullId::Battleship, Game::HullId::Carrier})
    {
      const float radius = Game::HullSpecOf(hull).BoundingRadiusMetres();
      for (int count = 2; count <= 8; ++count)
      {
        Game::World world;
        std::vector<Game::ShipId> ships;
        for (int i = 0; i < count; ++i)
        {
          ships.push_back(
            world.SpawnShip(Game::WorldPos{static_cast<float>(i) * radius * 4.0f, 0.0f}, 0.0f, static_cast<std::uint32_t>(hull)));
        }
        world.IssueMoveOrder(ships, Game::WorldPos{0.0f, 4000.0f}, false, 0.0f);

        // Slot separation against the group's actual hull radii, not against a bare metre.
        for (size_t a = 0; a < ships.size(); ++a)
        {
          for (size_t b = a + 1; b < ships.size(); ++b)
          {
            const float apart = Game::Distance(world.Ship(ships[a]).steerTargetPos, world.Ship(ships[b]).steerTargetPos);
            Assert::IsTrue(
              apart >= radius * 2.0f,
              std::format(L"two slots in a {}-ship formation are {:.1f} m apart for a hull {:.1f} m across", count, apart, radius * 2.0f)
                .c_str());
          }
        }
      }
    }
  }

  TEST_METHOD(AMixedFormationSpacesForItsLargestHull)
  {
    // A group plans once, with the largest hull's needs, so it takes one shape and stays together
    // rather than the Carrier arriving into three fighters that were spaced for themselves.
    Game::World world;
    std::vector<Game::ShipId> group;
    group.push_back(world.SpawnShip(Game::WorldPos{-300.0f, 0.0f}, 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor)));
    group.push_back(world.SpawnShip(Game::WorldPos{0.0f, 0.0f}, 0.0f, static_cast<std::uint32_t>(Game::HullId::Carrier)));
    group.push_back(world.SpawnShip(Game::WorldPos{300.0f, 0.0f}, 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor)));
    world.IssueMoveOrder(group, Game::WorldPos{0.0f, 3000.0f}, false, 0.0f);

    const float carrierRadius = Game::HullSpecOf(Game::HullId::Carrier).BoundingRadiusMetres();
    for (size_t a = 0; a < group.size(); ++a)
    {
      for (size_t b = a + 1; b < group.size(); ++b)
      {
        const float apart = Game::Distance(world.Ship(group[a]).steerTargetPos, world.Ship(group[b]).steerTargetPos);
        Assert::IsTrue(apart >= carrierRadius * 2.0f,
                       std::format(L"a mixed formation spaced its slots {:.1f} m apart, inside the Carrier's own {:.1f} m width", apart,
                                   carrierRadius * 2.0f)
                         .c_str());
      }
    }
  }

  TEST_METHOD(ACapitalFormationAssemblesWithoutFighting)
  {
    // The end to end version: order capitals somewhere and let them get there. Every ship arrives,
    // and none of them arrives inside another -- which is the thing the old fixed spacing made
    // impossible whatever the separation pass did.
    Game::World world;
    std::vector<Game::ShipId> wing;
    for (int i = 0; i < 4; ++i)
    {
      wing.push_back(world.SpawnShip(Game::WorldPos{static_cast<float>(i) * 400.0f - 600.0f, 0.0f}, 0.0f,
                                     static_cast<std::uint32_t>(Game::HullId::Battleship)));
    }
    world.IssueMoveOrder(wing, Game::WorldPos{0.0f, 2500.0f}, false, 0.0f);

    int settled = 0;
    for (int tick = 0; tick < 12000 && settled < static_cast<int>(wing.size()); ++tick)
    {
      world.Step();
      settled = 0;
      for (const Game::ShipId id : wing)
      {
        if (world.Ship(id).order == Game::OrderState::Idle)
          ++settled;
      }
    }
    Assert::AreEqual(static_cast<int>(wing.size()), settled, L"a capital formation never finished assembling");

    const Game::HullSpec& spec = Game::HullSpecOf(Game::HullId::Battleship);
    for (size_t a = 0; a < wing.size(); ++a)
    {
      for (size_t b = a + 1; b < wing.size(); ++b)
      {
        const float apart = Game::Distance(world.Ship(wing[a]).posWorld, world.Ship(wing[b]).posWorld);
        Assert::IsTrue(
          apart > spec.capsuleRadiusMetres * 2.0f,
          std::format(L"two capitals settled {:.1f} m apart, inside their combined {:.1f} m beam", apart, spec.capsuleRadiusMetres * 2.0f)
            .c_str());
      }
    }
  }
};
} // namespace GameLogicTests
