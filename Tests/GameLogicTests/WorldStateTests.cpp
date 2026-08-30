#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// A scene with something in every table the codec has to carry, because a state codec tested against
// an empty world proves only that zero equals zero. Architecture in the static set and the obstacle
// set, a fleet spanning the hull table, planned routes, a station with a garrison, ships on patrol,
// a docking in flight, and an aggression that has already turned the standings table and put a
// protector duty on somebody.
//
// The same builder is used by every row here, so a failure is about the codec and never about two
// tests disagreeing on what a world looks like.
struct Scene
{
  std::vector<Game::ShipId> fleet;
  Game::ShipId raider = Game::INVALID_SHIP_ID;
  Game::World::StationId station = Game::World::INVALID_STATION_ID;
};

Scene BuildScene(Game::World& _world)
{
  Scene scene;
  _world.ConfigureShard(3);

  // Architecture: two structures far enough apart to be two islands, so the router has a partition
  // to rebuild and the routes below have a version to be current against.
  const Game::ShipId post =
    _world.SpawnShip(Game::LocalPos(-260.0f, 700.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
  (void)_world.SpawnShip(Game::LocalPos(2600.0f, -1800.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure),
                         Game::FACTION_VANGUARD);

  Game::World::StationDesc desc;
  desc.ownerFaction = Game::FACTION_VANGUARD;
  desc.protectorHullId = static_cast<std::uint32_t>(Game::HullId::Interceptor);
  desc.protectorComplement = 2;
  desc.launchEveryTicks = 30;
  scene.station = _world.MakeStation(post, desc);

  const Game::HullId HULLS[] = {Game::HullId::Interceptor, Game::HullId::Corvette, Game::HullId::Frigate, Game::HullId::Battleship,
                                Game::HullId::Carrier};
  for (int at = 0; at < 5; ++at)
  {
    scene.fleet.push_back(
      _world.SpawnShip(Game::LocalPos(static_cast<float>(at) * 240.0f - 480.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(HULLS[at])));
  }
  _world.IssueMoveOrder(scene.fleet, Game::LocalPos(200.0f, 1350.0f), true, 1.2f);

  // Two on a standing patrol around the post, so the patrol table is not empty and the ring's
  // waypoint index is somewhere in the middle by the time the save is taken.
  for (int at = 0; at < 2; ++at)
  {
    const std::uint32_t index = static_cast<std::uint32_t>(at) * Game::PATROL_RING_WAYPOINTS / 2u;
    const Game::ShipId guard =
      _world.SpawnShip(Game::PatrolRingPoint(Game::LocalPos(-260.0f, 700.0f), index, 420.0f), Game::PatrolRingHeadingRad(index),
                       static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANGUARD);
    _world.AssignPatrol(guard, post, 420.0f, 11.0f);
  }

  // A visitor with a dock order in flight, close enough that it is inside the ledger before the
  // longer runs here end -- a docking that never completes leaves station.docked empty, and a test
  // that compares two empty ledgers is a test that would pass with the ledger deleted from the
  // format. Measured: it is in by tick 180.
  const Game::ShipId visitor =
    _world.SpawnShip(Game::LocalPos(-260.0f, 1120.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER);
  const Game::ShipId dockOrder[] = {visitor};
  (void)_world.IssueDockOrder(dockOrder, post, Game::FACTION_PLAYER);

  // And an aggressor, so the standings table is not the authored one and the protector response has
  // something to hunt.
  scene.raider =
    _world.SpawnShip(Game::LocalPos(-900.0f, 900.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber), Game::FACTION_VANDAL);
  _world.RecordAggression(_world.HandleOf(scene.raider), scene.station);

  return scene;
}

// Every ship's pose, in array order. The comparison the per-tick half of the gate is made of.
void PoseOf(const Game::World& _world, std::vector<Game::WorldPos>& _outPositions, std::vector<float>& _outHeadings)
{
  _outPositions.clear();
  _outHeadings.clear();
  for (const Game::ShipState& ship : _world.Ships())
  {
    _outPositions.push_back(ship.posWorld);
    _outHeadings.push_back(ship.headingRad);
  }
}
} // namespace

TEST_CLASS(WorldStateTests)
{
public:
  TEST_METHOD(ASavedWorldReplaysToTheSameRun)
  {
    // The gate this tree has been promising itself. What it had was four same-process rerun tests --
    // TheSameOrderProducesTheSameRun and its three siblings -- which run the same input twice in one
    // process and compare. That catches a clock or an address leaking into the tick. What it cannot
    // catch is state Step depends on that nothing can reconstruct, because both runs build it the
    // same way from the same start. This one takes a world apart and puts it back together
    // (Design/MmoScalabilityReview.md U3).
    Game::World original;
    const Scene scene = BuildScene(original);
    for (int tick = 0; tick < 240; ++tick)
      original.Step();

    std::vector<std::uint8_t> saved;
    Game::WriteWorldState(original, saved);
    Assert::IsTrue(saved.size() > 1000, L"the state came out suspiciously small for a scene this size");

    Game::World reloaded;
    Assert::IsTrue(Game::ReadWorldState(saved, reloaded), L"the state did not load");
    Assert::AreEqual(original.Tick(), reloaded.Tick(), L"the tick did not survive");
    Assert::AreEqual(original.ShipCount(), reloaded.ShipCount(), L"the ship count did not survive");

    // Tick for tick, so a divergence says WHEN rather than only that one happened.
    std::vector<Game::WorldPos> originalPositions;
    std::vector<Game::WorldPos> reloadedPositions;
    std::vector<float> originalHeadings;
    std::vector<float> reloadedHeadings;
    for (int tick = 0; tick < 300; ++tick)
    {
      original.Step();
      reloaded.Step();

      PoseOf(original, originalPositions, originalHeadings);
      PoseOf(reloaded, reloadedPositions, reloadedHeadings);
      Assert::AreEqual(originalPositions.size(), reloadedPositions.size(), L"the two worlds hold different numbers of ships");
      for (std::size_t at = 0; at < originalPositions.size(); ++at)
      {
        Assert::IsTrue(IsSamePosition(originalPositions[at], reloadedPositions[at]),
                       L"a reloaded world put a ship somewhere the original did not");
        Assert::AreEqual(originalHeadings[at], reloadedHeadings[at], 0.0f, L"a reloaded world pointed a ship somewhere else");
      }
    }

    // And the total comparison: two states written by the same codec, byte for byte. This is the
    // half that fails the day somebody adds a field to World that Step reads and forgets this codec,
    // because it needs no list of fields to keep in step with (work order 2.4).
    std::vector<std::uint8_t> afterOriginal;
    std::vector<std::uint8_t> afterReloaded;
    Game::WriteWorldState(original, afterOriginal);
    Game::WriteWorldState(reloaded, afterReloaded);
    Assert::AreEqual(afterOriginal.size(), afterReloaded.size(), L"the two states are different sizes after the replay");
    Assert::IsTrue(afterOriginal == afterReloaded, L"the two states diverged over the replay");
  }

  TEST_METHOD(ASaveTakenMidRouteResumesMidRoute)
  {
    // A route carries the grid version it was planned against, and a mismatch re-plans. That number
    // is an epoch counter with no meaning outside the run that produced it, so the codec writes
    // whether the route was current and the loader fills in the number its own rebuild produced. Get
    // that wrong and every routed ship re-plans on the tick after a reload and on no other, which is
    // a divergence that only shows up in a scene with architecture in it (work order 2.1).
    Game::World original;
    const Scene scene = BuildScene(original);
    for (int tick = 0; tick < 60; ++tick)
      original.Step();

    const Game::ShipId routed = scene.fleet[2];
    const std::vector<Game::WorldPos> routeBefore(original.RouteOf(routed).begin(), original.RouteOf(routed).end());
    Assert::IsTrue(!routeBefore.empty(), L"the scene's ship is not following a route, so this proves nothing");

    std::vector<std::uint8_t> saved;
    Game::WriteWorldState(original, saved);
    Game::World reloaded;
    Assert::IsTrue(Game::ReadWorldState(saved, reloaded), L"the state did not load");

    const std::span<const Game::WorldPos> routeAfter = reloaded.RouteOf(routed);
    Assert::AreEqual(routeBefore.size(), routeAfter.size(), L"the route changed length across a reload");
    for (std::size_t at = 0; at < routeBefore.size(); ++at)
      Assert::IsTrue(IsSamePosition(routeBefore[at], routeAfter[at]), L"a waypoint did not survive the reload");

    // One step each. A re-plan would rewrite the list; nothing else on this tick can.
    original.Step();
    reloaded.Step();
    const std::span<const Game::WorldPos> steppedOriginal = original.RouteOf(routed);
    const std::span<const Game::WorldPos> steppedReloaded = reloaded.RouteOf(routed);
    Assert::AreEqual(steppedOriginal.size(), steppedReloaded.size(), L"the reloaded ship re-planned on the tick after the load");
    for (std::size_t at = 0; at < steppedOriginal.size(); ++at)
      Assert::IsTrue(IsSamePosition(steppedOriginal[at], steppedReloaded[at]), L"the reloaded ship re-planned on the tick after the load");
  }

  TEST_METHOD(IdentityAndHandlesSurviveTheRoundTrip)
  {
    Game::World original;
    const Scene scene = BuildScene(original);
    for (int tick = 0; tick < 90; ++tick)
      original.Step();

    // A death before the save, so the freed slot, the bumped generation and the despawn log are all
    // carrying something rather than being empty.
    const Game::ShipId doomed = scene.fleet[4];
    const Game::ShipHandle doomedHandle = original.HandleOf(doomed);
    const Game::EntityId doomedEntity = original.EntityIdOf(doomed);
    Assert::IsTrue(original.DespawnShip(doomedHandle), L"the despawn failed");

    std::vector<Game::ShipHandle> handles;
    std::vector<Game::EntityId> entities;
    for (Game::ShipId id = 0; id < original.ShipCount(); ++id)
    {
      handles.push_back(original.HandleOf(id));
      entities.push_back(original.EntityIdOf(id));
    }

    std::vector<std::uint8_t> saved;
    Game::WriteWorldState(original, saved);
    Game::World reloaded;
    Assert::IsTrue(Game::ReadWorldState(saved, reloaded), L"the state did not load");

    for (Game::ShipId id = 0; id < reloaded.ShipCount(); ++id)
    {
      Assert::IsTrue(reloaded.HandleOf(id) == handles[id], L"a handle changed across a reload");
      Assert::IsTrue(reloaded.EntityIdOf(id) == entities[id], L"an identity changed across a reload");
      Assert::AreEqual(id, reloaded.Resolve(handles[id]), L"a handle resolved to a different ship after a reload");
      Assert::AreEqual(id, reloaded.ResolveEntity(entities[id]), L"an identity resolved to a different ship after a reload");
    }

    // The dead one stays dead, both ways of asking.
    Assert::AreEqual(Game::INVALID_SHIP_ID, reloaded.Resolve(doomedHandle), L"a dead handle came back to life across a reload");
    Assert::AreEqual(Game::INVALID_SHIP_ID, reloaded.ResolveEntity(doomedEntity), L"a dead identity came back to life across a reload");

    // And the counter came with it, so the next spawn cannot reissue an id the file already used.
    const Game::ShipId fresh = reloaded.SpawnShip(Game::LocalPos(4000.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Miner));
    const Game::EntityId freshEntity = reloaded.EntityIdOf(fresh);
    Assert::IsTrue(freshEntity != doomedEntity, L"a reloaded world reissued a dead entity's id");
    for (const Game::EntityId held : entities)
      Assert::IsTrue(freshEntity != held, L"a reloaded world reissued a live entity's id");

    // The free list came with it too, in order. That is the assertion the list exists for: reuse is
    // last-in-first-out, so the ORDER is the reproduction, and a reloaded world that rebuilt the
    // list instead of reading it would put the next ship in a different slot. The two worlds are
    // asked for the same spawn and must answer with the same handle, generation included.
    const Game::ShipId alsoFresh = original.SpawnShip(Game::LocalPos(4000.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Miner));
    Assert::AreEqual(fresh, alsoFresh, L"the two worlds put the same spawn at a different index");
    Assert::IsTrue(reloaded.HandleOf(fresh) == original.HandleOf(alsoFresh),
                   L"a reloaded world did not reuse the freed slot the original did");
    Assert::AreEqual(doomedHandle.slot, reloaded.HandleOf(fresh).slot, L"the freed slot was not reused at all, so this proves nothing");
    Assert::IsTrue(reloaded.EntityIdOf(fresh) == original.EntityIdOf(alsoFresh), L"the two worlds minted different ids for one spawn");

    // The despawn log came with it too: a subscriber that had not read that death still owes it.
    Assert::AreEqual(original.DespawnHead(), reloaded.DespawnHead(), L"the despawn cursor head did not survive");
    Assert::AreEqual(static_cast<std::size_t>(1), reloaded.DespawnsSince(0).size(), L"the despawn log did not survive");
    Assert::IsTrue(reloaded.DespawnsSince(0)[0].entity == doomedEntity, L"the despawn log lost who died");
  }

  TEST_METHOD(TheStationSideTablesSurviveTheRoundTrip)
  {
    Game::World original;
    const Scene scene = BuildScene(original);
    for (int tick = 0; tick < 400; ++tick)
      original.Step();

    const Game::World::Station& before = original.StationOf(scene.station);
    Assert::IsTrue(!before.targets.empty(), L"the aggression never reached the station's target list");
    // Both halves of the row have to be carrying something, or comparing them proves nothing.
    Assert::IsTrue(!before.docked.empty(), L"nothing ever docked, so the ledger comparison below is two empty lists");

    std::vector<std::uint8_t> saved;
    Game::WriteWorldState(original, saved);
    Game::World reloaded;
    Assert::IsTrue(Game::ReadWorldState(saved, reloaded), L"the state did not load");

    Assert::AreEqual(original.StationCount(), reloaded.StationCount(), L"the station table did not survive");
    const Game::World::Station& after = reloaded.StationOf(scene.station);
    Assert::IsTrue(after.structure == before.structure, L"the station's structure handle did not survive");
    Assert::AreEqual(static_cast<std::uint32_t>(before.ownerFaction), static_cast<std::uint32_t>(after.ownerFaction),
                     L"the station's owner did not survive");
    Assert::AreEqual(before.protectorComplement, after.protectorComplement, L"the garrison size did not survive");
    Assert::AreEqual(before.launchCooldownTicks, after.launchCooldownTicks, L"the launch metronome did not survive");
    Assert::AreEqual(before.targets.size(), after.targets.size(), L"the target list did not survive");
    for (std::size_t at = 0; at < before.targets.size(); ++at)
      Assert::IsTrue(before.targets[at] == after.targets[at], L"a target handle did not survive");
    Assert::AreEqual(before.docked.size(), after.docked.size(), L"the ledger did not survive");
    for (std::size_t at = 0; at < before.docked.size(); ++at)
    {
      Assert::AreEqual(before.docked[at].hullId, after.docked[at].hullId, L"a ledger row's hull did not survive");
      Assert::AreEqual(static_cast<std::uint32_t>(before.docked[at].factionId), static_cast<std::uint32_t>(after.docked[at].factionId),
                       L"a ledger row's faction did not survive");
    }

    // The standings the aggression turned, not the authored table. A reload that reset them would
    // re-admit an attacker the station had already refused.
    Assert::IsTrue(reloaded.StandingOf(Game::FACTION_VANGUARD, Game::FACTION_VANDAL) == Game::Standing::Hostile,
                   L"the recorded aggression did not survive the reload");
    Assert::AreEqual(static_cast<std::uint32_t>(original.HostileMaskFor(Game::FACTION_PLAYER)),
                     static_cast<std::uint32_t>(reloaded.HostileMaskFor(Game::FACTION_PLAYER)), L"the standings table did not survive");
  }

  TEST_METHOD(AMalformedStateIsRefusedAndChangesNothing)
  {
    // The same discipline SnapshotReceiver keeps, and AGENTS.md 5's rule for anything parsing
    // content: report or refuse, never throw and never assert -- and leave the caller's world alone,
    // because a half-replaced world with nothing saying which half is the failure a state codec is
    // most able to cause.
    Game::World source;
    (void)BuildScene(source);
    for (int tick = 0; tick < 30; ++tick)
      source.Step();

    std::vector<std::uint8_t> saved;
    Game::WriteWorldState(source, saved);

    // A world with something in it, so a partial apply would be visible.
    Game::World target;
    const Scene targetScene = BuildScene(target);
    for (int tick = 0; tick < 15; ++tick)
      target.Step();
    std::vector<std::uint8_t> before;
    Game::WriteWorldState(target, before);

    const auto refuses = [&](std::span<const std::uint8_t> _bytes, const wchar_t* _what)
    {
      Assert::IsFalse(Game::ReadWorldState(_bytes, target), _what);
      std::vector<std::uint8_t> after;
      Game::WriteWorldState(target, after);
      Assert::IsTrue(before == after, L"a refused state changed the world it was read into");
    };

    refuses({}, L"an empty buffer was accepted");
    refuses(std::span<const std::uint8_t>(saved).first(3), L"a buffer too short for the magic was accepted");
    refuses(std::span<const std::uint8_t>(saved).first(saved.size() / 2), L"a truncated state was accepted");
    refuses(std::span<const std::uint8_t>(saved).first(saved.size() - 1), L"a state one byte short was accepted");

    std::vector<std::uint8_t> wrongMagic = saved;
    wrongMagic[0] = static_cast<std::uint8_t>(wrongMagic[0] + 1u);
    refuses(wrongMagic, L"a buffer with the wrong magic was accepted");

    std::vector<std::uint8_t> wrongFormat = saved;
    wrongFormat[4] = static_cast<std::uint8_t>(wrongFormat[4] + 1u);
    refuses(wrongFormat, L"a state in a format this build does not know was accepted");

    // Every prefix of a valid state, which is every truncation there is. Nothing may throw, nothing
    // may be accepted, and the target world must be untouched by all of them.
    for (std::size_t length = 0; length < saved.size(); ++length)
      Assert::IsFalse(Game::ReadWorldState(std::span<const std::uint8_t>(saved).first(length), target), L"a truncated state was accepted");
    std::vector<std::uint8_t> afterEverything;
    Game::WriteWorldState(target, afterEverything);
    Assert::IsTrue(before == afterEverything, L"a run of refusals changed the world they were read into");

    // The whole thing still loads, so the sweep above was refusing truncations and not the format.
    Assert::IsTrue(Game::ReadWorldState(saved, target), L"the intact state was refused");
    Assert::AreEqual(source.Tick(), target.Tick(), L"the intact state did not take");
    Assert::AreNotEqual(static_cast<std::size_t>(0), static_cast<std::size_t>(targetScene.fleet.size()), L"the scene built nothing");
  }

  TEST_METHOD(AnEmptyWorldRoundTrips)
  {
    // The degenerate case, which a codec built around counts is exactly where an off-by-one lives.
    Game::World empty;
    std::vector<std::uint8_t> saved;
    Game::WriteWorldState(empty, saved);

    Game::World reloaded;
    (void)BuildScene(reloaded); // something to overwrite, so "loaded nothing" cannot pass by accident
    Assert::IsTrue(Game::ReadWorldState(saved, reloaded), L"an empty world did not load");
    Assert::AreEqual(0u, reloaded.ShipCount(), L"an empty state left ships behind");

    std::vector<std::uint8_t> again;
    Game::WriteWorldState(reloaded, again);
    Assert::IsTrue(saved == again, L"an empty world did not round-trip byte for byte");

    // And it still runs.
    reloaded.Step();
    Assert::AreEqual(static_cast<std::uint64_t>(1), reloaded.Tick(), L"a reloaded empty world would not step");
  }
};
} // namespace GameLogicTests
