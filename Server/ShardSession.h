#pragma once

#include "Publisher.h"
#include "ShipState.h"

#include "Transport.h"

namespace Shard
{
// One connection, as this server thinks of it.
//
// It owns nothing, and that is the whole shape: the listener owns the transport and recycles it, and
// the publisher owns the subscriber and can be asked to drop it. This struct is the join between the
// two, and holding it by value in a vector is safe precisely because neither field is a resource.
//
// `transport` is a KEY as much as a wire. `QuicListener::Accepted()` is the authority on which
// connections are live, and a pointer it once handed out belongs to the pool again after the Poll
// that finds its connection closed -- so a session is reconciled against that span every pass rather
// than trusted to still be there (QuicListener.h, ADR 0031).
struct ShardSession
{
  Neuron::Transport* transport = nullptr;
  Game::Publisher::Handle subscriber;

  // Whose fleets this session is told about, and whose orders it may give. There is no login, so it
  // is OWNER_LOCAL for the one session this slice serves -- and it is a field rather than a constant
  // read at the point of use so that the day a login supplies it, it is one assignment
  // (Design/ShardServer-slice-2.md 2.5).
  Game::OwnerId owner = Game::OWNER_LOCAL;
};
} // namespace Shard
