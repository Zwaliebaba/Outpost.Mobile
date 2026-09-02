#include "pch.h"

#include <algorithm>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// Two gates, far enough apart that no interest radius or gather reach spans them, wired to each
// other. The pair is what a jump needs, and building it by hand rather than through genesis is what
// keeps this suite independent of slice 3.
struct GatePair
{
  Game::ShipId nearStructure = Game::INVALID_SHIP_ID;
  Game::ShipId farStructure = Game::INVALID_SHIP_ID;
  Game::Universe::GateId nearGate = Game::Universe::INVALID_GATE_ID;
  Game::Universe::GateId farGate = Game::Universe::INVALID_GATE_ID;
};

[[nodiscard]] GatePair MakeGatePair(Game::Universe& _universe, const Game::UniversePos& _nearPos, const Game::UniversePos& _farPos,
                                    float _farHeadingRad = 0.0f)
{
  GatePair pair;
  pair.nearStructure = _universe.SpawnShip(_nearPos, 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
  pair.farStructure =
    _universe.SpawnShip(_farPos, _farHeadingRad, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);

  // Each names the other by identity, which is the currency that already survives leaving a
  // universe -- so the row needs no reshaping the day the far side is on another shard (ADR 0047).
  Game::Universe::GateDesc toFar;
  toFar.destination = _universe.EntityIdOf(pair.farStructure);
  pair.nearGate = _universe.MakeGate(pair.nearStructure, toFar);

  Game::Universe::GateDesc toNear;
  toNear.destination = _universe.EntityIdOf(pair.nearStructure);
  pair.farGate = _universe.MakeGate(pair.farStructure, toNear);
  return pair;
}

// A fleet standing on the near gate's doorstep, already inside the radius, so a jump is one tick
// away and the tests below are about the crossing rather than about the flight to it.
[[nodiscard]] std::vector<Game::ShipId> FleetAtTheGate(Game::Universe& _universe, const Game::UniversePos& _gatePos, std::uint32_t _count)
{
  std::vector<Game::ShipId> ships;
  for (std::uint32_t at = 0; at < _count; ++at)
  {
    Game::UniversePos where = _gatePos;
    Game::Translate(where, 20.0f * static_cast<float>(at) - 20.0f, 30.0f);
    ships.push_back(_universe.SpawnShip(where, 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER));
  }
  (void)_universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, ships);
  return ships;
}

[[nodiscard]] Game::Universe::FleetOrderResult OrderJump(Game::Universe& _universe, Game::ShipId _gate)
{
  Game::Universe::FleetCommand command;
  command.kind = Game::FleetOrderKind::Jump;
  command.gate = _gate;
  return _universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, command);
}
} // namespace

