#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NeuronClientTests
{
namespace
{
constexpr std::uint32_t MESH_VERTS = 100;

// A run, named. Every literal below goes through it, so no row binds MeshRange's fields by position
// and a row reads as "from here, this many" (AGENTS.md 1 R8).
[[nodiscard]] constexpr Neuron::MeshRange Run(std::uint32_t _firstVertex, std::uint32_t _vertexCount) noexcept
{
  return Neuron::MeshRange{.firstVertex = _firstVertex, .vertexCount = _vertexCount};
}

// The complement, as a vector, so a row reads as the answer rather than as a count and a buffer.
[[nodiscard]] std::vector<Neuron::MeshRange> Complement(std::vector<Neuron::MeshRange> _posed, std::uint32_t _vertexCount = MESH_VERTS)
{
  std::vector<Neuron::MeshRange> gaps(Neuron::ComplementCapacity(_posed.size()));
  const std::size_t written = Neuron::RangeComplement(_posed, _vertexCount, gaps);
  gaps.resize(written);
  return gaps;
}

void AssertRuns(const std::vector<Neuron::MeshRange>& _got, std::initializer_list<Neuron::MeshRange> _want, const wchar_t* _what)
{
  Assert::AreEqual(_want.size(), _got.size(), _what);
  std::size_t at = 0;
  for (const Neuron::MeshRange& want : _want)
  {
    Assert::AreEqual(want.firstVertex, _got[at].firstVertex, _what);
    Assert::AreEqual(want.vertexCount, _got[at].vertexCount, _what);
    ++at;
  }
}

// What the renderer actually relies on: the posed runs and the gaps together cover every vertex
// exactly once. Stated as a census rather than as a shape, because it is the property that makes a
// hull draw whole and it holds whatever the runs happen to be.
void AssertTiles(std::vector<Neuron::MeshRange> _posed, std::uint32_t _vertexCount = MESH_VERTS)
{
  const std::vector<Neuron::MeshRange> gaps = Complement(_posed, _vertexCount);
  std::vector<int> covered(_vertexCount, 0);
  for (const Neuron::MeshRange& run : _posed)
  {
    for (std::uint32_t v = run.firstVertex; v < run.End() && v < _vertexCount; ++v)
      ++covered[v];
  }
  for (const Neuron::MeshRange& run : gaps)
  {
    for (std::uint32_t v = run.firstVertex; v < run.End() && v < _vertexCount; ++v)
      ++covered[v];
  }
  for (std::uint32_t v = 0; v < _vertexCount; ++v)
    Assert::AreEqual(1, covered[v], L"a vertex is drawn twice or not at all");
}
} // namespace

TEST_CLASS(MeshRangeTests)
{
public:
  TEST_METHOD(NothingPosedIsTheWholeMesh)
  {
    // The hull every ship in the game has: no turret bound, one draw, exactly as before this landed.
    AssertRuns(Complement({}), {Run(0, MESH_VERTS)}, L"an unposed mesh should complement to itself");
  }

  TEST_METHOD(OneRunInTheMiddleLeavesTwoGaps)
  {
    AssertRuns(Complement({Run(40, 20)}), {Run(0, 40), Run(60, 40)}, L"a run in the middle should leave a gap on each side");
    AssertTiles({Run(40, 20)});
  }

  TEST_METHOD(ARunAtEitherEndLeavesOneGap)
  {
    AssertRuns(Complement({Run(0, 30)}), {Run(30, 70)}, L"a run at the start should leave one gap after it");
    AssertRuns(Complement({Run(70, 30)}), {Run(0, 70)}, L"a run at the end should leave one gap before it");
    AssertTiles({Run(0, 30)});
    AssertTiles({Run(70, 30)});
  }

  TEST_METHOD(TheWholeMeshPosedLeavesNothing)
  {
    AssertRuns(Complement({Run(0, MESH_VERTS)}), {}, L"posing the whole mesh should leave no complement at all");
    AssertTiles({Run(0, MESH_VERTS)});
  }

  TEST_METHOD(AdjacentRunsMergeRatherThanLeavingAnEmptyOneBetween)
  {
    // The case that motivates the merge: two turrets whose submeshes happen to be neighbours in file
    // order. A zero-length gap between them would be a draw of nothing, every frame, for ever.
    AssertRuns(Complement({Run(20, 10), Run(30, 10)}), {Run(0, 20), Run(40, 60)}, L"adjacent runs should leave no empty gap between them");
    AssertTiles({Run(20, 10), Run(30, 10)});
  }

  TEST_METHOD(TheOrderTheRunsArriveInDoesNotMatter)
  {
    // A caller builds its posed list in mount order, which is not file order. Three turrets, handed
    // over backwards, must give the same three gaps.
    const std::vector<Neuron::MeshRange> forwards = Complement({Run(10, 5), Run(40, 5), Run(80, 5)});
    const std::vector<Neuron::MeshRange> backwards = Complement({Run(80, 5), Run(10, 5), Run(40, 5)});
    AssertRuns(forwards, {Run(0, 10), Run(15, 25), Run(45, 35), Run(85, 15)}, L"three runs should leave four gaps");
    AssertRuns(backwards, {Run(0, 10), Run(15, 25), Run(45, 35), Run(85, 15)}, L"the same runs in another order gave another answer");
    AssertTiles({Run(80, 5), Run(10, 5), Run(40, 5)});
  }

  TEST_METHOD(OverlappingRunsAreMergedRatherThanDrawnTwice)
  {
    // Not a case the mount table should produce, and exactly the case that is wrong in silence if it
    // does: a gap carved out of an already-covered run would draw those vertices a second time,
    // unposed, inside the turret that is turning.
    AssertRuns(Complement({Run(20, 30), Run(30, 30)}), {Run(0, 20), Run(60, 40)}, L"overlapping runs should merge into one covered span");
    AssertRuns(Complement({Run(20, 40), Run(30, 10)}), {Run(0, 20), Run(60, 40)}, L"a run wholly inside another should not reopen it");
  }

  TEST_METHOD(ARunThePartDoesNotHaveIsIgnored)
  {
    // MeshData::RangeOf answers a zero-count run for a part a mesh does not carry, so a hull whose
    // art has no turret must draw whole rather than not at all.
    AssertRuns(Complement({Run(0, 0)}), {Run(0, MESH_VERTS)}, L"an empty run should cover nothing");
    AssertRuns(Complement({Run(40, 20), Run(0, 0)}), {Run(0, 40), Run(60, 40)}, L"an empty run beside a real one should change nothing");

    // And a run past the end is clamped rather than trusted.
    AssertRuns(Complement({Run(90, 50)}), {Run(0, 90)}, L"a run past the end should be clamped to it");
    AssertRuns(Complement({Run(200, 10)}), {Run(0, MESH_VERTS)}, L"a run wholly past the end should cover nothing");
  }

  TEST_METHOD(AMeshWithNoVerticesComplementsToNothing)
  {
    AssertRuns(Complement({}, 0), {}, L"an empty mesh has no complement to draw");
    AssertRuns(Complement({Run(0, 10)}, 0), {}, L"an empty mesh has no complement to draw");
  }
};
} // namespace NeuronClientTests
