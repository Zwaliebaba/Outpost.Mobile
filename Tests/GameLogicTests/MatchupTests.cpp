#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// One owner per faction, which is what every row here means: these suites were written when the key
// WAS the faction, and this keeps each of them saying exactly what it said (Design/Archive/OwnerKey-work-order.md).
[[nodiscard]] Game::Issuer IssuerFor(Game::FactionId _faction)
{
  return Game::Issuer{(_faction == Game::FACTION_PLAYER) ? Game::OWNER_LOCAL : Game::OwnerId{_faction} + 1u, _faction};
}

// The matchup matrix: every combatant hull against every other under the shipped tables, with the
// outcome of each cell written down (Design/Archive/MatchupMatrix-work-order.md). It moves no number. It is
// the instrument combat slice 5 measured with and did not commit, brought into the suite so that a
// retune which changes a fight's character -- who wins, or how long it takes by more than the band
// -- fails a test rather than a play session (Design/GameDesignReview.md, Combat 13).
//
// The geometry is this file's own: six hundred metres bow to bow, each hull a fleet of one, both
// ordered to attack. Slice 5's harness used another, unrecorded geometry, and its numbers are not
// reproduced here and not expected to be.
enum class Side : std::uint8_t
{
  Mine,
  Theirs,
  Both,
  Stalemate
};

struct Cell
{
  Game::HullId mine;
  Game::HullId theirs;
  Side outcome;
  int endTicks; // the tick the fight ended, or LIMIT_TICKS for a stalemate
};

constexpr Game::HullId COMBATANTS[] = {Game::HullId::Interceptor, Game::HullId::Bomber,     Game::HullId::Corvette,
                                       Game::HullId::Frigate,     Game::HullId::Battleship, Game::HullId::Carrier};

constexpr int LIMIT_TICKS = 60 * 180;       // three minutes; the longest decided cell ends inside it
constexpr float DUEL_APART_METRES = 600.0f; // outside every device range, inside the leash
constexpr float BAND = 0.15f;               // how far an end tick may move before the cell fails

// Measured on 2026-09-02 at the tables as shipped (Design/Archive/MatchupMatrix-work-order.md 7). Read a row
// as "mine versus theirs": an Interceptor against a Bomber loses at tick 370.
//
// These are CI's numbers -- Debug|x64, MSVC -- and not the Linux harness's that drafted them. MSVC
// is the only compiler this tree builds with (AGENTS.md 6), so within it the determinism contract
// makes every tick below exact and the band absorbs nothing at all; the band is there for the day a
// runner image brings a different build of the compiler. Three cells differed between the two
// toolchains by ulp-level drift compounded over a long fight, and the two longest are why the band
// is not tighter: Battleship against Carrier came out 10.3% apart, which is most of the fifteen
// percent. A cell that fails after a runner image changes and no number moved is that, and the log
// says which cell and by how much.
constexpr Cell DUELS[] = {
  {Game::HullId::Interceptor, Game::HullId::Interceptor, Side::Stalemate, LIMIT_TICKS},
  {Game::HullId::Interceptor, Game::HullId::Bomber, Side::Theirs, 370},
  {Game::HullId::Interceptor, Game::HullId::Corvette, Side::Theirs, 704},
  {Game::HullId::Interceptor, Game::HullId::Frigate, Side::Theirs, 568},
  {Game::HullId::Interceptor, Game::HullId::Battleship, Side::Theirs, 526},
  {Game::HullId::Interceptor, Game::HullId::Carrier, Side::Theirs, 708},
  {Game::HullId::Bomber, Game::HullId::Interceptor, Side::Mine, 370},
  {Game::HullId::Bomber, Game::HullId::Bomber, Side::Stalemate, LIMIT_TICKS},
  {Game::HullId::Bomber, Game::HullId::Corvette, Side::Theirs, 1211},
  {Game::HullId::Bomber, Game::HullId::Frigate, Side::Theirs, 780},
  {Game::HullId::Bomber, Game::HullId::Battleship, Side::Theirs, 659},
  {Game::HullId::Bomber, Game::HullId::Carrier, Side::Theirs, 984},
  {Game::HullId::Corvette, Game::HullId::Interceptor, Side::Mine, 735},
  {Game::HullId::Corvette, Game::HullId::Bomber, Side::Mine, 1212},
  {Game::HullId::Corvette, Game::HullId::Corvette, Side::Both, 2572},
  {Game::HullId::Corvette, Game::HullId::Frigate, Side::Theirs, 1017},
  {Game::HullId::Corvette, Game::HullId::Battleship, Side::Theirs, 771},
  {Game::HullId::Corvette, Game::HullId::Carrier, Side::Theirs, 1248},
  {Game::HullId::Frigate, Game::HullId::Interceptor, Side::Mine, 568},
  {Game::HullId::Frigate, Game::HullId::Bomber, Side::Mine, 780},
  {Game::HullId::Frigate, Game::HullId::Corvette, Side::Mine, 1017},
  {Game::HullId::Frigate, Game::HullId::Frigate, Side::Both, 2907},
  {Game::HullId::Frigate, Game::HullId::Battleship, Side::Theirs, 1380},
  {Game::HullId::Frigate, Game::HullId::Carrier, Side::Theirs, 1797},
  {Game::HullId::Battleship, Game::HullId::Interceptor, Side::Mine, 526},
  {Game::HullId::Battleship, Game::HullId::Bomber, Side::Mine, 659},
  {Game::HullId::Battleship, Game::HullId::Corvette, Side::Mine, 771},
  {Game::HullId::Battleship, Game::HullId::Frigate, Side::Mine, 1380},
  {Game::HullId::Battleship, Game::HullId::Battleship, Side::Both, 7231},
  {Game::HullId::Battleship, Game::HullId::Carrier, Side::Mine, 10193},
  {Game::HullId::Carrier, Game::HullId::Interceptor, Side::Mine, 708},
  {Game::HullId::Carrier, Game::HullId::Bomber, Side::Mine, 984},
  {Game::HullId::Carrier, Game::HullId::Corvette, Side::Mine, 1253},
  {Game::HullId::Carrier, Game::HullId::Frigate, Side::Mine, 1797},
  {Game::HullId::Carrier, Game::HullId::Battleship, Side::Theirs, 9242},
  {Game::HullId::Carrier, Game::HullId::Carrier, Side::Stalemate, LIMIT_TICKS},
};
static_assert(std::size(DUELS) == std::size(COMBATANTS) * std::size(COMBATANTS), "the matrix has a cell per ordered pair");

