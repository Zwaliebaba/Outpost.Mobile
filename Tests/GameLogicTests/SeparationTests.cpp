#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// The overlap between two ships as the simulation itself would compute it, so a test cannot pass
// by measuring something the narrow phase does not.
[[nodiscard]] float OverlapBetween(const Game::World& _world, Game::ShipId _a, Game::ShipId _b)
{
  const Game::ShipState& first = _world.Ship(_a);
  const Game::ShipState& second = _world.Ship(_b);
  const Game::HullSpec& firstHull = Game::HullSpecOf(first.hullId);
  const Game::HullSpec& secondHull = Game::HullSpecOf(second.hullId);

  const Game::Capsule self{
    0.0f, 0.0f, std::sin(first.headingRad), std::cos(first.headingRad), firstHull.capsuleHalfLengthMetres, firstHull.capsuleRadiusMetres};
  const Game::Capsule against{Game::OffsetX(first.posWorld, second.posWorld),
                              Game::OffsetZ(first.posWorld, second.posWorld),
                              std::sin(second.headingRad),
                              std::cos(second.headingRad),
                              secondHull.capsuleHalfLengthMetres,
                              secondHull.capsuleRadiusMetres};
  const Game::Contact contact = Game::CapsuleContact(self, against, _a, _b);
  return contact.touching ? contact.overlapMetres : 0.0f;
}

[[nodiscard]] float MovedThisTick(const Game::ShipState& _ship)
{
  return Game::Distance(_ship.prevPos, _ship.posWorld);
}
} // namespace

