#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace NeuronCoreTests
{
TEST_CLASS(ObjParserTests)
{
public:
  TEST_METHOD(AMissingMeshFailsClosed)
  {
    // Content errors are diagnostics, never crashes. A mesh that is not there reports false and
    // leaves the output alone; it does not throw, and it does not half-fill the mesh.
    Neuron::MeshData mesh;
    Assert::IsFalse(Neuron::ObjParser::Load(L"Meshes\\", L"NoSuchHullExists", mesh), L"a missing mesh reported success");
    Assert::IsTrue(mesh.Empty(), L"a failed load left vertices behind");
  }

  TEST_METHOD(EmptyBoundsAreUsable)
  {
    // Every consumer divides by or scales with these, so a default-constructed mesh has to give
    // answers that do not produce a degenerate matrix.
    const Neuron::MeshData mesh;
    const XMFLOAT3 extents = mesh.HalfExtents();
    Assert::IsTrue(extents.x > 0.0f && extents.y > 0.0f && extents.z > 0.0f, L"an empty mesh has a zero extent");
    Assert::AreEqual(0.0f, mesh.RestY(), 1e-5f, L"an empty mesh does not rest on the ground plane");
  }

  TEST_METHOD(BoundsGiveTheCentreAndTheLift)
  {
    Neuron::MeshData mesh;
    mesh.boundsMin = XMFLOAT3(-2.0f, -3.0f, -10.0f);
    mesh.boundsMax = XMFLOAT3(2.0f, 5.0f, 10.0f);

    const XMFLOAT3 centre = mesh.BoundsCentre();
    Assert::AreEqual(0.0f, centre.x, 1e-5f, L"centre x");
    Assert::AreEqual(1.0f, centre.y, 1e-5f, L"centre y");
    Assert::AreEqual(0.0f, centre.z, 1e-5f, L"centre z");

    const XMFLOAT3 extents = mesh.HalfExtents();
    Assert::AreEqual(2.0f, extents.x, 1e-5f, L"half extent x");
    Assert::AreEqual(4.0f, extents.y, 1e-5f, L"half extent y");
    Assert::AreEqual(10.0f, extents.z, 1e-5f, L"half extent z");

    Assert::AreEqual(3.0f, mesh.RestY(), 1e-5f, L"the lift does not put the lowest vertex on y = 0");
  }
};
} // namespace NeuronCoreTests