// The fights the pacing targets name (Design/Combat.md 13), as groups under the same rule. The
// mixed eight is two of each armed non-capital hull; the design says "a mixed eight-fleet" and no
// more.
constexpr Game::HullId EIGHT_CORVETTES[] = {Game::HullId::Corvette, Game::HullId::Corvette, Game::HullId::Corvette, Game::HullId::Corvette,
                                            Game::HullId::Corvette, Game::HullId::Corvette, Game::HullId::Corvette, Game::HullId::Corvette};
constexpr Game::HullId TWO_INTERCEPTORS[] = {Game::HullId::Interceptor, Game::HullId::Interceptor};
constexpr Game::HullId TWO_BOMBERS[] = {Game::HullId::Bomber, Game::HullId::Bomber};
constexpr Game::HullId MIXED_EIGHT[] = {Game::HullId::Interceptor, Game::HullId::Interceptor, Game::HullId::Bomber,  Game::HullId::Bomber,
                                        Game::HullId::Corvette,    Game::HullId::Corvette,    Game::HullId::Frigate, Game::HullId::Frigate};

struct GroupRow
{
  const wchar_t* label;
  const Game::HullId* hulls;
  std::size_t count;
  Game::HullId theirs;
  float apartMetres;
  Side outcome;
  std::uint32_t mineLeft;
  std::uint32_t theirsLeft;
  int endTicks;
};

constexpr GroupRow GROUPS[] = {
  {L"eight Corvettes on an Interceptor", EIGHT_CORVETTES, std::size(EIGHT_CORVETTES), Game::HullId::Interceptor, 300.0f, Side::Mine, 8, 0,
   249},
  {L"two Interceptors on a Frigate", TWO_INTERCEPTORS, std::size(TWO_INTERCEPTORS), Game::HullId::Frigate, 600.0f, Side::Theirs, 0, 1, 727},
  {L"two Bombers on a Frigate", TWO_BOMBERS, std::size(TWO_BOMBERS), Game::HullId::Frigate, 600.0f, Side::Theirs, 0, 1, 1492},
  {L"a mixed eight on a Battleship", MIXED_EIGHT, std::size(MIXED_EIGHT), Game::HullId::Battleship, 800.0f, Side::Theirs, 0, 1, 4536},
  // The one marginal row: a mixed eight grinds a Carrier down over a minute and a half and how many
  // of the eight are left at the end is decided in its last seconds. The Linux harness that drafted
  // this table had five survivors at 5,510 ticks and CI has four at 5,613 -- the same fight, one
  // ship's death landing on the other side of the end. Every other row here is a wipe with the
  // winner untouched and does not move. If this one moves again, read the outcome first: four
  // against five is the margin, and "mine" against "theirs" is the character.
  {L"a mixed eight on a Carrier", MIXED_EIGHT, std::size(MIXED_EIGHT), Game::HullId::Carrier, 800.0f, Side::Mine, 4, 0, 5613},
};

