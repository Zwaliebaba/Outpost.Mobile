#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// Its own copy rather than a shared one: every suite in this folder keeps its helpers in an
// anonymous namespace, which is what the tree does instead of a test-support header nobody owns.
Game::ShipId SpawnAt(Game::World& _world, float _x, float _z, Game::HullId _hull = Game::HullId::Corvette,
                     Game::FactionId _faction = Game::FACTION_PLAYER)
{
  return _world.SpawnShip(Game::LocalPos(_x, _z), 0.0f, static_cast<std::uint32_t>(_hull), _faction);
}

class CaptureLink final : public Neuron::Transport
{
public:
  [[nodiscard]] bool Send(const std::uint8_t* _bytes, std::uint32_t _count) override
  {
    if (_count > Neuron::MAX_DATAGRAM_BYTES)
      return false;
    sent.emplace_back(_bytes, _bytes + _count);
    return true;
  }

  [[nodiscard]] bool SendReliable(const std::uint8_t* _bytes, std::uint32_t _count) override
  {
    if (_count > Neuron::MAX_RELIABLE_BYTES)
      return false;
    sentReliable.emplace_back(_bytes, _bytes + _count);
    return true;
  }

  [[nodiscard]] std::uint32_t Receive(std::uint8_t*, std::uint32_t) override
  {
    return 0;
  }

  [[nodiscard]] std::uint32_t ReceiveReliable(std::uint8_t*, std::uint32_t) override
  {
    return 0;
  }

  void Poll() override {}

  [[nodiscard]] Neuron::ConnectionState State() const override
  {
    return Neuron::ConnectionState::Connected;
  }

  std::vector<std::vector<std::uint8_t>> sent;
  std::vector<std::vector<std::uint8_t>> sentReliable;
};

// Both lanes, in the order the writer used them.
void FeedBothLanes(Game::SnapshotReceiver& _receiver, const CaptureLink& _link)
{
  for (const std::vector<std::uint8_t>& message : _link.sentReliable)
    (void)_receiver.Accept(message);
  for (const std::vector<std::uint8_t>& datagram : _link.sent)
    (void)_receiver.Accept(datagram);
}

[[nodiscard]] const Game::ShipSnapshot* Find(const Game::WorldSnapshot& _snapshot, Game::EntityId _entity)
{
  for (const Game::ShipSnapshot& ship : _snapshot.ships)
  {
    if (ship.entity == _entity)
      return &ship;
  }
  return nullptr;
}

// Widened for the assertion: an EntityId prints as an integer, and a ShardId is a u16 that some
// frameworks would rather print as a character.
[[nodiscard]] std::uint32_t Shard(Game::ShardId _shard)
{
  return _shard;
}
} // namespace

