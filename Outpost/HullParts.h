#pragma once

#include "MeshData.h"

#include "DeviceSpec.h"
#include "HullSpec.h"

#include <cstdint>
#include <vector>

namespace Outpost
{
// Which parts of a hull's art turn with which mount.
//
// This is the binding `Combat.md` §10.1 named and three slices waited on: the simulation knows a
// mount has a bearing, an arc and a device, and the art knows a submesh is called
// `battleship_turret_0`. Nothing joined the two, so nothing could turn.
//
// **It is content, and it lives on the client side of ADR 0002**: a headless shard has no meshes and
// no business knowing a hull has a turret submesh. It is in `Outpost` rather than in `NeuronClient`
// because it names `Game::HullId`, and `NeuronClient` may not list `GameLogic` (AGENTS.md §3) --
// which is the one place the work order was wrong about where this goes, and is recorded in
// `Design/Combat-slice-6.md` §7. `Outpost` is the composition root, is the only thing that sees both
// layers, and already holds the hull-to-mesh table this is the sibling of.
//
// A hull with no row here, or a mount with no row, binds nothing and draws exactly as it does today:
// one instanced draw, no slew, effects from the hull's origin. That is the design's stated rule --
// content is a diagnostic, never a crash (`Combat.md` §3.1) -- and it is why this table can be
// authored one hull at a time.

// The most submeshes one mount may turn: a turret and its barrels. Three is what the shipped
// Battleship needs (a turret and two barrels) and nothing authored wants more.
inline constexpr std::uint32_t MAX_MOUNT_PARTS = 3;

struct MountArt
{
  Game::HullId hull = Game::HullId::Interceptor;
  std::uint32_t mount = 0;
  // Authored submesh names, in the mesh's own spelling. A trailing nullptr ends the list, so a mount
  // that turns one part costs one name rather than three.
  const char* parts[MAX_MOUNT_PARTS]{};
};

// The rows, and every one of them is a claim about the shipped art that
// `Tools/NmoShippedArtTest.py` checks.
//
// The Battleship's three heavy turrets and the Corvette's two are what `Combat.md` §10.1 said were
// "addressable the day this lands". Its two light mounts (3 and 4) and the Carrier's four have no
// turret submesh in the shipped art -- the design says as much, "mounts the shipped art can MOSTLY
// already wear" (§12) -- so they bind nothing and do not turn. The Interceptor, the Bomber and the
// Fighter carry fixed guns, which never turn whatever they are bound to, so they are not here at
// all.
inline constexpr MountArt HULL_MOUNT_ART[] = {
  {.hull = Game::HullId::Corvette, .mount = 0, .parts = {"corvette_turret"}},
  {.hull = Game::HullId::Corvette, .mount = 1, .parts = {"corvette_turret.001"}},
  {.hull = Game::HullId::Battleship, .mount = 0, .parts = {"battleship_turret_0", "battleship_barrel_0", "battleship_barrel_0.001"}},
  {.hull = Game::HullId::Battleship, .mount = 1, .parts = {"battleship_turret_1", "battleship_barrel_1", "battleship_barrel_1.001"}},
  {.hull = Game::HullId::Battleship, .mount = 2, .parts = {"battleship_turret_2", "battleship_barrel_2", "battleship_barrel_2.001"}},
};

// FNV-1a 32, the hash a submesh is stored under (Design/Archive/NmoFormat.md 5.10). constexpr, so a
// row's names are hashed at compile time and no string reaches a frame.
[[nodiscard]] constexpr std::uint32_t NameHash(const char* _name) noexcept
{
  std::uint32_t value = 2166136261u;
  for (const char* at = _name; *at != '\0'; ++at)
    value = (value ^ static_cast<std::uint32_t>(static_cast<unsigned char>(*at))) * 16777619u;
  return value;
}

// One mount of one ship, resolved against the mesh it is drawn from: what turns, what it turns
// about, where it rests, how far it may bear and how fast it gets there.
//
// Resolved once when a hull first appears, for the reason ShipView copies restY and pickCentre out
// of MeshData: the draw loop runs per ship per frame and must not be looking anything up by name.
struct MountView
{
  Neuron::MeshRange parts[MAX_MOUNT_PARTS];
  std::uint32_t partCount = 0;
  DirectX::XMFLOAT3 pivot{0.0f, 0.0f, 0.0f}; // mesh space, the turret's own bind-pose centre

  float restRad = 0.0f;    // the mount's authored bearing: where it points with nothing to shoot at
  float arcHalfRad = 0.0f; // how far either side of rest it may bear
  float traverseRadPerSec = 0.0f;

  // Where it is pointing now, in the hull frame, and where it is trying to point. Both start at
  // rest, so a hull that appears mid-battle draws stowed and turns from there rather than snapping.
  float aimRad = 0.0f;
  float wantRad = 0.0f;

  // How long the last shot from this mount is still worth holding the turret on. Counted down in
  // seconds by the view's own frame clock: a turret that snapped back the instant a shot finished
  // drawing would twitch on every cooldown.
  float holdSec = 0.0f;
};

// The mounts of _hull that this build can turn, resolved against _mesh.
//
// Empty for every hull with no row, every mount whose device is fixed, and every named part the mesh
// does not carry -- three separate ways of saying "draw it the way you always did", all of which end
// in the same one draw.
[[nodiscard]] std::vector<MountView> ResolveMounts(Game::HullId _hull, const Neuron::MeshData& _mesh);

// Advances one mount by _dtSec: turn toward wantRad at traverseRadPerSec, or drift back to rest once
// the hold has run out. Free rather than a method, because it is the arithmetic worth testing and it
// touches nothing else.
void SlewMount(MountView& _mount, float _dtSec) noexcept;

// The bearing _localX/_localZ makes in the hull frame, clamped into the mount's arc about its rest.
// The clamp is what stops a turret from swinging through the hull to reach a target behind it.
[[nodiscard]] float MountBearingToward(const MountView& _mount, float _localX, float _localZ) noexcept;
} // namespace Outpost