const wchar_t* const HULL_NAMES[] = {L"Interceptor", L"Bomber",     L"Corvette", L"Miner",    L"Frigate",
                                     L"Hauler",      L"Battleship", L"Carrier",  L"Stargate", L"Structure"};

const wchar_t* NameOf(Game::HullId _hull)
{
  return HULL_NAMES[static_cast<std::uint32_t>(_hull)];
}

const wchar_t* NameOf(Side _side)
{
  switch (_side)
  {
  case Side::Mine:
    return L"mine";
  case Side::Theirs:
    return L"theirs";
  case Side::Both:
    return L"both";
  case Side::Stalemate:
    return L"stalemate";
  }
  return L"?";
}

Game::ShipId Spawn(Game::Universe& _universe, float _x, float _z, float _headingRad, Game::HullId _hull, Game::FactionId _faction)
{
  return _universe.SpawnShip(Game::LocalPos(_x, _z), _headingRad, static_cast<std::uint32_t>(_hull), _faction);
}

void OrderAttack(Game::Universe& _universe, Game::FactionId _faction, Game::ShipId _target)
{
  Game::Universe::FleetCommand command;
  command.kind = Game::FleetOrderKind::Attack;
  command.target = _target;
  Assert::IsTrue(_universe.IssueFleetOrder(IssuerFor(_faction), 0, command) == Game::Universe::FleetOrderResult::Ordered,
                 L"an attack order was refused");
}

struct Survivors
{
  std::uint32_t mine = 0;
  std::uint32_t theirs = 0;
};

Survivors CountSides(const Game::Universe& _universe)
{
  Survivors left;
  for (const Game::ShipState& ship : _universe.Ships())
  {
    if (ship.factionId == Game::FACTION_PLAYER)
      ++left.mine;
    else
      ++left.theirs;
  }
  return left;
}

struct Result
{
  Side outcome = Side::Stalemate;
  Survivors left;
  int endTicks = LIMIT_TICKS;
};

// Steps until one side has nothing left, or the limit.
Result Fight(Game::Universe& _universe)
{
  Result result;
  for (int tick = 1; tick <= LIMIT_TICKS; ++tick)
  {
    _universe.Step();
    result.left = CountSides(_universe);
    if (result.left.mine == 0 || result.left.theirs == 0)
    {
      result.endTicks = tick;
      break;
    }
  }
  if (result.left.mine != 0 && result.left.theirs != 0)
    result.outcome = Side::Stalemate;
  else if (result.left.mine != 0)
    result.outcome = Side::Mine;
  else if (result.left.theirs != 0)
    result.outcome = Side::Theirs;
  else
    result.outcome = Side::Both;
  return result;
}

// Mine in a row along x, facing +z; theirs _apart metres up the z axis, facing back.
void Arrange(Game::Universe& _universe, std::span<const Game::HullId> _mine, Game::HullId _theirs, float _apartMetres)
{
  std::vector<Game::ShipId> mine;
  for (std::size_t at = 0; at < _mine.size(); ++at)
  {
    const float x = (static_cast<float>(at) - static_cast<float>(_mine.size() - 1) * 0.5f) * 60.0f;
    mine.push_back(Spawn(_universe, x, 0.0f, 0.0f, _mine[at], Game::FACTION_PLAYER));
  }
  const Game::ShipId theirs = Spawn(_universe, 0.0f, _apartMetres, DirectX::XM_PI, _theirs, Game::FACTION_VANDAL);
  const Game::ShipId theirFleet[] = {theirs};
  Assert::IsTrue(_universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0,
                                     std::span<const Game::ShipId>(mine.data(), mine.size())) != Game::Universe::INVALID_FLEET_ID,
                 L"the player's fleet did not form");
  Assert::IsTrue(_universe.FormFleet(Game::Issuer{Game::OwnerId{2}, Game::FACTION_VANDAL}, 0,
                                     std::span<const Game::ShipId>(theirFleet, 1)) != Game::Universe::INVALID_FLEET_ID,
                 L"the hostile fleet did not form");
  OrderAttack(_universe, Game::FACTION_PLAYER, theirs);
  OrderAttack(_universe, Game::FACTION_VANDAL, mine.front());
}