TEST_CLASS(EntityIdentityTests)
{
public:
  TEST_METHOD(AnIdCarriesItsShardAndItsSerial)
  {
    // The packing, asserted rather than trusted, because everything below depends on the two halves
    // not overlapping. Serial 1 is the first a shard ever mints; shard 0 with serial 0 is the null
    // id and is what the top of the range must not alias.
    Assert::IsTrue(Game::MakeEntityId(0, 0) == Game::INVALID_ENTITY_ID, L"shard 0 serial 0 is not the null id");
    Assert::IsTrue(Game::MakeEntityId(7, 1) != Game::INVALID_ENTITY_ID, L"a real id came out null");

    const Game::EntityId top = Game::MakeEntityId(0xFFFFu, Game::ENTITY_SERIAL_MASK);
    Assert::AreEqual(Shard(0xFFFFu), Shard(Game::EntityShardOf(top)), L"the shard did not survive packing");
    Assert::IsTrue(Game::EntitySerialOf(top) == Game::ENTITY_SERIAL_MASK, L"the serial did not survive packing");

    // A serial that overflows its field must not spill into the shard, or two shards would issue
    // ids that collide.
    const Game::EntityId spilled = Game::MakeEntityId(3, Game::ENTITY_SERIAL_MASK + 1);
    Assert::AreEqual(Shard(3), Shard(Game::EntityShardOf(spilled)), L"an oversized serial changed the shard");
  }

  TEST_METHOD(EveryShipGetsItsOwnIdAndNoneIsReused)
  {
    Game::World world;
    world.ConfigureShard(4);

    const Game::ShipId first = SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipId second = SpawnAt(world, 100.0f, 0.0f);
    const Game::EntityId firstEntity = world.EntityIdOf(first);
    const Game::EntityId secondEntity = world.EntityIdOf(second);

    Assert::IsTrue(firstEntity != secondEntity, L"two ships share an id");
    Assert::AreEqual(Shard(4), Shard(Game::EntityShardOf(firstEntity)), L"the configured shard is not in the id");
    Assert::AreEqual(Shard(4), Shard(Game::EntityShardOf(secondEntity)), L"the configured shard is not in the id");

    // A slot is reused; an id is not. This is the whole difference between a reference and an
    // identity, and it is what a 48-bit serial buys over a generation that wraps (ADR 0047).
    const Game::ShipHandle firstHandle = world.HandleOf(first);
    Assert::IsTrue(world.DespawnShip(firstHandle), L"the despawn failed");
    const Game::ShipId third = SpawnAt(world, 200.0f, 0.0f);
    Assert::AreEqual(firstHandle.slot, world.HandleOf(third).slot, L"the slot was not reused, so this proves nothing");

    const Game::EntityId thirdEntity = world.EntityIdOf(third);
    Assert::IsTrue(thirdEntity != firstEntity, L"a reused slot reissued the dead ship's id");
    Assert::AreEqual(Game::INVALID_SHIP_ID, world.ResolveEntity(firstEntity), L"a dead entity still resolves");
    Assert::IsTrue(world.ResolveEntity(thirdEntity) == third, L"the new entity does not resolve to the new ship");
  }

  TEST_METHOD(AHandleAndAnIdAnswerDifferentQuestions)
  {
    // A despawned handle names nothing and a despawned id names nothing, which is the same answer
    // for two different reasons: the handle's generation moved on, and the id was retired.
    Game::World world;
    const Game::ShipId ship = SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipHandle handle = world.HandleOf(ship);
    const Game::EntityId entity = world.EntityIdOf(ship);

    Assert::IsTrue(world.EntityIdOf(handle) == entity, L"a handle and its ship disagree about the id");
    Assert::IsTrue(world.HandleOfEntity(entity) == handle, L"an id and its ship disagree about the handle");

    Assert::IsTrue(world.DespawnShip(handle), L"the despawn failed");
    Assert::AreEqual(Game::INVALID_SHIP_ID, world.Resolve(handle), L"a dead handle still resolves");
    Assert::AreEqual(Game::INVALID_SHIP_ID, world.ResolveEntity(entity), L"a dead id still resolves");
    Assert::IsTrue(world.EntityIdOf(handle) == Game::INVALID_ENTITY_ID, L"a dead handle still names an entity");
    Assert::IsTrue(world.HandleOfEntity(entity) == Game::ShipHandle{}, L"a dead id still names a handle");
  }

  TEST_METHOD(TheSameEntityInTwoWorldsHasTwoHandlesAndOneId)
  {
    // The sentence this whole slice exists to make true (Design/Archive/EntityIdentity-work-order.md 6).
    Game::World alpha;
    alpha.ConfigureShard(1);
    Game::World beta;
    beta.ConfigureShard(2);

    // Beta is given some ships of its own first, so that the handed-over entity cannot land in the
    // same slot by accident and pass this test for the wrong reason.
    (void)SpawnAt(beta, -500.0f, 0.0f);
    (void)SpawnAt(beta, -400.0f, 0.0f);
    (void)SpawnAt(beta, -300.0f, 0.0f);

    const Game::ShipId inAlpha = SpawnAt(alpha, 60.0f, -20.0f, Game::HullId::Frigate, Game::FACTION_VANGUARD);
    const Game::EntityId entity = alpha.EntityIdOf(inAlpha);
    const Game::ShipHandle alphaHandle = alpha.HandleOf(inAlpha);

    const Game::ShipId inBeta = beta.SpawnShipAs(entity, Game::LocalPos(60.0f, -20.0f), 0.0f,
                                                 static_cast<std::uint32_t>(Game::HullId::Frigate), Game::FACTION_VANGUARD);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, inBeta, L"the handoff was refused");

    const Game::ShipHandle betaHandle = beta.HandleOf(inBeta);
    Assert::IsTrue(beta.EntityIdOf(inBeta) == entity, L"the entity did not keep its id across worlds");
    Assert::IsFalse(alphaHandle == betaHandle, L"the two worlds happened to issue the same handle, so this proves nothing");
    Assert::AreEqual(Shard(1), Shard(Game::EntityShardOf(beta.EntityIdOf(inBeta))), L"the id was reminted under the receiving shard");
  }

  TEST_METHOD(AHandoverIsNotADeathAndAnEnter)
  {
    // The failure U3 names: a client keyed on handles sees the handle it held disappear and an
    // unfamiliar one arrive. Keyed on ids it sees one record, updated in place -- which is what
    // "same ship, new region" has to look like on the wire.
    Game::World alpha;
    alpha.ConfigureShard(1);
    Game::World beta;
    beta.ConfigureShard(2);
    (void)SpawnAt(beta, -900.0f, 0.0f); // so beta's slots do not line up with alpha's

    const Game::ShipId inAlpha = SpawnAt(alpha, 0.0f, 0.0f, Game::HullId::Interceptor);
    const Game::EntityId entity = alpha.EntityIdOf(inAlpha);
    alpha.Step();

    // The client is told about it by the first world.
    Game::SnapshotReceiver client;
    {
      CaptureLink link;
      Game::SnapshotWriter writer;
      const Game::ShipHandle sent[] = {alpha.HandleOf(inAlpha)};
      Assert::IsTrue(writer.WriteInterest(alpha, sent, {}, {}, {}, link) > 0, L"the first update did not send");
      FeedBothLanes(client, link);
    }
    Assert::AreEqual(static_cast<std::size_t>(1), client.Latest().ships.size(), L"the client did not take the ship");
    Assert::IsTrue(Find(client.Latest(), entity) != nullptr, L"the client did not hold the entity");

    // It moves: gone from alpha, alive in beta under the same identity. The departure is deliberately
    // NOT sent -- a handover is not a departure, and the point is that the client needs no message
    // to know the two records are one ship.
    Assert::IsTrue(alpha.DespawnShip(alpha.HandleOf(inAlpha)), L"the despawn failed");
    const Game::ShipId inBeta =
      beta.SpawnShipAs(entity, Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor));
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, inBeta, L"the handoff was refused");
    for (int tick = 0; tick < 4; ++tick)
      beta.Step();

    {
      CaptureLink link;
      Game::SnapshotWriter writer;
      const Game::ShipHandle sent[] = {beta.HandleOf(inBeta)};
      Assert::IsTrue(writer.WriteInterest(beta, sent, {}, {}, {}, link) > 0, L"the second update did not send");
      FeedBothLanes(client, link);
    }

    Assert::AreEqual(static_cast<std::size_t>(1), client.Latest().ships.size(), L"the handover appended a second record");
    Assert::IsTrue(Find(client.Latest(), entity) != nullptr, L"the entity is not the one the client holds");
    Assert::IsTrue(client.Destroyed().empty(), L"a handover was reported as a death");
    Assert::IsTrue(client.Docked().empty(), L"a handover was reported as a docking");
  }

  TEST_METHOD(AForeignIdResolvesOnlyWhereItWasHandedIn)
  {
    Game::World here;
    here.ConfigureShard(9);
    Game::World elsewhere;
    elsewhere.ConfigureShard(9); // the same shard number, to prove the answer is not "not mine"

    const Game::EntityId foreign = Game::MakeEntityId(77, 12345);
    const Game::ShipId landed =
      here.SpawnShipAs(foreign, Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, landed, L"a foreign id was refused");
    Assert::IsTrue(here.ResolveEntity(foreign) == landed, L"a foreign id did not resolve where it was handed in");
    Assert::AreEqual(Game::INVALID_SHIP_ID, elsewhere.ResolveEntity(foreign), L"a foreign id resolved in a world that never saw it");

    // And it stays foreign: a world does not adopt an id into its own numbering.
    Assert::AreEqual(Shard(77), Shard(Game::EntityShardOf(here.EntityIdOf(landed))), L"the receiving world reminted the id");

    // The same id twice in one world is refused rather than resolved to whichever copy is found
    // first, because an entity existing twice is what identity exists to make impossible.
    Assert::AreEqual(Game::INVALID_SHIP_ID,
                     here.SpawnShipAs(foreign, Game::LocalPos(50.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette)),
                     L"the same entity was admitted twice");
    Assert::AreEqual(
      Game::INVALID_SHIP_ID,
      here.SpawnShipAs(Game::INVALID_ENTITY_ID, Game::LocalPos(50.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette)),
      L"the null id was admitted");
  }

  TEST_METHOD(ALocalIdHandedInMovesTheCounterPastItself)
  {
    // What stops a reloaded world minting an id its own file already used. A foreign id says nothing
    // about this shard's numbering and must move the counter not at all.
    Game::World world;
    world.ConfigureShard(5);

    Assert::AreNotEqual(
      Game::INVALID_SHIP_ID,
      world.SpawnShipAs(Game::MakeEntityId(5, 900), Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette)),
      L"a local id was refused");
    const Game::ShipId next = SpawnAt(world, 100.0f, 0.0f);
    Assert::IsTrue(Game::EntitySerialOf(world.EntityIdOf(next)) > 900, L"the counter did not move past a local id handed in");

    Assert::AreNotEqual(Game::INVALID_SHIP_ID,
                        world.SpawnShipAs(Game::MakeEntityId(6, 100000), Game::LocalPos(200.0f, 0.0f), 0.0f,
                                          static_cast<std::uint32_t>(Game::HullId::Corvette)),
                        L"a foreign id was refused");
    const Game::ShipId after = SpawnAt(world, 300.0f, 0.0f);
    Assert::IsTrue(Game::EntitySerialOf(world.EntityIdOf(after)) < 100000, L"a foreign id moved this shard's counter");
  }

  TEST_METHOD(TheIndexSurvivesEveryOrderOfSpawnAndDespawn)
  {
    // The reverse index is a sorted vector, so the case that breaks it is an insertion that is not an
    // append: a foreign id below every local one, then a despawn from the middle. Walked
    // exhaustively over a small table rather than sampled, because an off-by-one in a lower_bound
    // shows up as one entity resolving to another and nothing else.
    for (int order = 0; order < 6; ++order)
    {
      Game::World world;
      world.ConfigureShard(2);

      std::vector<Game::EntityId> entities;
      std::vector<Game::ShipId> ships;
      for (int at = 0; at < 6; ++at)
      {
        // Alternating: local ids ascend and append, foreign ones land wherever they sort.
        const Game::ShipId id = (at % 2 == 0) ? SpawnAt(world, static_cast<float>(at) * 60.0f, 0.0f)
                                              : world.SpawnShipAs(Game::MakeEntityId(1, static_cast<std::uint64_t>(20 - at)),
                                                                  Game::LocalPos(static_cast<float>(at) * 60.0f, 0.0f), 0.0f,
                                                                  static_cast<std::uint32_t>(Game::HullId::Corvette));
        Assert::AreNotEqual(Game::INVALID_SHIP_ID, id, L"a spawn was refused");
        ships.push_back(id);
        entities.push_back(world.EntityIdOf(id));
      }

      // Retire one, chosen by the loop, and then check that every survivor still names itself.
      const Game::EntityId retired = entities[static_cast<std::size_t>(order)];
      Assert::IsTrue(world.DespawnShip(world.HandleOfEntity(retired)), L"the despawn failed");
      Assert::AreEqual(Game::INVALID_SHIP_ID, world.ResolveEntity(retired), L"a retired entity still resolves");

      for (const Game::EntityId entity : entities)
      {
        if (entity == retired)
          continue;
        const Game::ShipId resolved = world.ResolveEntity(entity);
        Assert::AreNotEqual(Game::INVALID_SHIP_ID, resolved, L"a survivor stopped resolving after a despawn");
        Assert::IsTrue(world.EntityIdOf(resolved) == entity, L"an entity resolved to a different ship");
      }
    }
  }

  TEST_METHOD(AnOrderNamesTheShipItsIdNames)
  {
    // The publisher's resolve loop, end to end: a client that only ever saw ids orders by id, and an
    // id minted somewhere else buys nothing -- which is what stops a client ordering a ship this
    // world does not own.
    Game::World world;
    world.ConfigureShard(3);
    const Game::ShipId first = SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipId second = SpawnAt(world, 80.0f, 0.0f);
    world.Step();

    Game::MoveOrder sent;
    sent.ships = {world.EntityIdOf(first), Game::MakeEntityId(3, 999999), world.EntityIdOf(second)};
    sent.destination = Game::LocalPos(0.0f, 900.0f);

    CaptureLink link;
    Assert::IsTrue(Game::WriteMoveOrder(sent, link), L"the order did not send");

    Game::MoveOrder got;
    Assert::IsTrue(Game::ReadMoveOrder(link.sentReliable[0], got), L"the order did not decode");
    Assert::AreEqual(sent.ships.size(), got.ships.size(), L"the id list changed size on the wire");
    for (std::size_t at = 0; at < sent.ships.size(); ++at)
      Assert::IsTrue(got.ships[at] == sent.ships[at], L"an id did not survive the wire");

    Assert::IsTrue(world.ResolveEntity(got.ships[0]) == first, L"the first id named the wrong ship");
    Assert::AreEqual(Game::INVALID_SHIP_ID, world.ResolveEntity(got.ships[1]), L"an id nobody minted resolved");
    Assert::IsTrue(world.ResolveEntity(got.ships[2]) == second, L"the second id named the wrong ship");
  }
};
} // namespace GameLogicTests