TEST_CLASS(JumpTests)
{
public:
  // ADR 0038's pattern, re-run: the ship goes on being a Structure and the row is what knows it is
  // a road. Everything reads it through Resolve, so a row whose ship is gone reports nothing rather
  // than naming whichever ship arrived in that slot (ADR 0005).
  TEST_METHOD(AGateIsAStructureWithARow)
  {
    Game::Universe universe;
    const GatePair pair = MakeGatePair(universe, Game::LocalPos(0.0f, 0.0f), Game::LocalPos(60000.0f, 0.0f));

    Assert::AreNotEqual(Game::Universe::INVALID_GATE_ID, pair.nearGate, L"MakeGate refused a live structure");
    Assert::IsTrue(universe.IsGate(pair.nearStructure), L"the structure is not reported as a gate");
    Assert::AreEqual(pair.nearGate, universe.GateAt(pair.nearStructure), L"GateAt named a different row");
    Assert::AreEqual(universe.EntityIdOf(pair.farStructure), universe.GateOf(pair.nearGate).destination,
                     L"the gate does not lead where it was pointed");
    Assert::AreEqual(static_cast<std::uint32_t>(2), universe.GateCount(), L"the pair did not make two rows");

    // A plain ship is not a gate, and neither is an id past the end.
    const Game::ShipId ship = universe.SpawnShip(Game::LocalPos(10.0f, 10.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));
    Assert::IsFalse(universe.IsGate(ship), L"a Corvette reported itself a gate");
    Assert::AreEqual(Game::Universe::INVALID_GATE_ID, universe.GateAt(9999u), L"an id past the end named a gate");
    Assert::AreEqual(Game::Universe::INVALID_GATE_ID, universe.MakeGate(9999u, Game::Universe::GateDesc{}),
                     L"MakeGate accepted a ship that does not exist");
  }

  // The order gate, and the one refusal it has. A refused order must change nothing at all, which is
  // the rule every gate in IssueFleetOrder already follows.
  TEST_METHOD(AJumpOrderNamesAGate)
  {
    Game::Universe universe;
    const GatePair pair = MakeGatePair(universe, Game::LocalPos(0.0f, 0.0f), Game::LocalPos(60000.0f, 0.0f));
    (void)FleetAtTheGate(universe, Game::LocalPos(0.0f, 0.0f), 2);

    Assert::IsTrue(Game::Universe::FleetOrderResult::Ordered == OrderJump(universe, pair.nearStructure),
                   L"a jump at a live gate was refused");

    // A station is a structure with the other kind of row, and it is not a road.
    const Game::ShipId stationShip =
      universe.SpawnShip(Game::LocalPos(500.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
    (void)universe.MakeStation(stationShip, Game::Universe::StationDesc{});
    Assert::IsTrue(Game::Universe::FleetOrderResult::NotAGate == OrderJump(universe, stationShip), L"a jump at a station was accepted");

    const Game::Universe::FleetId id = universe.FleetInSlot(Game::OWNER_LOCAL, 0);
    Assert::IsTrue(Game::FleetOrderKind::Jump == universe.FleetOf(id).orderKind,
                   L"a refused jump replaced the standing order it should have left alone");

    // An empty slot is refused before anything is looked at.
    Game::Universe::FleetCommand command;
    command.kind = Game::FleetOrderKind::Jump;
    command.gate = pair.nearStructure;
    Assert::IsTrue(Game::Universe::FleetOrderResult::NoSuchFleet ==
                     universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 3, command),
                   L"a jump was accepted for a slot holding no fleet");
  }

  // Whole or not at all. A fleet is never half in one system and half in another -- which is a
  // sentence the fleet row cannot say, and the reason the trickle lost (ADR 0056).
  TEST_METHOD(AFleetJumpsWholeOrNotAtAll)
  {
    Game::Universe universe;
    const Game::UniversePos nearPos = Game::LocalPos(0.0f, 0.0f);
    const Game::UniversePos farPos = Game::LocalPos(60000.0f, 0.0f);
    const GatePair pair = MakeGatePair(universe, nearPos, farPos);

    // Two on the doorstep and one genuinely far away, formed into one fleet. Spawning all three at
    // the gate and *intending* one to be outside is what the first draft of this row did, and it
    // tested nothing: every member was already inside, so a per-ship crossing passed it
    // (Design/Archive/Universe-slice-2.md 7).
    std::vector<Game::ShipId> ships;
    for (int at = 0; at < 2; ++at)
    {
      Game::UniversePos where = nearPos;
      Game::Translate(where, 20.0f * static_cast<float>(at), 400.0f);
      ships.push_back(universe.SpawnShip(where, 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER));
    }
    Game::UniversePos away = nearPos;
    Game::Translate(away, 1500.0f, 0.0f);
    ships.push_back(universe.SpawnShip(away, 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER));
    (void)universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, ships);
    Assert::IsTrue(Game::Distance(universe.Ship(ships[2]).posUniverse, universe.Ship(pair.nearStructure).posUniverse) > 1000.0f,
                   L"the straggler is not actually outside the gate, so this row would prove nothing");
    // GateRangeMetres(Structure, Corvette) is 665 m: 251.8 + 13.1 of skin, plus GATE_CAPTURE_METRES.

    Assert::IsTrue(Game::Universe::FleetOrderResult::Ordered == OrderJump(universe, pair.nearStructure), L"the jump order was refused");

    // Positions, not a ship count. A jump despawns and respawns, so the count is identical either
    // way and cannot see a crossing at all -- which a mutation that crossed one ship at a time
    // survived until this row was rewritten (Design/Archive/Universe-slice-2.md 7).
    std::vector<Game::EntityId> crossing;
    for (const Game::ShipId ship : ships)
      crossing.push_back(universe.EntityIdOf(ship));

    universe.Step();
    for (const Game::EntityId entity : crossing)
    {
      const Game::ShipId now = universe.ResolveEntity(entity);
      Assert::AreNotEqual(Game::INVALID_SHIP_ID, now, L"a ship vanished before the fleet was ready to cross");
      Assert::IsTrue(Game::Distance(universe.Ship(now).posUniverse, farPos) > 10000.0f,
                     L"a ship crossed while one of its fleet was still outside the gate");
    }

    // Let it arrive. The fleet crosses on the tick the last member is inside, and every member is
    // then at the far gate.
    bool crossed = false;
    for (int tick = 0; tick < 6000 && !crossed; ++tick)
    {
      universe.Step();
      const Game::Universe::FleetId id = universe.FleetInSlot(Game::OWNER_LOCAL, 0);
      if (id == Game::Universe::INVALID_FLEET_ID)
        break;
      const Game::Universe::Fleet& fleet = universe.FleetOf(id);
      std::uint32_t atFar = 0;
      for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
      {
        const Game::ShipId member = universe.Resolve(fleet.members[at]);
        if (member != Game::INVALID_SHIP_ID && Game::Distance(universe.Ship(member).posUniverse, farPos) < 2000.0f)
          ++atFar;
      }
      crossed = atFar == 3;
    }
    Assert::IsTrue(crossed, L"the fleet never crossed, or crossed without all three members");
  }

  // The whole of what a jump is: the ship that left is the ship that arrives. Handles die, because a
  // handle names a life in one universe; identity does not, because it is what outlives one.
  TEST_METHOD(AJumpKeepsIdentityAndDamage)
  {
    Game::Universe universe;
    const Game::UniversePos nearPos = Game::LocalPos(0.0f, 0.0f);
    const Game::UniversePos farPos = Game::LocalPos(60000.0f, 0.0f);
    const GatePair pair = MakeGatePair(universe, nearPos, farPos);
    // Unarmed hulls, so the fleet cannot shoot its attacker down and the wound below actually
    // accumulates. A Miner is a fleet member like any other -- it is the armed hulls that turn, and
    // these have none (ADR 0050).
    std::vector<Game::ShipId> ships;
    for (int at = 0; at < 2; ++at)
    {
      // Well clear of the gate's own skin, which reaches 251.8 m from its centre. Parked closer, the
      // raider ends up pressed against the structure and correctly shoots *that* instead -- it is
      // the nearest hostile surface, and it is indestructible, so nobody is ever hurt (ADR 0003).
      Game::UniversePos where = nearPos;
      Game::Translate(where, 40.0f * static_cast<float>(at), 1200.0f);
      ships.push_back(universe.SpawnShip(where, 0.0f, static_cast<std::uint32_t>(Game::HullId::Miner), Game::FACTION_PLAYER));
    }
    (void)universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, ships);

    std::vector<Game::EntityId> before;
    for (const Game::ShipId ship : ships)
      before.push_back(universe.EntityIdOf(ship));

    // A real wound to carry across, taken from a real gun: there is no other way to lower a hull,
    // and a test that asserted a full hull arrived full would prove nothing at all.
    // South of the fleet and pointed north at it: an Interceptor's guns are fixed to its bow, so a
    // raider parked abeam would sit there aiming at nothing (AGENTS.md, no turret that turns).
    const Game::ShipId raider =
      universe.SpawnShip(Game::LocalPos(0.0f, 1120.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    const Game::EntityId raiderEntity = universe.EntityIdOf(raider);

    bool wounded = false;
    for (int tick = 0; tick < 600 && !wounded; ++tick)
    {
      universe.Step();
      for (const Game::EntityId entity : before)
      {
        const Game::ShipId now = universe.ResolveEntity(entity);
        wounded = wounded || (now != Game::INVALID_SHIP_ID &&
                              universe.Ship(now).hullPoints < Game::HullSpecOf(universe.Ship(now).hullId).maxHullPoints);
      }
    }
    Assert::IsTrue(wounded, L"nobody was hurt, so this row would prove nothing about damage");

    // The attacker leaves, so the hull numbers stop moving and the comparison below is about the
    // crossing rather than about the fight.
    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(raider)), L"the raider did not despawn");
    universe.Step();

    std::vector<std::uint32_t> hullBefore;
    for (const Game::EntityId entity : before)
    {
      const Game::ShipId now = universe.ResolveEntity(entity);
      Assert::AreNotEqual(Game::INVALID_SHIP_ID, now, L"a ship died in the fight, so the crossing cannot be measured");
      hullBefore.push_back(universe.Ship(now).hullPoints);
    }

    Assert::IsTrue(Game::Universe::FleetOrderResult::Ordered == OrderJump(universe, pair.nearStructure), L"the jump order was refused");
    bool crossed = false;
    for (int tick = 0; tick < 6000 && !crossed; ++tick)
    {
      universe.Step();
      const Game::ShipId first = universe.ResolveEntity(before[0]);
      crossed = first != Game::INVALID_SHIP_ID && Game::Distance(universe.Ship(first).posUniverse, farPos) < 2000.0f;
    }
    Assert::IsTrue(crossed, L"the fleet never crossed");

    for (std::size_t at = 0; at < before.size(); ++at)
    {
      const Game::ShipId now = universe.ResolveEntity(before[at]);
      Assert::AreNotEqual(Game::INVALID_SHIP_ID, now, L"a ship did not arrive");
      Assert::AreEqual(hullBefore[at], universe.Ship(now).hullPoints, L"a hull was repaired or wrecked by crossing a gate");
    }

    for (const Game::EntityId entity : before)
    {
      const Game::ShipId now = universe.ResolveEntity(entity);
      Assert::AreNotEqual(Game::INVALID_SHIP_ID, now, L"an identity did not survive the jump");
      Assert::IsTrue(Game::Distance(universe.Ship(now).posUniverse, farPos) < 2000.0f, L"a jumped ship did not arrive at the far gate");
    }

    // The departure was stated, and stated as a jump rather than as a death. The raider is excluded
    // because this test killed it on purpose to freeze the hull numbers.
    bool sawJump = false;
    for (const Game::DespawnRecord& gone : universe.DespawnsSince(0))
    {
      if (gone.entity == raiderEntity)
        continue;
      const bool mine = std::find(before.begin(), before.end(), gone.entity) != before.end();
      if (!mine)
        continue;
      sawJump = sawJump || gone.cause == Game::DespawnCause::JumpedOut;
      Assert::IsFalse(gone.cause == Game::DespawnCause::Destroyed, L"a jump was logged as a death");
    }
    Assert::IsTrue(sawJump, L"no departure was logged for the jump");
  }

  // Identity and damage cross; intent does not. Every one of these is something the far side
  // re-derives, and a fresh row's rest state is exactly what it should re-derive from.
  TEST_METHOD(AJumpClearsIntentAndTheAlert)
  {
    Game::Universe universe;
    const Game::UniversePos nearPos = Game::LocalPos(0.0f, 0.0f);
    const GatePair pair = MakeGatePair(universe, nearPos, Game::LocalPos(60000.0f, 0.0f));
    std::vector<Game::ShipId> ships = FleetAtTheGate(universe, nearPos, 2);

    // Roused, so the alert and the threat are burning when the fleet reaches the door.
    const Game::ShipId raider =
      universe.SpawnShip(Game::LocalPos(300.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    universe.RecordHostileAct(universe.HandleOf(raider), universe.HandleOf(ships[0]));

    const Game::Universe::FleetId before = universe.FleetInSlot(Game::OWNER_LOCAL, 0);
    Assert::IsTrue(universe.FleetOf(before).alertTicks > 0, L"the fleet was not roused, so this row proves nothing");

    const Game::EntityId first = universe.EntityIdOf(ships[0]);
    Assert::IsTrue(Game::Universe::FleetOrderResult::Ordered == OrderJump(universe, pair.nearStructure), L"the jump order was refused");
    universe.Step();

    const Game::Universe::FleetId id = universe.FleetInSlot(Game::OWNER_LOCAL, 0);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, id, L"the fleet did not survive its own jump");
    const Game::Universe::Fleet& fleet = universe.FleetOf(id);
    Assert::IsTrue(Game::FleetOrderKind::Idle == fleet.orderKind, L"the jump order outlived the jump");
    Assert::AreEqual(static_cast<std::uint32_t>(0), fleet.alertTicks, L"the alert crossed the gate: a leash a system away never releases");
    Assert::AreEqual(Game::INVALID_SHIP_ID, universe.Resolve(fleet.threat), L"the threat crossed the gate");

    const Game::ShipId arrived = universe.ResolveEntity(first);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, arrived, L"the ship did not arrive");
    Assert::IsTrue(universe.RouteOf(arrived).empty(), L"a route crossed the gate");
    Assert::IsFalse(universe.PatrolOf(arrived).active, L"a patrol crossed the gate");
    Assert::IsFalse(universe.DockingOf(arrived).active, L"a docking intent crossed the gate");
    Assert::IsFalse(universe.ProtectorOf(arrived).active, L"a protector duty crossed the gate");
  }

  // The one failure this pass must not have. A gate that leads nowhere holds the fleet; it does not
  // eat it (Design/Archive/Universe-slice-2.md 4.6).
  TEST_METHOD(AJumpThroughAStaleGateStrandsNobody)
  {
    Game::Universe universe;
    const Game::UniversePos nearPos = Game::LocalPos(0.0f, 0.0f);
    const GatePair pair = MakeGatePair(universe, nearPos, Game::LocalPos(60000.0f, 0.0f));
    std::vector<Game::ShipId> ships = FleetAtTheGate(universe, nearPos, 2);

    std::vector<Game::EntityId> before;
    for (const Game::ShipId ship : ships)
      before.push_back(universe.EntityIdOf(ship));

    // The far side is gone. Its row stays, so the near gate still points at an identity that no
    // longer names anything.
    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(pair.farStructure)), L"the far structure did not despawn");

    Assert::IsTrue(Game::Universe::FleetOrderResult::Ordered == OrderJump(universe, pair.nearStructure), L"the jump order was refused");
    for (int tick = 0; tick < 30; ++tick)
      universe.Step();

    for (const Game::EntityId entity : before)
    {
      const Game::ShipId now = universe.ResolveEntity(entity);
      Assert::AreNotEqual(Game::INVALID_SHIP_ID, now, L"a fleet was lost into a gate that leads nowhere");
      Assert::IsTrue(Game::Distance(universe.Ship(now).posUniverse, nearPos) < 2000.0f, L"a ship left through a gate with no far side");
    }
    for (const Game::DespawnRecord& gone : universe.DespawnsSince(0))
      Assert::IsFalse(gone.cause == Game::DespawnCause::JumpedOut, L"something jumped through a gate that leads nowhere");
  }

  // A gate whose own structure died stands the fleet down where it is, rather than leaving an order
  // that resolves to nothing standing forever.
  TEST_METHOD(ADeadGateStandsTheFleetDown)
  {
    Game::Universe universe;
    const Game::UniversePos nearPos = Game::LocalPos(0.0f, 0.0f);
    const GatePair pair = MakeGatePair(universe, nearPos, Game::LocalPos(60000.0f, 0.0f));
    (void)FleetAtTheGate(universe, nearPos, 2);

    Assert::IsTrue(Game::Universe::FleetOrderResult::Ordered == OrderJump(universe, pair.nearStructure), L"the jump order was refused");
    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(pair.nearStructure)), L"the near structure did not despawn");
    universe.Step();

    const Game::Universe::FleetId id = universe.FleetInSlot(Game::OWNER_LOCAL, 0);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, id, L"the fleet was lost with its gate");
    Assert::IsTrue(Game::FleetOrderKind::Idle == universe.FleetOf(id).orderKind, L"an order naming a dead gate is still standing");
  }
};
} // namespace GameLogicTests
