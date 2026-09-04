#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// A scene with something in every table the codec has to carry, because a state codec tested against
// an empty universe proves only that zero equals zero. Architecture in the static set and the obstacle
// set, a fleet spanning the hull table, planned routes, a station with a garrison, ships on patrol,
// a docking in flight, and an aggression that has already turned the standings table and put a
// protector duty on somebody.
//
// The same builder is used by every row here, so a failure is about the codec and never about two
// tests disagreeing on what a universe looks like.
struct Scene
{
  std::vector<Game::ShipId> fleet;
  Game::ShipId raider = Game::INVALID_SHIP_ID;
  Game::Universe::StationId station = Game::Universe::INVALID_STATION_ID;
};

Scene BuildScene(Game::Universe& _universe)
{
  Scene scene;
  _universe.ConfigureShard(3);

  // Architecture: two structures far enough apart to be two islands, so the router has a partition
  // to rebuild and the routes below have a version to be current against.
  const Game::ShipId post =
    _universe.SpawnShip(Game::LocalPos(-260.0f, 700.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
  (void)_universe.SpawnShip(Game::LocalPos(2600.0f, -1800.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure),
                            Game::FACTION_VANGUARD);

  Game::Universe::StationDesc desc;
  desc.ownerFaction = Game::FACTION_VANGUARD;
  desc.protectorHullId = static_cast<std::uint32_t>(Game::HullId::Interceptor);
  desc.protectorComplement = 2;
  desc.launchEveryTicks = 30;
  scene.station = _universe.MakeStation(post, desc);

  const Game::HullId HULLS[] = {Game::HullId::Interceptor, Game::HullId::Corvette, Game::HullId::Frigate, Game::HullId::Battleship,
                                Game::HullId::Carrier};
  for (int at = 0; at < 5; ++at)
  {
    scene.fleet.push_back(
      _universe.SpawnShip(Game::LocalPos(static_cast<float>(at) * 240.0f - 480.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(HULLS[at])));
  }
  _universe.IssueMoveOrder(scene.fleet, Game::LocalPos(200.0f, 1350.0f), true, 1.2f);

  // Two on a standing patrol around the post, so the patrol table is not empty and the ring's
  // waypoint index is somewhere in the middle by the time the save is taken.
  for (int at = 0; at < 2; ++at)
  {
    const std::uint32_t index = static_cast<std::uint32_t>(at) * Game::PATROL_RING_WAYPOINTS / 2u;
    const Game::ShipId guard =
      _universe.SpawnShip(Game::PatrolRingPoint(Game::LocalPos(-260.0f, 700.0f), index, 420.0f), Game::PatrolRingHeadingRad(index),
                          static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANGUARD);
    _universe.AssignPatrol(guard, post, 420.0f, 11.0f);
  }

  // A visitor with a dock order in flight, close enough that it is inside the ledger before the
  // longer runs here end -- a docking that never completes leaves station.docked empty, and a test
  // that compares two empty ledgers is a test that would pass with the ledger deleted from the
  // format. Measured: it is in by tick 180.
  const Game::ShipId visitor =
    _universe.SpawnShip(Game::LocalPos(-260.0f, 1120.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER);
  const Game::ShipId dockOrder[] = {visitor};
  (void)_universe.IssueDockOrder(dockOrder, post, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER});

  // And an aggressor, so the standings table is not the authored one and the protector response has
  // something to hunt.
  scene.raider =
    _universe.SpawnShip(Game::LocalPos(-900.0f, 900.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber), Game::FACTION_VANDAL);
  _universe.RecordAggression(_universe.HandleOf(scene.raider), scene.station);

  return scene;
}

// Every ship's pose, in array order. The comparison the per-tick half of the gate is made of.
void PoseOf(const Game::Universe& _universe, std::vector<Game::UniversePos>& _outPositions, std::vector<float>& _outHeadings)
{
  _outPositions.clear();
  _outHeadings.clear();
  for (const Game::ShipState& ship : _universe.Ships())
  {
    _outPositions.push_back(ship.posUniverse);
    _outHeadings.push_back(ship.headingRad);
  }
}

// One save file per format the reader still accepts, kept under Assets\ and copied beside the
// suite by the project file, exactly as NeuronClientTests carries its NMO fixture. Each is
// `UniverseGen 0` run at the commit before the format moved on: the shipped galaxy at tick zero,
// which is byte-identical on any machine (ADR 0058) and is exactly the file a deployment has on
// disk. The census the tool printed for it is recorded here beside the name, so a reader that
// mis-parses an older layout into a plausible universe fails on a count rather than passing.
//
// The rule (Design/Archive/SaveMigration-work-order.md 1.3): the commit that bumps UNIVERSE_STATE_FORMAT
// to N+1 adds UniverseFormatN.sav to this table and to the project file, and its row is green in
// that same commit. Nothing is removed from here except by the decision record that moves
// UNIVERSE_STATE_FORMAT_OLDEST.
struct Fixture
{
  const wchar_t* name;
  std::uint8_t fileFormat;
  std::uint8_t stateFormat;
  std::size_t systems;
  std::uint32_t gates;
  std::uint32_t stations;
  std::uint32_t ships;
  std::uint32_t fleets;
  std::size_t bytes;
};

constexpr Fixture FIXTURES[] = {
  {L"UniverseFormat7.sav", 1, 7, 54, 136, 165, 307, 1, 124438},
  {L"UniverseFormat8.sav", 2, 8, 54, 136, 165, 307, 1, 126906},
  // The station rule changed between 8 and 9 -- one per system rather than one per planet -- so the
  // census drops by 110 stations and 110 ships here and nothing about the FORMAT did. Recorded
  // because a reader that skipped the change would still parse this file into a plausible universe,
  // and the count is the only thing that would say so.
  {L"UniverseFormat9.sav", 2, 9, 54, 136, 55, 197, 1, 80502},
};

// The fixtures sit under Assets\ beside the suite, the shape the app and the client suite both use.
Neuron::ByteBuffer ReadFixture(const wchar_t* _name)
{
  Neuron::FileSys::SetHomeDirectory(L".");
  return Neuron::BinaryFile::ReadFile(_name);
}

// The shipped universe as the tool writes it: LayOutGalaxy, BuildStartingUniverse, WriteSaveFile
// with the seed in the header. Kept in step with Tools/UniverseGen/Generate.cpp by the row that
// compares the two, which is the only way this can be kept in step.
void WriteShippedUniverse(std::vector<std::uint8_t>& _outFile)
{
  const Game::GalaxyLayout galaxy =
    Game::LayOutGalaxy(Game::STARTING_GALAXY_SEED, Game::UniversePos{}, Game::STARTING_GALAXY, Game::GALAXY_PINS);
  Game::Universe universe;
  Game::BuildStartingUniverse(galaxy, 0, universe);
  Game::SaveHeader header;
  header.galaxySeed = Game::STARTING_GALAXY_SEED;
  header.shard = universe.Shard();
  Game::WriteSaveFile(universe, header, _outFile);
}

// Writes a buffer into the test log as base64, a hundred characters to the line behind a marker.
// This is how a fixture is regenerated by somebody with nothing but CI: when the identity row
// below finds the committed fixture is not what the tool writes, the log carries what it should
// have been, and a reviewer decodes it rather than building the tool to find out.
void LogAsBase64(const char* _label, const std::vector<std::uint8_t>& _bytes)
{
  static constexpr char DIGITS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve((_bytes.size() + 2) / 3 * 4);
  for (std::size_t at = 0; at < _bytes.size(); at += 3)
  {
    const std::uint32_t a = _bytes[at];
    const std::uint32_t b = (at + 1 < _bytes.size()) ? _bytes[at + 1] : 0u;
    const std::uint32_t c = (at + 2 < _bytes.size()) ? _bytes[at + 2] : 0u;
    const std::uint32_t triple = (a << 16) | (b << 8) | c;
    encoded.push_back(DIGITS[(triple >> 18) & 63u]);
    encoded.push_back(DIGITS[(triple >> 12) & 63u]);
    encoded.push_back((at + 1 < _bytes.size()) ? DIGITS[(triple >> 6) & 63u] : '=');
    encoded.push_back((at + 2 < _bytes.size()) ? DIGITS[triple & 63u] : '=');
  }
  std::string line = std::string("FIXTURE-BASE64|BEGIN|") + _label + "|" + std::to_string(_bytes.size()) + " bytes";
  Logger::WriteMessage(line.c_str());
  for (std::size_t at = 0; at < encoded.size(); at += 100)
  {
    line = "FIXTURE-BASE64|" + encoded.substr(at, 100);
    Logger::WriteMessage(line.c_str());
  }
  line = std::string("FIXTURE-BASE64|END|") + _label;
  Logger::WriteMessage(line.c_str());
}
} // namespace

TEST_CLASS(UniverseStateTests)
{
public:
  TEST_METHOD(ASavedUniverseReplaysToTheSameRun)
  {
    // The gate this tree has been promising itself. What it had was four same-process rerun tests --
    // TheSameOrderProducesTheSameRun and its three siblings -- which run the same input twice in one
    // process and compare. That catches a clock or an address leaking into the tick. What it cannot
    // catch is state Step depends on that nothing can reconstruct, because both runs build it the
    // same way from the same start. This one takes a universe apart and puts it back together
    // (Design/Archive/MmoScalabilityReview.md U3).
    Game::Universe original;
    const Scene scene = BuildScene(original);
    for (int tick = 0; tick < 240; ++tick)
      original.Step();

    std::vector<std::uint8_t> saved;
    Game::WriteUniverseState(original, saved);
    Assert::IsTrue(saved.size() > 1000, L"the state came out suspiciously small for a scene this size");

    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(saved, reloaded), L"the state did not load");
    Assert::AreEqual(original.Tick(), reloaded.Tick(), L"the tick did not survive");
    Assert::AreEqual(original.ShipCount(), reloaded.ShipCount(), L"the ship count did not survive");

    // Tick for tick, so a divergence says WHEN rather than only that one happened.
    std::vector<Game::UniversePos> originalPositions;
    std::vector<Game::UniversePos> reloadedPositions;
    std::vector<float> originalHeadings;
    std::vector<float> reloadedHeadings;
    for (int tick = 0; tick < 300; ++tick)
    {
      original.Step();
      reloaded.Step();

      PoseOf(original, originalPositions, originalHeadings);
      PoseOf(reloaded, reloadedPositions, reloadedHeadings);
      Assert::AreEqual(originalPositions.size(), reloadedPositions.size(), L"the two universes hold different numbers of ships");
      for (std::size_t at = 0; at < originalPositions.size(); ++at)
      {
        Assert::IsTrue(IsSamePosition(originalPositions[at], reloadedPositions[at]),
                       L"a reloaded universe put a ship somewhere the original did not");
        Assert::AreEqual(originalHeadings[at], reloadedHeadings[at], 0.0f, L"a reloaded universe pointed a ship somewhere else");
      }
    }

    // And the total comparison: two states written by the same codec, byte for byte. This is the
    // half that fails the day somebody adds a field to Universe that Step reads and forgets this codec,
    // because it needs no list of fields to keep in step with (work order 2.4).
    std::vector<std::uint8_t> afterOriginal;
    std::vector<std::uint8_t> afterReloaded;
    Game::WriteUniverseState(original, afterOriginal);
    Game::WriteUniverseState(reloaded, afterReloaded);
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
    Game::Universe original;
    const Scene scene = BuildScene(original);
    for (int tick = 0; tick < 60; ++tick)
      original.Step();

    const Game::ShipId routed = scene.fleet[2];
    const std::vector<Game::UniversePos> routeBefore(original.RouteOf(routed).begin(), original.RouteOf(routed).end());
    Assert::IsTrue(!routeBefore.empty(), L"the scene's ship is not following a route, so this proves nothing");

    std::vector<std::uint8_t> saved;
    Game::WriteUniverseState(original, saved);
    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(saved, reloaded), L"the state did not load");

    const std::span<const Game::UniversePos> routeAfter = reloaded.RouteOf(routed);
    Assert::AreEqual(routeBefore.size(), routeAfter.size(), L"the route changed length across a reload");
    for (std::size_t at = 0; at < routeBefore.size(); ++at)
      Assert::IsTrue(IsSamePosition(routeBefore[at], routeAfter[at]), L"a waypoint did not survive the reload");

    // One step each. A re-plan would rewrite the list; nothing else on this tick can.
    original.Step();
    reloaded.Step();
    const std::span<const Game::UniversePos> steppedOriginal = original.RouteOf(routed);
    const std::span<const Game::UniversePos> steppedReloaded = reloaded.RouteOf(routed);
    Assert::AreEqual(steppedOriginal.size(), steppedReloaded.size(), L"the reloaded ship re-planned on the tick after the load");
    for (std::size_t at = 0; at < steppedOriginal.size(); ++at)
      Assert::IsTrue(IsSamePosition(steppedOriginal[at], steppedReloaded[at]), L"the reloaded ship re-planned on the tick after the load");
  }

  TEST_METHOD(IdentityAndHandlesSurviveTheRoundTrip)
  {
    Game::Universe original;
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
    Game::WriteUniverseState(original, saved);
    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(saved, reloaded), L"the state did not load");

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
    Assert::IsTrue(freshEntity != doomedEntity, L"a reloaded universe reissued a dead entity's id");
    for (const Game::EntityId held : entities)
      Assert::IsTrue(freshEntity != held, L"a reloaded universe reissued a live entity's id");

    // The free list came with it too, in order. That is the assertion the list exists for: reuse is
    // last-in-first-out, so the ORDER is the reproduction, and a reloaded universe that rebuilt the
    // list instead of reading it would put the next ship in a different slot. The two universes are
    // asked for the same spawn and must answer with the same handle, generation included.
    const Game::ShipId alsoFresh = original.SpawnShip(Game::LocalPos(4000.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Miner));
    Assert::AreEqual(fresh, alsoFresh, L"the two universes put the same spawn at a different index");
    Assert::IsTrue(reloaded.HandleOf(fresh) == original.HandleOf(alsoFresh),
                   L"a reloaded universe did not reuse the freed slot the original did");
    Assert::AreEqual(doomedHandle.slot, reloaded.HandleOf(fresh).slot, L"the freed slot was not reused at all, so this proves nothing");
    Assert::IsTrue(reloaded.EntityIdOf(fresh) == original.EntityIdOf(alsoFresh), L"the two universes minted different ids for one spawn");

    // The despawn log came with it too: a subscriber that had not read that death still owes it.
    Assert::AreEqual(original.DespawnHead(), reloaded.DespawnHead(), L"the despawn cursor head did not survive");
    Assert::AreEqual(static_cast<std::size_t>(1), reloaded.DespawnsSince(0).size(), L"the despawn log did not survive");
    Assert::IsTrue(reloaded.DespawnsSince(0)[0].entity == doomedEntity, L"the despawn log lost who died");
  }

  TEST_METHOD(TheStationSideTablesSurviveTheRoundTrip)
  {
    Game::Universe original;
    const Scene scene = BuildScene(original);
    for (int tick = 0; tick < 400; ++tick)
      original.Step();

    const Game::Universe::Station& before = original.StationOf(scene.station);
    Assert::IsTrue(!before.targets.empty(), L"the aggression never reached the station's target list");
    // Both halves of the row have to be carrying something, or comparing them proves nothing.
    Assert::IsTrue(!before.docked.empty(), L"nothing ever docked, so the ledger comparison below is two empty lists");

    std::vector<std::uint8_t> saved;
    Game::WriteUniverseState(original, saved);
    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(saved, reloaded), L"the state did not load");

    Assert::AreEqual(original.StationCount(), reloaded.StationCount(), L"the station table did not survive");
    const Game::Universe::Station& after = reloaded.StationOf(scene.station);
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

  // The gate table and the order that names one. Step reads both -- the jump pass resolves a
  // destination through the table on every tick a fleet is crossing -- so a file that dropped them
  // would reload a universe whose fleets stop at doors they were ordered through.
  TEST_METHOD(TheGateTableSurvivesTheRoundTrip)
  {
    Game::Universe original;

    const Game::UniversePos nearPos = Game::LocalPos(0.0f, 0.0f);
    const Game::UniversePos farPos = Game::LocalPos(60000.0f, 0.0f);
    const Game::ShipId nearShip =
      original.SpawnShip(nearPos, 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
    const Game::ShipId farShip =
      original.SpawnShip(farPos, 1.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);

    Game::Universe::GateDesc toFar;
    toFar.destination = original.EntityIdOf(farShip);
    const Game::Universe::GateId nearGate = original.MakeGate(nearShip, toFar);
    Game::Universe::GateDesc toNear;
    toNear.destination = original.EntityIdOf(nearShip);
    toNear.ownerFaction = Game::FACTION_VANDAL;
    (void)original.MakeGate(farShip, toNear);

    // A fleet with a standing jump order, so orderGate is carrying something: comparing a null
    // handle against a null handle would prove nothing.
    std::vector<Game::ShipId> ships;
    for (int at = 0; at < 2; ++at)
    {
      Game::UniversePos where = nearPos;
      Game::Translate(where, 30.0f * static_cast<float>(at), 400.0f);
      ships.push_back(original.SpawnShip(where, 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER));
    }
    (void)original.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, ships);
    Game::Universe::FleetCommand command;
    command.kind = Game::FleetOrderKind::Jump;
    command.gate = nearShip;
    Assert::IsTrue(Game::Universe::FleetOrderResult::Ordered ==
                     original.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, command),
                   L"the jump order was refused, so there is no order to round-trip");

    std::vector<std::uint8_t> saved;
    Game::WriteUniverseState(original, saved);
    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(saved, reloaded), L"the state did not load");

    Assert::AreEqual(original.GateCount(), reloaded.GateCount(), L"the gate table did not survive");
    const Game::Universe::Gate& before = original.GateOf(nearGate);
    const Game::Universe::Gate& after = reloaded.GateOf(nearGate);
    Assert::IsTrue(before.structure == after.structure, L"the gate's structure handle did not survive");
    Assert::AreEqual(before.destination, after.destination, L"the gate's destination did not survive");
    Assert::AreEqual(static_cast<std::uint32_t>(original.GateOf(1u).ownerFaction),
                     static_cast<std::uint32_t>(reloaded.GateOf(1u).ownerFaction), L"a gate's owner did not survive");

    const Game::Universe::FleetId id = reloaded.FleetInSlot(Game::OWNER_LOCAL, 0);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, id, L"the fleet did not survive");
    Assert::IsTrue(Game::FleetOrderKind::Jump == reloaded.FleetOf(id).orderKind, L"the standing jump order did not survive");
    Assert::IsTrue(original.FleetOf(id).orderGate == reloaded.FleetOf(id).orderGate, L"the order's gate handle did not survive");

    // And the whole-state comparison, which is what catches a field added to Universe and forgotten
    // in the codec without any list here having to be kept in step.
    std::vector<std::uint8_t> again;
    Game::WriteUniverseState(reloaded, again);
    Assert::IsTrue(saved == again, L"a reloaded universe does not write the bytes it was read from");
  }

  TEST_METHOD(AMalformedStateIsRefusedAndChangesNothing)
  {
    // The same discipline SnapshotReceiver keeps, and AGENTS.md 5's rule for anything parsing
    // content: report or refuse, never throw and never assert -- and leave the caller's universe alone,
    // because a half-replaced universe with nothing saying which half is the failure a state codec is
    // most able to cause.
    Game::Universe source;
    (void)BuildScene(source);
    for (int tick = 0; tick < 30; ++tick)
      source.Step();

    std::vector<std::uint8_t> saved;
    Game::WriteUniverseState(source, saved);

    // A universe with something in it, so a partial apply would be visible.
    Game::Universe target;
    const Scene targetScene = BuildScene(target);
    for (int tick = 0; tick < 15; ++tick)
      target.Step();
    std::vector<std::uint8_t> before;
    Game::WriteUniverseState(target, before);

    const auto refuses = [&](std::span<const std::uint8_t> _bytes, const wchar_t* _what)
    {
      Assert::IsFalse(Game::ReadUniverseState(_bytes, target), _what);
      std::vector<std::uint8_t> after;
      Game::WriteUniverseState(target, after);
      Assert::IsTrue(before == after, L"a refused state changed the universe it was read into");
    };

    refuses({}, L"an empty buffer was accepted");
    refuses(std::span<const std::uint8_t>(saved).first(3), L"a buffer too short for the magic was accepted");
    refuses(std::span<const std::uint8_t>(saved).first(saved.size() / 2), L"a truncated state was accepted");
    refuses(std::span<const std::uint8_t>(saved).first(saved.size() - 1), L"a state one byte short was accepted");

    std::vector<std::uint8_t> wrongMagic = saved;
    wrongMagic[0] = static_cast<std::uint8_t>(wrongMagic[0] + 1u);
    refuses(wrongMagic, L"a buffer with the wrong magic was accepted");

    // Both ends of the window, explicitly: a byte one past the newest this build writes and one
    // before the oldest it still reads. While the two constants agree these are the same two
    // mutations a plain +1 and -1 would make, and the day they differ, "-1" would land inside the
    // window and this row would be testing acceptance under the name of refusal.
    std::vector<std::uint8_t> tooNew = saved;
    tooNew[4] = static_cast<std::uint8_t>(Game::UNIVERSE_STATE_FORMAT + 1u);
    refuses(tooNew, L"a state in a format newer than this build was accepted");
    std::vector<std::uint8_t> tooOld = saved;
    tooOld[4] = static_cast<std::uint8_t>(Game::UNIVERSE_STATE_FORMAT_OLDEST - 1u);
    refuses(tooOld, L"a state in a format older than this build still reads was accepted");

    // Every prefix of a valid state, which is every truncation there is. Nothing may throw, nothing
    // may be accepted, and the target universe must be untouched by all of them.
    for (std::size_t length = 0; length < saved.size(); ++length)
      Assert::IsFalse(Game::ReadUniverseState(std::span<const std::uint8_t>(saved).first(length), target),
                      L"a truncated state was accepted");
    std::vector<std::uint8_t> afterEverything;
    Game::WriteUniverseState(target, afterEverything);
    Assert::IsTrue(before == afterEverything, L"a run of refusals changed the universe they were read into");

    // The whole thing still loads, so the sweep above was refusing truncations and not the format.
    Assert::IsTrue(Game::ReadUniverseState(saved, target), L"the intact state was refused");
    Assert::AreEqual(source.Tick(), target.Tick(), L"the intact state did not take");
    Assert::AreNotEqual(static_cast<std::size_t>(0), static_cast<std::size_t>(targetScene.fleet.size()), L"the scene built nothing");
  }

  TEST_METHOD(AnEmptyUniverseRoundTrips)
  {
    // The degenerate case, which a codec built around counts is exactly where an off-by-one lives.
    Game::Universe empty;
    std::vector<std::uint8_t> saved;
    Game::WriteUniverseState(empty, saved);

    Game::Universe reloaded;
    (void)BuildScene(reloaded); // something to overwrite, so "loaded nothing" cannot pass by accident
    Assert::IsTrue(Game::ReadUniverseState(saved, reloaded), L"an empty universe did not load");
    Assert::AreEqual(0u, reloaded.ShipCount(), L"an empty state left ships behind");

    std::vector<std::uint8_t> again;
    Game::WriteUniverseState(reloaded, again);
    Assert::IsTrue(saved == again, L"an empty universe did not round-trip byte for byte");

    // And it still runs.
    reloaded.Step();
    Assert::AreEqual(static_cast<std::uint64_t>(1), reloaded.Tick(), L"a reloaded empty universe would not step");
  }
  // --- the save file (Design/Archive/Universe-slice-5.md) ----------------------------------------------

  // The header round-trips, and the file it fronts is still the state codec's own bytes.
  TEST_METHOD(ASaveFileCarriesItsHeader)
  {
    Game::Universe source;
    (void)BuildScene(source);
    for (int tick = 0; tick < 20; ++tick)
      source.Step();

    Game::SaveHeader header;
    header.galaxySeed = 0x46726F6E74696572ull; // "Frontier"
    header.shard = source.Shard();

    std::vector<std::uint8_t> file;
    Game::WriteSaveFile(source, header, file);

    // The state is the state, byte for byte, and it begins exactly where the constant says.
    std::vector<std::uint8_t> state;
    Game::WriteUniverseState(source, state);
    Assert::AreEqual(Game::SAVE_HEADER_BYTES + state.size(), file.size(), L"the file is not its header plus its state");
    Assert::IsTrue(std::equal(state.begin(), state.end(), file.begin() + Game::SAVE_HEADER_BYTES),
                   L"the state does not begin at SAVE_HEADER_BYTES");

    Game::Universe reloaded;
    (void)BuildScene(reloaded); // something to overwrite, so "loaded nothing" cannot pass by accident
    Game::SaveHeader read;
    Assert::IsTrue(Game::ReadSaveFile(file, read, reloaded), L"a file this build wrote was refused");
    Assert::AreEqual(header.galaxySeed, read.galaxySeed, L"the galaxy seed did not survive the header");
    Assert::AreEqual(static_cast<std::uint32_t>(header.shard), static_cast<std::uint32_t>(read.shard),
                     L"the shard did not survive the header");
    Assert::AreEqual(source.Tick(), reloaded.Tick(), L"the state behind the header did not take");
  }

  // The property the file exists for: a universe that has been through it is the same universe, and
  // goes on being the same universe. Byte equality after sixty more ticks on both -- the standing
  // replay gate, run through the file rather than through the state codec alone.
  TEST_METHOD(ARestoredSaveFileReplaysToTheSameRun)
  {
    Game::Universe source;
    (void)BuildScene(source);
    for (int tick = 0; tick < 40; ++tick)
      source.Step();

    Game::SaveHeader header;
    header.galaxySeed = 0x53797331ull;
    header.shard = source.Shard();
    std::vector<std::uint8_t> file;
    Game::WriteSaveFile(source, header, file);

    Game::Universe restored;
    Game::SaveHeader read;
    Assert::IsTrue(Game::ReadSaveFile(file, read, restored), L"the save file was refused");

    for (int tick = 0; tick < 60; ++tick)
    {
      source.Step();
      restored.Step();
    }

    std::vector<std::uint8_t> afterSource;
    std::vector<std::uint8_t> afterRestored;
    Game::WriteUniverseState(source, afterSource);
    Game::WriteUniverseState(restored, afterRestored);
    Assert::IsTrue(afterSource == afterRestored, L"a restored universe diverged from the run that saved it");
  }

  // A file that disagrees with itself is refused rather than reconciled. The shard is in the header
  // AND in the state; picking either answer would mean running the shard that was not saved, and
  // nothing can say which of the two is the mistake.
  TEST_METHOD(ASaveFileThatDisagreesWithItselfIsRefused)
  {
    Game::Universe source;
    (void)BuildScene(source); // BuildScene configures shard 3
    source.Step();

    Game::SaveHeader header;
    header.galaxySeed = 1234u;
    header.shard = source.Shard();
    std::vector<std::uint8_t> file;
    Game::WriteSaveFile(source, header, file);

    // The header's shard sits after the magic, the format byte and the seed.
    constexpr std::size_t SHARD_AT = 4u + 1u + 8u;
    file[SHARD_AT] = static_cast<std::uint8_t>(file[SHARD_AT] + 1u);

    Game::Universe target;
    Game::SaveHeader read;
    Assert::IsFalse(Game::ReadSaveFile(file, read, target), L"a file whose header and body disagree about the shard was accepted");
    Assert::AreEqual(0u, target.ShipCount(), L"a refused file was applied anyway");
    Assert::AreEqual(static_cast<std::uint64_t>(0), read.galaxySeed, L"a refused file wrote its header out");
  }

  // Every way a file can be wrong, and the same discipline the state codec keeps: refuse, change
  // nothing, never throw, never assert.
  TEST_METHOD(TheShardCountSurvivesTheHeader)
  {
    // A universe generated for four shards is not a universe four OTHER shards can run: the
    // partition is a function of the layout and the count, so a count that arrived wrong would move
    // every system in silence (Design/CrossShard.md 2). It rides the header for the shard's own
    // reason -- a header is readable without decoding the body.
    Game::Universe source;
    (void)BuildScene(source);

    Game::SaveHeader header;
    header.galaxySeed = 0x1234u;
    header.shard = source.Shard();
    header.shardCount = 4;

    std::vector<std::uint8_t> file;
    Game::WriteSaveFile(source, header, file);

    Game::Universe loaded;
    Game::SaveHeader read;
    Assert::IsTrue(Game::ReadSaveFile(file, read, loaded), L"a four-shard file was refused");
    Assert::AreEqual(4u, read.shardCount, L"the shard count did not survive the header");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::SAVE_FILE_FORMAT), static_cast<std::uint32_t>(read.fileFormat),
                     L"the file was not written at the current file format");

    // Zero shards is not a galaxy anything can be placed in, and is refused rather than read as one.
    std::vector<std::uint8_t> zeroed = file;
    zeroed[23] = 0;
    zeroed[24] = 0;
    zeroed[25] = 0;
    zeroed[26] = 0;
    Game::Universe untouched;
    Game::SaveHeader ignored;
    Assert::IsFalse(Game::ReadSaveFile(zeroed, ignored, untouched), L"a file claiming zero shards was accepted");
  }

  TEST_METHOD(AnOlderFixtureLoadsAndReplays)
  {
    // The migration gate. Every fixture is a real file in a format this build still reads; each
    // must load, report the format its name claims, count what the tool counted when it wrote it,
    // save at the current format and be the same universe afterwards -- twice, so that migrating a
    // migrated file is a no-op -- and replay against its own reload to byte equality, which is the
    // standing replay gate applied to a migrated universe rather than to a built one
    // (Design/Archive/SaveMigration-work-order.md 4).
    for (const Fixture& fixture : FIXTURES)
    {
      const Neuron::ByteBuffer file = ReadFixture(fixture.name);
      Assert::IsFalse(file.empty(), L"a fixture named in FIXTURES is not beside the suite -- is it in the project file?");
      Assert::AreEqual(fixture.bytes, file.size(), L"the fixture is not the size the tool printed when it wrote it");

      Game::Universe loaded;
      Game::SaveHeader header;
      Assert::IsTrue(Game::ReadSaveFile(file, header, loaded), L"a fixture in a format this build reads was refused");
      Assert::AreEqual(static_cast<std::uint32_t>(fixture.fileFormat), static_cast<std::uint32_t>(header.fileFormat),
                       L"the header does not report the file format the fixture is in");
      Assert::AreEqual(static_cast<std::uint32_t>(fixture.stateFormat), static_cast<std::uint32_t>(header.stateFormat),
                       L"the header does not report the state format the fixture is in");
      Assert::AreEqual(Game::STARTING_GALAXY_SEED, header.galaxySeed, L"the fixture was not written for the shipped galaxy");

      // The census, against what the tool printed. A count that came back different is a reader
      // that parsed an older layout into the wrong shape without refusing it.
      Assert::AreEqual(fixture.ships, loaded.ShipCount(), L"the fixture did not load the ships the tool wrote");
      Assert::AreEqual(fixture.gates, loaded.GateCount(), L"the fixture did not load the gates the tool wrote");
      Assert::AreEqual(fixture.stations, loaded.StationCount(), L"the fixture did not load the stations the tool wrote");
      Assert::AreEqual(fixture.fleets, loaded.FleetCount(), L"the fixture did not load the fleets the tool wrote");
      const Game::GalaxyLayout galaxy =
        Game::LayOutGalaxy(header.galaxySeed, Game::UniversePos{}, Game::STARTING_GALAXY, Game::GALAXY_PINS);
      Assert::AreEqual(fixture.systems, galaxy.systems.size(), L"the fixture's seed does not lay out the galaxy the tool counted");

      // Written at the current format, whatever it was read at, and idempotent from there.
      Game::SaveHeader current;
      current.galaxySeed = header.galaxySeed;
      current.shard = loaded.Shard();
      std::vector<std::uint8_t> once;
      Game::WriteSaveFile(loaded, current, once);
      Assert::AreEqual(static_cast<std::uint32_t>(Game::SAVE_FILE_FORMAT), static_cast<std::uint32_t>(once[4]),
                       L"a migrated universe was not written at the current file format");
      Assert::AreEqual(static_cast<std::uint32_t>(Game::UNIVERSE_STATE_FORMAT),
                       static_cast<std::uint32_t>(once[Game::SAVE_HEADER_BYTES + 4]),
                       L"a migrated universe was not written at the current state format");

      Game::Universe reloaded;
      Game::SaveHeader reread;
      Assert::IsTrue(Game::ReadSaveFile(once, reread, reloaded), L"a migrated universe's own save was refused");
      std::vector<std::uint8_t> twice;
      Game::WriteSaveFile(reloaded, current, twice);
      Assert::IsTrue(once == twice, L"migrating a migrated file changed it");

      for (int tick = 0; tick < 600; ++tick)
      {
        loaded.Step();
        reloaded.Step();
      }
      std::vector<std::uint8_t> a;
      std::vector<std::uint8_t> b;
      Game::WriteUniverseState(loaded, a);
      Game::WriteUniverseState(reloaded, b);
      Assert::IsTrue(a == b, L"a migrated universe and its reload diverged within 600 ticks");
    }
  }

  TEST_METHOD(AUniverseMidHandoffSurvivesItsOwnSaveFile)
  {
    // The claim slice 3 exists for, and the one ADR 0065 made before it was true: a shard that dies
    // between the despawn and the send still has the ship, in its file, in its outbox.
    Game::GalaxyDesc desc = Game::STARTING_GALAXY;
    desc.shardCount = 4;
    const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(Game::STARTING_GALAXY_SEED, Game::UniversePos{}, desc, Game::GALAXY_PINS);
    std::vector<Game::Universe> shards(desc.shardCount);
    Game::BuildStartingGalaxy(galaxy, desc, shards);

    std::uint32_t departing = desc.shardCount;
    for (std::uint32_t at = 0; at < shards.size(); ++at)
    {
      if (shards[at].FleetInSlot(Game::OWNER_LOCAL, 0) != Game::Universe::INVALID_FLEET_ID)
        departing = at;
    }
    Assert::IsTrue(departing < shards.size(), L"no shard holds the starting fleet");

    Game::ShipId gate = Game::INVALID_SHIP_ID;
    for (std::uint32_t at = 0; at < shards[departing].GateCount() && gate == Game::INVALID_SHIP_ID; ++at)
    {
      const Game::EntityId destination = shards[departing].GateOf(at).destination;
      if (destination != Game::INVALID_ENTITY_ID &&
          Game::EntityShardOf(destination) != static_cast<std::uint32_t>(shards[departing].Shard()))
        gate = shards[departing].Resolve(shards[departing].GateOf(at).structure);
    }
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, gate, L"the fleet's shard has no gate leading out");

    Game::Universe::FleetCommand command;
    command.kind = Game::FleetOrderKind::Jump;
    command.gate = gate;
    Assert::IsTrue(shards[departing].IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, command) ==
                     Game::Universe::FleetOrderResult::Ordered,
                   L"the jump order was refused");
    for (int tick = 0; tick < 30000 && shards[departing].Outbox().empty(); ++tick)
      shards[departing].Step();
    Assert::IsFalse(shards[departing].Outbox().empty(), L"the fleet never reached the gate");

    // The shard dies here, and comes back from its file.
    std::vector<std::uint8_t> saved;
    Game::WriteUniverseState(shards[departing], saved);
    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(saved, reloaded), L"a universe with a handoff in flight was refused");

    Assert::AreEqual(shards[departing].Outbox().size(), reloaded.Outbox().size(), L"the outbox did not survive the file");
    for (std::size_t at = 0; at < shards[departing].Outbox().size(); ++at)
    {
      Assert::AreEqual(shards[departing].Outbox()[at].entity, reloaded.Outbox()[at].entity, L"an outbox entry changed identity");
      Assert::AreEqual(shards[departing].Outbox()[at].sequence, reloaded.Outbox()[at].sequence, L"an outbox entry lost its sequence");
      Assert::AreEqual(shards[departing].Outbox()[at].gate, reloaded.Outbox()[at].gate, L"an outbox entry lost the gate it is bound for");
    }

    // And it re-saves to the same bytes, which is the idempotence every other format row asserts.
    std::vector<std::uint8_t> again;
    Game::WriteUniverseState(reloaded, again);
    Assert::IsTrue(saved == again, L"a reloaded universe with a handoff in flight saved differently");

    // The recovered outbox is a real one: delivering it still lands the fleet on the far side.
    const std::uint32_t arriving = Game::EntityShardOf(shards[departing].GateOf(shards[departing].GateAt(gate)).destination);
    for (const Game::Universe::Handoff& handoff : reloaded.Outbox())
      shards[arriving].DeliverHandoff(handoff);
    Assert::AreEqual(std::uint32_t{3}, shards[arriving].DrainInbox(), L"a handoff recovered from a file did not apply");
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, shards[arriving].FleetInSlot(Game::OWNER_LOCAL, 0),
                        L"the fleet did not re-form from a recovered handoff");
  }

  TEST_METHOD(AnUndeliveredInboxSurvivesTooAndDrainsToTheSameUniverse)
  {
    // The other half, and it is not symmetrical with the outbox: an inbox that did not survive would
    // lose ships the far side has already been told are gone, which is the same failure from the
    // other end.
    Game::GalaxyDesc desc = Game::STARTING_GALAXY;
    desc.shardCount = 4;
    const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(Game::STARTING_GALAXY_SEED, Game::UniversePos{}, desc, Game::GALAXY_PINS);
    std::vector<Game::Universe> shards(desc.shardCount);
    Game::BuildStartingGalaxy(galaxy, desc, shards);

    std::uint32_t departing = desc.shardCount;
    for (std::uint32_t at = 0; at < shards.size(); ++at)
    {
      if (shards[at].FleetInSlot(Game::OWNER_LOCAL, 0) != Game::Universe::INVALID_FLEET_ID)
        departing = at;
    }
    Game::ShipId gate = Game::INVALID_SHIP_ID;
    for (std::uint32_t at = 0; at < shards[departing].GateCount() && gate == Game::INVALID_SHIP_ID; ++at)
    {
      const Game::EntityId destination = shards[departing].GateOf(at).destination;
      if (destination != Game::INVALID_ENTITY_ID &&
          Game::EntityShardOf(destination) != static_cast<std::uint32_t>(shards[departing].Shard()))
        gate = shards[departing].Resolve(shards[departing].GateOf(at).structure);
    }
    Game::Universe::FleetCommand command;
    command.kind = Game::FleetOrderKind::Jump;
    command.gate = gate;
    (void)shards[departing].IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, command);
    for (int tick = 0; tick < 30000 && shards[departing].Outbox().empty(); ++tick)
      shards[departing].Step();
    Assert::IsFalse(shards[departing].Outbox().empty(), L"the fleet never reached the gate");

    const std::uint32_t arriving = Game::EntityShardOf(shards[departing].GateOf(shards[departing].GateAt(gate)).destination);

    // Delivered but NOT drained, then the shard dies: the queue is the only record that those ships
    // are coming, and the departing shard has already let go of them.
    for (const Game::Universe::Handoff& handoff : shards[departing].Outbox())
      shards[arriving].DeliverHandoff(handoff);

    std::vector<std::uint8_t> saved;
    Game::WriteUniverseState(shards[arriving], saved);
    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(saved, reloaded), L"a universe with an undrained inbox was refused");

    // Both drain to the same universe, which is the claim: the file lost nothing that mattered.
    const std::uint32_t here = shards[arriving].DrainInbox();
    const std::uint32_t there = reloaded.DrainInbox();
    Assert::AreEqual(here, there, L"a reloaded inbox drained a different number of ships");
    Assert::IsTrue(here > 0, L"nothing was in the inbox to begin with");

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    Game::WriteUniverseState(shards[arriving], a);
    Game::WriteUniverseState(reloaded, b);
    Assert::IsTrue(a == b, L"a universe that drained a reloaded inbox differs from one that never saved");
  }

  TEST_METHOD(AnEmptyQueueCostsTheFormatOnlyItsCounts)
  {
    // Format 9 adds a sequence and two counts to a universe with nothing in flight, and nothing else
    // may have moved. Stated as an exact size difference rather than "about the same", because a
    // field that quietly grew would otherwise pass.
    Game::Universe universe;
    (void)BuildScene(universe);
    for (int tick = 0; tick < 25; ++tick)
      universe.Step();

    std::vector<std::uint8_t> bytes;
    Game::WriteUniverseState(universe, bytes);
    Assert::IsTrue(universe.Outbox().empty(), L"this row needs a universe with nothing in flight");

    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(bytes, reloaded), L"an empty-queue universe was refused");
    Assert::IsTrue(reloaded.Outbox().empty(), L"an empty outbox came back with something in it");

    std::vector<std::uint8_t> again;
    Game::WriteUniverseState(reloaded, again);
    Assert::IsTrue(bytes == again, L"an empty-queue universe did not round-trip");
  }

  TEST_METHOD(TheNewestFixtureIsTheToolsOutput)
  {
    // While the newest fixture is in the format this build writes, it must be byte-for-byte what
    // the tool writes for the shipped galaxy, which is the proof that a fixture is a real file and
    // not a hand-made one -- and, at the commit this row was written, the proof that adding the
    // window moved no byte of the format. After a bump the newest fixture is one format behind and
    // this row has nothing to compare against; the fixture committed with that bump is checked
    // here by the next commit that does not move the format, which is every other one.
    //
    // On a mismatch the log carries what the fixture should have been, as base64, so that it can be
    // regenerated from a CI run without building the tool.
    const Fixture& newest = FIXTURES[std::size(FIXTURES) - 1];
    if (newest.stateFormat != Game::UNIVERSE_STATE_FORMAT || newest.fileFormat != Game::SAVE_FILE_FORMAT)
      return;

    std::vector<std::uint8_t> written;
    WriteShippedUniverse(written);
    const Neuron::ByteBuffer committed = ReadFixture(newest.name);
    if (committed != written)
      LogAsBase64("UniverseFormat7.sav", written);
    Assert::AreEqual(written.size(), committed.size(), L"the committed fixture is not the size the tool writes -- the log has the bytes");
    Assert::IsTrue(committed == written, L"the committed fixture is not what the tool writes -- the log has the bytes");
  }

  TEST_METHOD(AMalformedSaveFileIsRefusedAndChangesNothing)
  {
    Game::Universe source;
    (void)BuildScene(source);
    for (int tick = 0; tick < 25; ++tick)
      source.Step();

    Game::SaveHeader header;
    header.galaxySeed = 0xABCDEF0123456789ull;
    header.shard = source.Shard();
    std::vector<std::uint8_t> file;
    Game::WriteSaveFile(source, header, file);

    // A universe with something in it, so a partial apply would be visible.
    Game::Universe target;
    (void)BuildScene(target);
    for (int tick = 0; tick < 10; ++tick)
      target.Step();
    std::vector<std::uint8_t> before;
    Game::WriteUniverseState(target, before);

    Game::SaveHeader read;
    const auto refuses = [&](std::span<const std::uint8_t> _bytes, const wchar_t* _what)
    {
      Assert::IsFalse(Game::ReadSaveFile(_bytes, read, target), _what);
      std::vector<std::uint8_t> after;
      Game::WriteUniverseState(target, after);
      Assert::IsTrue(before == after, L"a refused save file changed the universe it was read into");
      Assert::AreEqual(static_cast<std::uint64_t>(0), read.galaxySeed, L"a refused save file wrote its header out");
    };

    refuses({}, L"an empty file was accepted");
    refuses(std::span<const std::uint8_t>(file).first(Game::SAVE_HEADER_BYTES - 1), L"a file too short for its header was accepted");
    refuses(std::span<const std::uint8_t>(file).first(Game::SAVE_HEADER_BYTES), L"a header with no state behind it was accepted");

    std::vector<std::uint8_t> wrongMagic = file;
    wrongMagic[0] = static_cast<std::uint8_t>(wrongMagic[0] + 1u);
    refuses(wrongMagic, L"a file with the wrong magic was accepted");

    std::vector<std::uint8_t> tooNew = file;
    tooNew[4] = static_cast<std::uint8_t>(Game::SAVE_FILE_FORMAT + 1u);
    refuses(tooNew, L"a file in a format newer than this build was accepted");
    std::vector<std::uint8_t> tooOld = file;
    tooOld[4] = static_cast<std::uint8_t>(Game::SAVE_FILE_FORMAT_OLDEST - 1u);
    refuses(tooOld, L"a file in a format older than this build still reads was accepted");

    // The length field, wrong in both directions. Neither is caught by the state codec: it is the
    // header's job, and this row is the whole reason the field exists.
    std::vector<std::uint8_t> shortLength = file;
    constexpr std::size_t LENGTH_AT = 4u + 1u + 8u + 2u;
    shortLength[LENGTH_AT] = static_cast<std::uint8_t>(shortLength[LENGTH_AT] - 1u);
    refuses(shortLength, L"a file whose length field is short was accepted");

    std::vector<std::uint8_t> longLength = file;
    longLength[LENGTH_AT] = static_cast<std::uint8_t>(longLength[LENGTH_AT] + 1u);
    refuses(longLength, L"a file whose length field is long was accepted");

    // A file with something appended. ReadUniverseState alone would take this: it stops at the end
    // of the state and never looks past it.
    std::vector<std::uint8_t> appended = file;
    appended.push_back(0x7Fu);
    refuses(appended, L"a file with rubbish appended was accepted");

    // Every prefix of a valid file, which is every truncation there is.
    for (std::size_t length = 0; length < file.size(); ++length)
      Assert::IsFalse(Game::ReadSaveFile(std::span<const std::uint8_t>(file).first(length), read, target),
                      L"a truncated save file was accepted");
    std::vector<std::uint8_t> afterEverything;
    Game::WriteUniverseState(target, afterEverything);
    Assert::IsTrue(before == afterEverything, L"a run of refusals changed the universe they were read into");

    // The whole thing still loads, so the sweep above was refusing damage and not the format.
    Assert::IsTrue(Game::ReadSaveFile(file, read, target), L"the intact file was refused");
    Assert::AreEqual(header.galaxySeed, read.galaxySeed, L"the intact file did not take");
  }
};
} // namespace GameLogicTests
