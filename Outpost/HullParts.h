#pragma once

#include "MeshData.h"

#include "DeviceSpec.h"
#include "HullSpec.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Outpost
{
// Which parts of a hull's art turn with which mount.
//
// This is the binding `Design/Archive/Combat.md` §10.1 named and three slices waited on: the simulation knows a
// mount has a bearing, an arc and a device, and the art knows a submesh is called
// `battleship_turret_0`. Nothing joined the two, so nothing could turn.
//
// **It is content, and it lives on the client side of ADR 0002**: a headless shard has no meshes and
// no business knowing a hull has a turret submesh. It is in `Outpost` rather than in `NeuronClient`
// because it names `Game::HullId`, and `NeuronClient` may not list `GameLogic` (AGENTS.md §3) --
// which is the one place the work order was wrong about where this goes, and is recorded in
// `Design/Archive/Combat-slice-6.md` §7. `Outpost` is the composition root, is the only thing that sees both
// layers, and already holds the hull-to-mesh table this is the sibling of.
//
// A hull with no row here, or a mount with no row, binds nothing and draws exactly as it does today:
// one instanced draw, no slew, effects from the hull's origin. That is the design's stated rule --
// content is a diagnostic, never a crash (`Design/Archive/Combat.md` §3.1) -- and it is why this table can be
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
// The Battleship's three heavy turrets, the Corvette's two and the Frigate's two batteries. The
// Battleship's two light mounts (3 and 4) and the Carrier's four have no turret submesh in the
// shipped art -- the design says as much, "mounts the shipped art can MOSTLY already wear" (§12) --
// so they bind nothing and do not turn. The Interceptor, the Bomber and the Fighter carry fixed
// guns, which never turn whatever they are bound to, so they are not here at all.
//
// **A known disagreement, recorded rather than papered over**: on both the Corvette and the Frigate,
// `HullSpec` bears these two mounts fore and aft (0 and pi) while the art puts both turrets port and
// starboard. With a +/-150 degree arc that changes no engagement -- only where a stowed turret
// points -- and correcting the bearings is simulation content that re-opens the matchup matrix, so
// it is owed to a later pass (Design/Archive/Combat-slice-6.md 8).
inline constexpr MountArt HULL_MOUNT_ART[] = {
  {.hull = Game::HullId::Corvette, .mount = 0, .parts = {"corvette_turret"}},
  {.hull = Game::HullId::Corvette, .mount = 1, .parts = {"corvette_turret.001"}},
  // The BATTERIES and not the lances, which the owner settled on 2026-09-02: they are the parts
  // named like guns, they sit where a traversing turret belongs, and the lances are a fixed forward
  // feature of the hull. See Design/Archive/Combat-slice-6.md 8 for the bearing disagreement this leaves.
  {.hull = Game::HullId::Frigate, .mount = 0, .parts = {"frigate_battery"}},
  {.hull = Game::HullId::Frigate, .mount = 1, .parts = {"frigate_battery.001"}},
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
  // Which mount this is. Carried rather than implied by position in the list: the list holds only
  // the mounts the art binds and the device turns, so it is DENSE where mount indices are not -- the
  // Battleship binds 0, 1 and 2 of five. Indexing this list by a mount id happens to work today,
  // because the bound mounts are a prefix on every shipped hull, and would silently point at the
  // wrong turret the first time one is not.
  std::uint32_t mount = 0;

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

// The most muzzles one mount may have authored: a turret with four barrels is past anything the
// roster carries, and the search below is a fixed set of names per mount rather than a scan.
inline constexpr std::uint32_t MAX_MOUNT_MUZZLES = 4;

// One authored muzzle, in mesh space: where a shot from its mount draws from.
//
// `MeshMarker` keeps a marker's name only as a hash, so the client cannot read `Gun0A` off a file
// and parse it -- it has to hash the names it expects and look those up. That is why the naming rule
// is a rule and not a convention: `Gun<N>` and `Gun<N><letter>` are the only shapes anything can
// find, and `Tools/NmoShippedArtTest.py` is what holds the art to them.
struct MuzzleView
{
  std::uint32_t mount = 0;
  DirectX::XMFLOAT3 local{0.0f, 0.0f, 0.0f};
};

// Every authored muzzle of _mesh, for a hull carrying _mountCount mounts.
//
// A mount with no marker contributes nothing and its shots draw from the hull's origin, which is
// what `Design/Archive/Combat.md` §3.1 says content that is missing must do. Six shipped mounts are in that state.
[[nodiscard]] std::vector<MuzzleView> ResolveMuzzles(const Neuron::MeshData& _mesh, std::uint32_t _mountCount);

// The mounts of _hull that this build can turn, resolved against _mesh.
//
// Empty for every hull with no row, every mount whose device is fixed, and every named part the mesh
// does not carry -- three separate ways of saying "draw it the way you always did", all of which end
// in the same one draw.
[[nodiscard]] std::vector<MountView> ResolveMounts(Game::HullId _hull, const Neuron::MeshData& _mesh);

// The mount with index _mount in _mounts, or nullptr. Linear over at most six entries, which is
// cheaper than the branch that would keep a lookup table in step with them.
[[nodiscard]] const MountView* FindMount(std::span<const MountView> _mounts, std::uint32_t _mount) noexcept;
[[nodiscard]] MountView* FindMount(std::span<MountView> _mounts, std::uint32_t _mount) noexcept;

// Advances one mount by _dtSec: turn toward wantRad at traverseRadPerSec, or drift back to rest once
// the hold has run out. Free rather than a method, because it is the arithmetic worth testing and it
// touches nothing else.
void SlewMount(MountView& _mount, float _dtSec) noexcept;

// The bearing _localX/_localZ makes in the hull frame, clamped into the mount's arc about its rest.
// The clamp is what stops a turret from swinging through the hull to reach a target behind it.
[[nodiscard]] float MountBearingToward(const MountView& _mount, float _localX, float _localZ) noexcept;
} // namespace Outpost
