#pragma once

#include "InterestSet.h"
#include "ShipState.h"
#include "WorldPos.h"
#include "WorldSnapshot.h"

#include "Transport.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Game
{
class World;

// The server's side of the seam, for N subscribers rather than one.
//
// It lives in GameLogic, and ADR 0008's three-way elimination is why -- re-run rather than cited,
// because the answer is not obvious and the record's own reasoning is what settles it. NeuronServer
// never names GameLogic, so it cannot hold an InterestSet. Outpost could, and does today, but then
// the executable owns the fan-out and the day there are two executables it is in the wrong one.
// GameLogic owns both types being tabled and already depends on NeuronCore, so it may include
// Transport.h. Same test AGENTS.md 2 applies to content readers: the code lives with what it is
// about (ADR 0030).
//
// What this class is NOT: it is not a session layer. It does not know who a subscriber is, how it
// authenticated, or when it should go away -- a subscriber is a transport and a faction that
// somebody hands in. Deciding that is the composition root's, and for a dedicated server it needs a
// configuration story that does not exist yet (MmoScalabilityPlan.md 4, decision 3).
//
// Everything here is outside the replay contract. It changes what is sent, never what is simulated,
// which is what lets a server retune a radius or a phase mid-match and still replay the recording.
class Publisher
{
public:
  // A stable reference to one subscriber. Slot plus generation for the same reason ShipHandle is
  // (ADR 0005): removal swaps the last entry down, so a bare index would silently retarget.
  struct Handle
  {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0; // 0 is never issued, so a default handle is null
  };

  struct Desc
  {
    // The wire to this subscriber. The publisher does not own it and it must outlive the entry.
    Neuron::Transport* transport = nullptr;

    // Whose orders this subscriber may give. The simulation gates the order itself (ADR 0014); this
    // is what the publisher passes it, not a second authority check.
    FactionId faction = FACTION_PLAYER;

    // Its own, so a spectator or a distant region can be given a different one without touching
    // anybody else's. Defaults to the tuning header's.
    InterestSet::Desc interest;

    // The ceiling on orders read from this subscriber in one tick. A client saturating its send rate
    // otherwise converts wire bytes into formation solves and route planning at a leverage no other
    // message has (Design/Archive/MmoScalabilityReview.md E6).
    std::uint32_t ordersPerTick = 8;

    // Where it is looking, until SetCentre says otherwise.
    WorldPos centre;

    // Where in the world's despawn log this subscriber starts reading. A subscriber joining a
    // running world passes World::DespawnHead(), so it is told about deaths from now on and about
    // none of the ships it never held; zero means "everything the log still holds" and is right only
    // for a subscriber present from the first tick (ADR 0027).
    //
    // It is a field rather than something Add works out for itself, because Add has no World and
    // giving it one to read a single number would be a dependency for a default.
    std::uint64_t openingDespawnCursor = 0;

    // Where in the world's shot log this subscriber starts reading, on the same terms and for the
    // same reason: a subscriber joining a running world passes World::ShotHead() and is told about
    // gunfire from now on rather than about a battle it did not watch (ADR 0027, ADR 0053).
    std::uint64_t openingShotCursor = 0;
  };

  // Adds a subscriber and returns its handle. Its phase is assigned here, from the slot it takes, so
  // that consecutive subscribers land on consecutive ticks and nothing has to be told to spread.
  Handle Add(const Desc& _desc);

  // Removes one. False if the handle was already stale. Its cursor stops holding the despawn log
  // back on the next Publish.
  bool Remove(Handle _handle);

  void SetCentre(Handle _handle, const WorldPos& _centre) noexcept;

  [[nodiscard]] std::uint32_t Count() const noexcept
  {
    return static_cast<std::uint32_t>(m_subscribers.size());
  }

  // Reads every subscriber's orders, under its budget, and applies them to the world. Call once per
  // tick, before the world steps: an order that arrives this frame is meant for this tick.
  void ApplyOrders(World& _world);

  // Sends every subscriber that is due its update, then trims the despawn log to the minimum cursor
  // across all of them. Call once per tick, after the world steps.
  //
  // Non-const because the trim is a write. Nothing else here touches the world, and a Publish that
  // took a const reference and cast it away would be hiding exactly the mutation that matters.
  void Publish(World& _world);

  // Diagnostics, per subscriber. Both should be zero and are worth watching if they are not.
  //
  // Throttled ticks are ticks on which this subscriber still had orders waiting when its budget ran
  // out. Nothing was thrown away -- what is over budget stays in the transport's queue and is read
  // next tick -- so this counts the throttling, not a loss. A subscriber that throttles every tick
  // is either misbehaving or under-budgeted; one that keeps it up long enough fills its own queue,
  // and the transport's own backpressure is what drops from there.
  [[nodiscard]] std::uint32_t ThrottledTickCount(Handle _handle) const noexcept;
  [[nodiscard]] std::uint32_t RefusedLeaveCount(Handle _handle) const noexcept;

  // Which tick within the update period a subscriber is due on. Exposed so a test can assert the
  // spread rather than infer it.
  [[nodiscard]] std::uint32_t PhaseOf(Handle _handle) const noexcept;

private:
  struct Subscriber
  {
    Neuron::Transport* transport = nullptr;
    FactionId faction = FACTION_PLAYER;
    std::uint32_t ordersPerTick = 8;
    std::uint32_t phase = 0;
    std::uint64_t despawnCursor = 0;
    std::uint64_t shotCursor = 0;
    std::uint32_t throttledTicks = 0;
    WorldPos centre;
    InterestSet interest;
    SnapshotWriter writer;

    // What this subscriber was last told is in each of its faction's slots, so Publish can diff
    // rather than be told when a roster changed.
    //
    // Here rather than in World, and that is the design of it: the diff is per subscriber, changes
    // nothing that is simulated, is outside the replay contract and is not saved. It also makes
    // join-time delivery free -- a new subscriber's lists are empty, so its first Publish finds
    // every occupied slot changed and sends the lot, which is the despawn cursor's own joining rule
    // arriving at fleets with no second mechanism (Design/Archive/Fleets.md 8.1, ADR 0027).
    //
    // Membership is the whole of what is diffed; occupancy is not tracked here because it does not
    // have to be. A slot that empties always passes through a roster change on its way out -- the
    // prune drops the last member before the retire drops the row -- and a slot that fills starts
    // empty, which is what the client already holds. Occupancy is the status block's mask.
    std::vector<EntityId> lastRoster[FLEET_SLOTS];
  };

  struct Slot
  {
    std::uint32_t subscriber = INVALID_SUBSCRIBER;
    std::uint32_t generation = 0;
  };

  static constexpr std::uint32_t INVALID_SUBSCRIBER = 0xFFFFFFFFu;

  [[nodiscard]] Subscriber* Resolve(Handle _handle) noexcept;
  [[nodiscard]] const Subscriber* Resolve(Handle _handle) const noexcept;

  // One subscriber's update: the interest walk, the split of what left from what died, and the two
  // messages. Everything WorldSimulation used to do for its single subscriber, per entry.
  void PublishOne(const World& _world, Subscriber& _subscriber);

  // States any of _subscriber's five slots whose membership has changed since it was last told.
  //
  // Every tick, not only on the ticks the subscriber is due: membership changes at human speed, so
  // a diff over five slots of at most eight ids is nothing, and a roster held back for a phase
  // would arrive after the status block that describes it.
  void PublishRosters(const World& _world, Subscriber& _subscriber);

  // Splits _subscriber's Left() three ways: the deaths, the dockings, and the ships that merely flew
  // out of range. The world's despawn log intersected with what this subscriber was holding -- so a
  // departure nobody could see is told to nobody, and a ship that left the radius is reported
  // neither dead nor docked (Design/Archive/Hostiles.md 4.4, ADR 0040).
  void SplitTheLost(const World& _world, Subscriber& _subscriber);

  // Dense, because iteration order is array order and nothing here may depend on a pointer or a
  // hash (AGENTS.md 5). Removal swap-and-pops and the slot table repairs the handles, exactly as
  // World does it for ships.
  std::vector<Subscriber> m_subscribers;
  std::vector<std::uint32_t> m_subscriberSlot; // parallel to m_subscribers
  std::vector<Slot> m_slots;
  std::vector<std::uint32_t> m_freeSlots;

  // Scratch, reused so a tick allocates nothing once the subscriber list has stopped growing.
  // Handles on the way in, ids on the way out: the interest set deals in references into this world
  // and the wire deals in identities, and SplitTheLost is where the two meet (ADR 0047).
  std::vector<ShipHandle> m_sendScratch;
  std::vector<ShipHandle> m_departedScratch; // sorted handles of everything the log said left, any cause
  std::vector<ShotRecord> m_fireScratch;
  std::vector<EntityId> m_leftScratch;
  std::vector<EntityId> m_destroyedScratch;
  std::vector<EntityId> m_dockedScratch;
  std::vector<ShipId> m_resolvedScratch;
  std::vector<std::uint8_t> m_messageScratch;
  FleetRoster m_rosterScratch; // one roster at a time, its vector kept so a quiet tick allocates nothing
};
} // namespace Game