Result Duel(Game::HullId _mine, Game::HullId _theirs)
{
  Game::Universe universe;
  const Game::HullId mine[] = {_mine};
  Arrange(universe, mine, _theirs, DUEL_APART_METRES);
  return Fight(universe);
}

bool InBand(int _measured, int _expected)
{
  const float low = static_cast<float>(_expected) * (1.0f - BAND);
  const float high = static_cast<float>(_expected) * (1.0f + BAND);
  return static_cast<float>(_measured) >= low && static_cast<float>(_measured) <= high;
}
} // namespace

TEST_CLASS(MatchupTests)
{
public:
  TEST_METHOD(TheDuelMatrixIsWhatItWas)
  {
    // Every cell is measured before any is judged, so a failing run logs the whole matrix: a retune
    // reads what it did from the log rather than from thirty-six reruns.
    std::vector<Result> measured;
    for (const Cell& cell : DUELS)
      measured.push_back(Duel(cell.mine, cell.theirs));

    bool allHold = true;
    for (std::size_t at = 0; at < std::size(DUELS); ++at)
    {
      const Cell& cell = DUELS[at];
      const Result& got = measured[at];
      const bool holds = got.outcome == cell.outcome && (cell.outcome == Side::Stalemate || InBand(got.endTicks, cell.endTicks));
      allHold = allHold && holds;
      Logger::WriteMessage(std::format(L"MATRIX|{} vs {}|{}|{} ticks|expected {} at {}|{}", NameOf(cell.mine), NameOf(cell.theirs),
                                       NameOf(got.outcome), got.endTicks, NameOf(cell.outcome), cell.endTicks, holds ? L"ok" : L"MOVED")
                             .c_str());
    }
    Assert::IsTrue(allHold, L"a cell of the matchup matrix moved -- the log has every cell");
  }

  TEST_METHOD(TheGroupRowsAreWhatTheyWere)
  {
    bool allHold = true;
    for (const GroupRow& row : GROUPS)
    {
      Game::Universe universe;
      Arrange(universe, std::span<const Game::HullId>(row.hulls, row.count), row.theirs, row.apartMetres);
      const Result got = Fight(universe);
      const bool holds = got.outcome == row.outcome && got.left.mine == row.mineLeft && got.left.theirs == row.theirsLeft &&
                         (row.outcome == Side::Stalemate || InBand(got.endTicks, row.endTicks));
      allHold = allHold && holds;
      Logger::WriteMessage(std::format(L"GROUP|{}|{}|{} ticks|survivors {} and {}|expected {} at {} with {} and {}|{}", row.label,
                                       NameOf(got.outcome), got.endTicks, got.left.mine, got.left.theirs, NameOf(row.outcome), row.endTicks,
                                       row.mineLeft, row.theirsLeft, holds ? L"ok" : L"MOVED")
                             .c_str());
    }
    Assert::IsTrue(allHold, L"a group row of the matchup matrix moved -- the log has every row");
  }

  TEST_METHOD(TheMatrixIsDeterministic)
  {
    // The matrix's claim to exactness, pinned beside it: one cell twice, compared on every tick.
    Game::Universe first;
    Game::Universe second;
    const Game::HullId mine[] = {Game::HullId::Corvette};
    Arrange(first, mine, Game::HullId::Frigate, DUEL_APART_METRES);
    Arrange(second, mine, Game::HullId::Frigate, DUEL_APART_METRES);
    for (int tick = 0; tick < 1200; ++tick)
    {
      first.Step();
      second.Step();
      Assert::AreEqual(first.ShipCount(), second.ShipCount(), L"the two fights lost different ships");
      for (Game::ShipId id = 0; id < first.ShipCount(); ++id)
      {
        Assert::AreEqual(first.Ship(id).hullPoints, second.Ship(id).hullPoints, L"hull points diverged");
        Assert::IsTrue(IsSamePosition(first.Ship(id).posUniverse, second.Ship(id).posUniverse), L"positions diverged");
      }
    }
  }
};
} // namespace GameLogicTests
