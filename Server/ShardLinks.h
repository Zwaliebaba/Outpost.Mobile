#pragma once

#include "ShardLink.h"
#include "Universe.h"

#include "Transport.h"

#include <cstdint>
#include <vector>

namespace Shard
{
// Every link this shard has: one per neighbouring shard, and the transport each of them talks over.
//
// **The neighbour set is a function of the save, not of a list somebody maintains** (ADR 0063, and
// `Design/ShardServer-slice-3.md` §2.1). It comes out of the universe's own gates, so a shard cannot
// disagree with its own file about who it borders -- which a shard that re-derived the partition from
// a seed and a `GalaxyDesc` could, the day any of those drifted.
//
// Built once at boot. A shard's gates do not change while it runs, so neither does this; the day a
// gate can be built or destroyed, that sentence is the one that stops being true.
class ShardLinks
{
public:
  // Reads the universe's gates and makes one link per shard beyond them. Returns how many.
  std::uint32_t Open(const Game::Universe& _universe)
  {
    m_links.clear();
    m_peerScratch.resize(_universe.NeighbourShards({}));
    const std::uint32_t count = _universe.NeighbourShards(m_peerScratch);
    m_links.reserve(count);
    for (const Game::ShardId peer : m_peerScratch)
    {
      // Constructed in place and then named, rather than a designated initialiser: a ShardLink holds
      // its own scratch and is not something to spell out in a braced list.
      m_links.emplace_back();
      m_links.back().peer = peer;
    }
    return count;
  }

  [[nodiscard]] std::uint32_t Count() const noexcept
  {
    return static_cast<std::uint32_t>(m_links.size());
  }

  [[nodiscard]] Game::ShardId PeerAt(std::uint32_t _at) const noexcept
  {
    return (_at < m_links.size()) ? m_links[_at].peer : Game::ShardId{0};
  }

  // Gives one link somewhere to send. **Slice 4 is what calls this with a real transport**; until
  // then every link has none, and a link with no transport does nothing -- which is not an error and
  // is why this slice can land without an address anywhere in it. False if no link has that peer,
  // which is a deployment pointed at a shard this one does not border.
  bool Attach(Game::ShardId _peer, Neuron::Transport* _transport) noexcept
  {
    for (Link& link : m_links)
    {
      if (link.peer != _peer)
        continue;
      link.transport = _transport;
      return true;
    }
    return false;
  }

  [[nodiscard]] const Neuron::Transport* TransportFor(Game::ShardId _peer) const noexcept
  {
    for (const Link& link : m_links)
    {
      if (link.peer == _peer)
        return link.transport;
    }
    return nullptr;
  }

  // Once per pass, and each link is told which shard is on its far end -- because the outbox is one
  // queue for every destination and a link must carry only what is for its own peer
  // (Design/ShardServer-slice-3.md §8).
  void Pump(Game::Universe& _universe)
  {
    for (Link& link : m_links)
    {
      if (link.transport != nullptr)
        (void)link.link.Pump(_universe, *link.transport, link.peer);
    }
  }

  // **Called after a save that HAPPENED, and after no other kind.** That is the whole of ADR 0066:
  // an acknowledgement asserts the entry is in this shard's file, so a save that was refused makes
  // nothing durable and a link told otherwise would ack a fleet into a file that does not contain
  // it. The call site is `ShardApp::SaveUniverse`, past the refusal's early return.
  void NoteDurableThrough(std::uint64_t _tick) noexcept
  {
    for (Link& link : m_links)
      link.link.NoteDurableThrough(_tick);
  }

  // What every link has been told, for a caller that wants to check rather than infer it. Zero when
  // there are no links, which is a shard with no neighbours and not a shard that never saved.
  [[nodiscard]] std::uint64_t DurableTick() const noexcept
  {
    return m_links.empty() ? 0 : m_links.front().link.DurableTick();
  }

private:
  struct Link
  {
    Game::ShardId peer = 0;
    Neuron::Transport* transport = nullptr; // not owned; slice 4 supplies it, and it must outlive this
    Game::ShardLink link;
  };

  std::vector<Link> m_links;
  std::vector<Game::ShardId> m_peerScratch; // sized once by Open and not touched again
};
} // namespace Shard
