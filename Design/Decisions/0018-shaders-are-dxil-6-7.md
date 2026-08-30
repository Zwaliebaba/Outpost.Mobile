# 0018 — Shaders are DXIL for shader model 6.7, compiled by DXC

Status: accepted
Date: 2026-08-29

## Context

Every shader in the tree was compiled by FXC for shader model 5.1: thirteen small forward-pipeline
stages — flat-shaded hulls, a procedural ground grid, decals, the HUD atlas, the explosion's
fragments and sprites, and the two-pass body — none of which uses anything past SM 5.0. FXC has
been frozen since 2017; DXC is the compiler Microsoft and the IHVs develop, DXIL is the bytecode
their drivers' optimisers are tuned for, and everything after SM 5.1 — wave intrinsics, 16-bit
arithmetic, `ResourceDescriptorHeap`, mesh shaders, and the advanced texture operations of 6.7 —
is DXC-only. `Design/Archive/PlanetRenderer.md` §17 named the FXC toolchain as the reason a compute bake (slice 6)
was deferred; slice 6 has since landed anyway (`0017`), at the cost of a 32x32 -> 64 multiply
written out by hand because FXC rejects `umul`, and identifiers renamed around FXC's keyword
list. Both of those are FXC's, not the kernels'.

Visual Studio's `FxCompile` task runs the Windows SDK's `dxc.exe` when `ShaderModel` is 6.x, so
the switch is a project setting, not machinery. The installed SDK (10.0.26100) ships DXC
1.8.2502 with `dxil.dll` beside it, so DXIL is signed at build time and needs no runtime
component. All thirteen shaders compile for 6.7 unchanged, with strict mode kept.

A DXIL 6.7 pipeline state fails to create on a device whose driver stops short of 6.7, and the
error it fails with is `E_INVALIDARG` with no further word. Feature level 11_0, which `GpuDevice`
asks for, says nothing about the shader model.

## Decision

Compile every shader as shader model 6.7 through DXC, spelled as `ShaderModel` 6.7 in the four
`FxCompile` `ItemDefinitionGroup`s of `NeuronClient.vcxproj`, with `AllResourcesBound` on and, in
Debug, the PDB embedded in the container (`/Qembed_debug`) so PIX can source-step it. Ask the
device for `D3D12_FEATURE_SHADER_MODEL` at creation and refuse a device below 6.7 with a trace
that names the model it reached.

## Alternatives considered

- **Stay on FXC and 5.1.** Nothing in the thirteen shaders needs more. Lost because the compiler
  is dead, the compute bake and any wave-level work are impossible on it, and the cost of moving
  is one setting and a capability check.
- **6.0 or 6.6 rather than 6.7.** 6.6 is where `ResourceDescriptorHeap` and `IsHelperLane` arrive
  and is more widely supported on older hardware. Lost because the target devices are specified
  as supporting shader model 6.7 at minimum, the tree runs on Windows 11 with in-box 6.7 support,
  and there is no reason to pin below the model the SDK's compiler, the runtime and the hardware
  floor all offer.
- **Compile at runtime through `dxcompiler.dll`.** Would allow choosing the profile per device.
  Lost for the reason AGENTS.md §3 already gives: a shader mistake is a build error, startup does
  no compilation, and the binary carries no compiler.
- **Rewrite the shaders around 6.7 features.** Examined stage by stage: `SampleCmpLevel`, raw
  gather, programmable sample offsets, `QuadAny`/`QuadAll`, writable MSAA and integer sampling
  have no consumer in a flat-shaded forward pass that samples four 2D textures with `Sample`.
  Lost because a change with no visible or measurable effect is a change with only risk.

## Consequences

- The four `CompiledShaders/*.h` are DXIL containers; the renderer code that binds them is
  unchanged.
- A machine whose driver reaches only 6.6 or below no longer starts; it says so in the trace.
  No supported target device is such a machine.
- The compute bake of `0017` compiles as `cs_6_7`: its emulated 64-bit multiply and its renamed
  identifiers become FXC workarounds with nothing left to work around, and its two reductions
  can use wave intrinsics. 16-bit types (`-enable-16bit-types`) and `ResourceDescriptorHeap`
  are available but not adopted — each is a separate decision with a root-signature or
  capability cost.
- The ARM64 configurations carry the same setting and remain, as before, unverified.
