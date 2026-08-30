#pragma once

#include "ShipState.h"
#include "SimTuning.h"
#include "WorldPos.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Game
{
class World;

// What one subscriber can see, and what changed since it last looked.
//
// Design/Archive/Collision.md 1 names this as the spatial index's second customer and is careful about what
// it costs: QueryCircle, the cell decomposition and the static/dynamic split are reused unchanged,
// while the subscription set, the enter/leave delta and the priority accumulator are new. This is
// that new part, and nothing in SpatialIndex changed to accommodate it.
//
// The problem it solves is that a snapshot carrying every entity costs O(N) per subscriber and so
// O(N^2) across them. Carrying only what a subscriber can see costs O(k), where k is the population
// of one neighbourhood, and stops growing once the world is larger than the view.
//
// Nothing here is in the replay contract. It changes what is sent, never what is simulated.
class InterestSet
{
public:
  struct Desc
  {
    float radiusMetres = INTEREST_RADIUS_METRES;
    std::uint32_t updateEveryTicks = INTEREST_UPDATE_EVERY_TICKS;
    float minWeight = INTEREST_MIN_WEIGHT;
  };

  void Configure(const Desc& _desc) noexcept;

  // True on the ticks an update is due. The rate is counted in ticks rather than seconds so that a
  // measurement reproduces and a test can assert "ten updates in sixty ticks" exactly.
  //
  // _phase is which tick within the period this subscriber is due on, and it exists so that N of
  // them do not all come due together: without it every subscriber's query, sort and egress lands on
  // the same tick, and the server's worst frame is the period times its average one
  // (Design/MmoScalabilityReview.md E4). Zero is the old behavior and the right default for the one
  // subscriber that has no siblings to avoid.
  [[nodiscard]] bool IsDueOn(std::uint64_t _tick, std::uint32_t _phase = 0) const noexcept;

  // Recomputes the set around _centre and works out what changed. Call only on a due tick.
  void Update(const World& _world, const WorldPos& _centre);

  // Handles seen for the first time this update. Sent in full: the subscriber has never had them.
  [[nodiscard]] std::span<const ShipHandle> Entered() const noexcept
  {
    return m_entered;
  }

  // Handles that dropped out. Sent bare: the subscriber drops them.
  //
  // A ship leaving the radius and a ship dying arrive identically, and the subscriber must not try
  // to tell them apart -- coming back is a leave followed by an enter
  // (Design/Archive/Collision-slice-6.md 6.2).
  [[nodiscard]] std::span<const ShipHandle> Left() const noexcept
  {
    return m_left;
  }

  // Handles already known whose priority came due. Near entities appear here every update, far ones
  // once in every 1 / minWeight.
  [[nodiscard]] std::span<const ShipHandle> Refreshed() const noexcept
  {
    return m_refreshed;
  }

  // Everything currently subscribed, sorted. Diagnostics and tests.
  [[nodiscard]] std::span<const ShipHandle> Subscribed() const noexcept
  {
    return m_subscribed;
  }

private:
  Desc m_desc;

  // Sorted by (slot, generation), which is a total order, so the sets do not depend on the order
  // QueryCircle happened to return. A hash map would be the obvious structure here and is the one
  // thing this class must not use: AGENTS.md 5 bans iteration order that is not dense-array order,
  // and a map would put hashing into the answer (Design/Decisions/0010).
  std::vector<ShipHandle> m_subscribed;
  std::vector<float> m_priority; // parallel to m_subscribed

  std::vector<ShipHandle> m_entered;
  std::vector<ShipHandle> m_left;
  std::vector<ShipHandle> m_refreshed;

  // Scratch, reused so an update allocates nothing after the first.
  std::vector<ShipId> m_queryScratch;
  std::vector<ShipHandle> m_currentScratch;
  std::vector<float> m_distanceScratch; // parallel to m_currentScratch
  std::vector<ShipHandle> m_mergedScratch;
  std::vector<float> m_mergedPriority;
};

// The total order the sets are kept in. A ShipHandle is a slot and a generation, and neither alone
// is unique across a despawn, so both are compared.
[[nodiscard]] inline bool HandleOrderBefore(ShipHandle _a, ShipHandle _b) noexcept
{
  return (_a.slot != _b.slot) ? (_a.slot < _b.slot) : (_a.generation < _b.generation);
}
} // namespace Game
