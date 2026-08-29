#include "pch.h"
#include "InterestSet.h"

#include "World.h"

#include <algorithm>
#include <cmath>

namespace Game
{
void InterestSet::Configure(const Desc& _desc) noexcept
{
  m_desc.radiusMetres = (_desc.radiusMetres > 0.0f) ? _desc.radiusMetres : INTEREST_RADIUS_METRES;
  m_desc.updateEveryTicks = (_desc.updateEveryTicks > 0) ? _desc.updateEveryTicks : INTEREST_UPDATE_EVERY_TICKS;
  m_desc.minWeight = std::clamp(_desc.minWeight, 1.0f / 64.0f, 1.0f);
}

bool InterestSet::IsDueOn(std::uint64_t _tick) const noexcept
{
  return (_tick % m_desc.updateEveryTicks) == 0;
}

void InterestSet::Update(const World& _world, const WorldPos& _centre)
{
  m_entered.clear();
  m_left.clear();
  m_refreshed.clear();

  // The set as it stands now, sorted into the total order so that nothing downstream depends on the
  // order QueryCircle walked its cells in.
  _world.Index().QueryCircle(_centre, m_desc.radiusMetres, m_queryScratch);

  m_currentScratch.clear();
  m_distanceScratch.clear();
  for (const ShipId id : m_queryScratch)
  {
    if (id >= _world.ShipCount())
      continue;
    m_currentScratch.push_back(_world.HandleOf(id));
    m_distanceScratch.push_back(Distance(_centre, _world.Ship(id).posWorld));
  }

  // Sort the two arrays together. Small and bounded by the radius, so an index permutation costs
  // more than it saves; a selection over the parallel arrays keeps them in step.
  for (std::size_t at = 1; at < m_currentScratch.size(); ++at)
  {
    const ShipHandle handle = m_currentScratch[at];
    const float distance = m_distanceScratch[at];
    std::size_t back = at;
    while (back > 0 && HandleOrderBefore(handle, m_currentScratch[back - 1]))
    {
      m_currentScratch[back] = m_currentScratch[back - 1];
      m_distanceScratch[back] = m_distanceScratch[back - 1];
      --back;
    }
    m_currentScratch[back] = handle;
    m_distanceScratch[back] = distance;
  }

  // Walk the old and new sets together. One pass gives entered, left, and the carried priority of
  // everything that stayed -- which is why this is a merge rather than two set_differences.
  m_mergedScratch.clear();
  m_mergedPriority.clear();

  std::size_t oldAt = 0;
  std::size_t newAt = 0;
  while (oldAt < m_subscribed.size() || newAt < m_currentScratch.size())
  {
    const bool oldLeft = oldAt < m_subscribed.size();
    const bool newLeft = newAt < m_currentScratch.size();

    if (newLeft && (!oldLeft || HandleOrderBefore(m_currentScratch[newAt], m_subscribed[oldAt])))
    {
      // New, and entering IS the send -- it goes out in full on the update it was first seen,
      // never a turn later -- so its priority starts spent and builds from there like any other.
      m_entered.push_back(m_currentScratch[newAt]);
      m_mergedScratch.push_back(m_currentScratch[newAt]);
      m_mergedPriority.push_back(0.0f);
      ++newAt;
      continue;
    }

    if (oldLeft && (!newLeft || HandleOrderBefore(m_subscribed[oldAt], m_currentScratch[newAt])))
    {
      m_left.push_back(m_subscribed[oldAt]);
      ++oldAt;
      continue;
    }

    // In both. Accumulate priority by how near it is, and refresh when a whole update has built up.
    //
    // Subtracting one rather than zeroing is what keeps the average rate right: zeroing would round
    // every entity's rate down to the next whole number of updates, so a weight of 0.6 would refresh
    // every other update rather than three times in five.
    const float distance = m_distanceScratch[newAt];
    const float nearness = 1.0f - (distance / m_desc.radiusMetres);
    const float weight = std::clamp(nearness, m_desc.minWeight, 1.0f);

    float priority = m_priority[oldAt] + weight;
    if (priority >= 1.0f)
    {
      m_refreshed.push_back(m_subscribed[oldAt]);
      priority -= 1.0f;
    }
    m_mergedScratch.push_back(m_subscribed[oldAt]);
    m_mergedPriority.push_back(priority);
    ++oldAt;
    ++newAt;
  }

  m_subscribed.swap(m_mergedScratch);
  m_priority.swap(m_mergedPriority);
}
} // namespace Game