TEST_CLASS(SeparationTests)
{
public:
  TEST_METHOD(ShipsSpawnedOnOneSpotUnpackThemselves)
  {
    Game::World world;
    const Game::ShipId first = world.SpawnShip(Game::WorldPos{0.0f, 0.0f}, 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));
    const Game::ShipId second = world.SpawnShip(Game::WorldPos{0.0f, 0.0f}, 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));

    Assert::IsTrue(OverlapBetween(world, first, second) > 0.0f, L"two hulls on the same point do not start overlapping");
    for (int tick = 0; tick < 240; ++tick)
      world.Step();
    Assert::AreEqual(0.0f, OverlapBetween(world, first, second), 1e-3f, L"two hulls on the same point never separated");
  }

  TEST_METHOD(ADenseSpawnSeparatesWithBoundedEnergy)
  {
    // A hundred ships on one point is the case that makes the per-tick clamp load-bearing.
    // Unclamped, the correction is as large as the overlap, a Jacobi solve applies all of them at
    // once, and the fleet explodes rather than unpacking (Design/Collision.md 9, 16).
    Game::World world;
    std::vector<Game::ShipId> ships;
    std::uint32_t noise = 1u;
    const auto jitter = [&noise]
    {
      noise ^= noise << 13;
      noise ^= noise >> 17;
      noise ^= noise << 5;
      return static_cast<float>(noise >> 8) / static_cast<float>(1u << 24);
    };
    for (int i = 0; i < 100; ++i)
    {
      ships.push_back(world.SpawnShip(Game::WorldPos{(jitter() - 0.5f) * 6.0f, (jitter() - 0.5f) * 6.0f}, jitter() * 6.2831853f,
                                      static_cast<std::uint32_t>(Game::HullId::Interceptor)));
    }

    const float clamp = Game::SEPARATION_CLAMP_FRACTION * Game::HullSpecOf(Game::HullId::Interceptor).capsuleRadiusMetres;
    float worstStep = 0.0f;
    for (int tick = 0; tick < 1800; ++tick)
    {
      world.Step();
      for (const Game::ShipId id : ships)
      {
        const Game::ShipState& ship = world.Ship(id);
        Assert::IsTrue(std::isfinite(ship.posWorld.localX) && std::isfinite(ship.posWorld.localZ), L"a ship's position became a NaN");
        worstStep = std::max(worstStep, MovedThisTick(ship));
      }
    }

    // Nothing is under orders, so the only thing moving a ship is the correction, and the clamp is
    // exactly what bounds it. A small tolerance for the float arithmetic in the scale-down itself.
    Assert::IsTrue(worstStep <= clamp * 1.01f,
                   std::format(L"a ship moved {:.4f} m in one tick against a clamp of {:.4f} m", worstStep, clamp).c_str());
    Logger::WriteMessage(
      std::format(L"dense spawn: worst single-tick displacement {:.4f} m against a clamp of {:.4f} m\n", worstStep, clamp).c_str());

    for (size_t a = 0; a < ships.size(); ++a)
    {
      for (size_t b = a + 1; b < ships.size(); ++b)
        Assert::AreEqual(0.0f, OverlapBetween(world, ships[a], ships[b]), 1e-2f, L"a dense spawn left ships inside each other");
    }
  }

  TEST_METHOD(ACapitalIsNotShovedAsideByAFighter)
  {
    // The asymmetry has to be visible, not merely present: a Carrier that yields to an Interceptor
    // reads as a bug to anyone watching.
    Game::World world;
    const Game::ShipId carrier = world.SpawnShip(Game::WorldPos{0.0f, 0.0f}, 0.0f, static_cast<std::uint32_t>(Game::HullId::Carrier));
    const Game::ShipId fighter = world.SpawnShip(Game::WorldPos{30.0f, 0.0f}, 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor));

    const Game::WorldPos carrierStart = world.Ship(carrier).posWorld;
    const Game::WorldPos fighterStart = world.Ship(fighter).posWorld;
    for (int tick = 0; tick < 120; ++tick)
      world.Step();

    const float carrierMoved = Game::Distance(carrierStart, world.Ship(carrier).posWorld);
    const float fighterMoved = Game::Distance(fighterStart, world.Ship(fighter).posWorld);
    Assert::IsTrue(fighterMoved > carrierMoved * 4.0f,
                   std::format(L"the Carrier moved {:.3f} m and the Interceptor {:.3f} m", carrierMoved, fighterMoved).c_str());
    Assert::AreEqual(0.0f, OverlapBetween(world, carrier, fighter), 1e-2f, L"the pair never separated");
  }

  TEST_METHOD(AShipNeverEndsInsideAStructure)
  {
    // Asserted on the final position every tick, not on the correction: the guarantee is about
    // where a ship is, and a correct correction applied at the wrong point in the pass order is
    // still a ship inside a wall.
    Game::World world;
    const Game::ShipId structure = world.SpawnShip(Game::WorldPos{0.0f, 0.0f}, 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure));
    const Game::ShipId ship = world.SpawnShip(Game::WorldPos{-600.0f, 0.0f}, 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));

    const Game::WorldPos structureStart = world.Ship(structure).posWorld;
    const Game::ShipId order[] = {ship};
    world.IssueMoveOrder(order, Game::WorldPos{600.0f, 0.0f}, false, 0.0f);

    for (int tick = 0; tick < 3000; ++tick)
    {
      world.Step();
      Assert::AreEqual(0.0f, OverlapBetween(world, ship, structure), 1e-2f, L"a ship ended a tick inside a Structure");
    }
    Assert::AreEqual(0.0f, Game::Distance(structureStart, world.Ship(structure).posWorld), 0.0f, L"the Structure moved");
  }

  TEST_METHOD(TrafficCannotPushAShipThroughAStructure)
  {
    // The one case where the clamp must not apply. A ship pinned against architecture by a column
    // behind it has a soft correction pushing it in and a hard one pushing it out; the hard one
    // runs second and is unbounded, which is what makes blocking hard rather than decorative.
    Game::World world;
    const Game::ShipId structure = world.SpawnShip(Game::WorldPos{0.0f, 0.0f}, 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure));

    std::vector<Game::ShipId> column;
    for (int i = 0; i < 12; ++i)
    {
      column.push_back(world.SpawnShip(Game::WorldPos{-400.0f - static_cast<float>(i) * 26.0f, 0.0f}, 0.0f,
                                       static_cast<std::uint32_t>(Game::HullId::Corvette)));
    }
    world.IssueMoveOrder(column, Game::WorldPos{400.0f, 0.0f}, false, 0.0f);

    for (int tick = 0; tick < 2400; ++tick)
    {
      world.Step();
      for (const Game::ShipId id : column)
        Assert::AreEqual(0.0f, OverlapBetween(world, id, structure), 1e-2f, L"traffic squeezed a ship into a Structure");
    }
  }

  TEST_METHOD(AStargateIsFlownThrough)
  {
    // Decision 2 of the design review: no hull on a Stargate. Four capsules approximating a ring
    // would be real narrow-phase cost on every pair every tick, for an object players fly through
    // on purpose (Design/Collision.md 18).
    Game::World world;
    const Game::ShipId gate = world.SpawnShip(Game::WorldPos{0.0f, 0.0f}, 0.0f, static_cast<std::uint32_t>(Game::HullId::Stargate));
    const Game::ShipId ship = world.SpawnShip(Game::WorldPos{0.0f, -400.0f}, 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor));

    const Game::ShipId order[] = {ship};
    world.IssueMoveOrder(order, Game::WorldPos{0.0f, 400.0f}, false, 0.0f);
    for (int tick = 0; tick < 3000 && world.Ship(ship).order != Game::OrderState::Idle; ++tick)
      world.Step();

    Assert::AreEqual(Game::OrderState::Idle, world.Ship(ship).order, L"a ship could not fly through a Stargate");
    Assert::IsTrue(world.Ship(ship).posWorld.localZ > 380.0f, L"a ship was blocked by a Stargate on its way through");
    Assert::AreEqual(0.0f, world.Ship(gate).posWorld.localZ, 0.0f, L"a Stargate moved");
  }
};
} // namespace GameLogicTests
