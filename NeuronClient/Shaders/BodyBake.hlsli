// The generator, in HLSL. Every function here is a port of one in NeuronClient, and the ports are
// line for line on purpose: a body baked on the GPU has to be the body the CPU builder makes, and
// the acceptance for that is a readback compared against it (Design/PlanetRenderer.md 17.4).
//
// Three rules govern everything below.
//
// 1. **The arithmetic is in the same order as the C++.** a + b + c and a + (b + c) differ in the
//    last bit and six octaves of that is a few ULPs of height. Where a line here looks clumsy, it is
//    matching a line over there.
// 2. **Everything integer is bit-exact and is expected to be.** The cell hash, the dither's
//    generator and the noise permutation are integer end to end, so the grain of a baked body is the
//    grain of a built one exactly, not approximately.
// 3. **The CPU draws every random number; this file draws none.** It reads the block BodyParams
//    filled (17.3). There is no seeding here, only evaluation.
//
// One shape is FXC's rather than the C++'s: a function returns once, from a variable initialised
// where it is declared, or from a select. FXC reads an early return out of a branch as a value that
// might be uninitialised (warning X4000), so the originals' early exits are folded here. Every fold
// was checked to be bit for bit the function it replaced.

#define BAKE_MAX_TILES 8
#define BAKE_MAX_FLATTEN 32
#define BAKE_PERMUTATION_SIZE 256

struct BakeTile
{
  float4 centreHalfWidth;         // xyz centre direction, w halfWidthRad
  float4 edgeDesiredPosYRidged;   // x edgeFraction, y desiredHeight, z posY, w ridged
  float4 fractal;                 // x fractalDimension, y heightScale, z lowlandSmoothing, w amplitude
};

struct BakeFlatten
{
  float4 centreHalfWidth;         // xyz centre direction, w halfWidthRad
  float4 modeValueThreshold;      // x FlattenMode, y value, z threshold, w unused
};

// BodyParams, field for field. A constant buffer pads every array element to sixteen bytes, which is
// why the two scalar arrays over there are float4 and uint4 groups here and why BodyParams carries
// them the same way; a "tidier" float or uint array would silently occupy four times the space and
// read the wrong words. BodyRenderer static_asserts the total against sizeof(BodyParams).
cbuffer BakeParams : register(b0)
{
  float4 radiusEllipsoid;         // x radiusMetres, yzw ellipsoid
  float4 seedOffset;              // xyz where in the noise this body reads, w lumpiness
  float4 outsideMaxHeightGrid;    // x outsideHeight, y maxHeight, z gridPower, w octaves
  float4 polar;                   // x polarStrength, y capStart, z capNoise, w polarGeometry
  float4 spinAxis;                // xyz, w unused
  BakeTile tiles[BAKE_MAX_TILES];
  BakeFlatten flatten[BAKE_MAX_FLATTEN];
  uint4 counts;                   // x tileCount, y flattenCount, z seedLow, w seedHigh
  float4 octaveAmplitude[BAKE_MAX_TILES][2];
  uint4 permutation[BAKE_PERMUTATION_SIZE / 4];
};

// The constants of BodyField.cpp and BodyMeshBuilder.cpp, spelled with their names so the two files
// can be read side by side.
static const float SOURCE_LEVEL_LENGTH = 256.0;
static const float ROUGHNESS_FLOOR = 0.1;
static const float ROUGHNESS_GAIN = 0.15;
static const float OCTAVE_BASE_FREQUENCY = 0.5;
static const uint RIDGED_OCTAVES = 2;
static const uint LUMP_OCTAVES = 3;
static const float LUMP_BASE_FREQUENCY = 1.0;
static const float LUMP_OFFSET = 97.0;
static const float CLIMATE_DITHER_CELLS = 256.0;
static const float SOURCE_MAP_SIZE = 2000.0;
static const float SLOPE_EXPONENT = 0.4;
static const float DITHER_STRENGTH = 0.45;
static const float DITHER_SOFTENING = 2.0;
static const float BODY_SHORE_THRESHOLD = 0.0003;
static const float BODY_SHORE_DIP = -0.01;
static const float PI_OVER_4 = 0.78539816339744830961;

// ------------------------------------------------------------------------------------------------
// Sixty-four-bit integers, which shader model 5.1 does not have.
//
// Pcg32 is 64 bits of state and a 64-bit multiply-add, and splitmix64's finaliser is two more
// multiplies; there is no way to reproduce the CPU's dither without them. A uint2 with the low word
// first is the whole of it, and every operation below is exact -- this is the part of the port that
// is not "within a few ULPs" but identical.

uint2 Add64(uint2 _a, uint2 _b)
{
  uint2 sum;
  sum.x = _a.x + _b.x;
  sum.y = _a.y + _b.y + ((sum.x < _a.x) ? 1u : 0u);
  return sum;
}

// A 32x32 -> 64 product as (low, high), out of four 16-bit partial products. HLSL documents a `umul`
// intrinsic for exactly this and FXC does not have one -- "error X3004: undeclared identifier
// 'umul'" -- so it is written out. Every step is exact: no partial product here exceeds 32 bits, and
// the one place the middle can carry out of 32 is detected and folded into bit 48.
uint2 Mul32(uint _a, uint _b)
{
  const uint aLow = _a & 0xffffu;
  const uint aHigh = _a >> 16u;
  const uint bLow = _b & 0xffffu;
  const uint bHigh = _b >> 16u;

  const uint lowLow = aLow * bLow;
  const uint middle = aHigh * bLow + (lowLow >> 16u);
  const uint middleSum = middle + aLow * bHigh;
  const uint middleCarry = (middleSum < middle) ? 0x10000u : 0u; // carried out of 32 bits, so into bit 48

  uint2 product;
  product.x = (middleSum << 16u) | (lowLow & 0xffffu);
  product.y = aHigh * bHigh + (middleSum >> 16u) + middleCarry;
  return product;
}

uint2 Mul64(uint2 _a, uint2 _b)
{
  // The low 64 bits of a 128-bit product: the full 32x32 of the low words, and the two cross terms,
  // which is all of the rest that survives into the bottom half.
  const uint2 lowProduct = Mul32(_a.x, _b.x);

  uint2 product;
  product.x = lowProduct.x;
  product.y = lowProduct.y + _a.x * _b.y + _a.y * _b.x;
  return product;
}

uint2 Shr64(uint2 _value, uint _bits)
{
  // A shift of nothing is its own case: HLSL masks a shift count to five bits, so the general form's
  // `_value.y << 32` would be `_value.y << 0` and would fold the high word back in.
  uint2 shifted = _value;
  if (_bits >= 32u)
    shifted = uint2(_value.y >> (_bits - 32u), 0u);
  else if (_bits != 0u)
    shifted = uint2((_value.x >> _bits) | (_value.y << (32u - _bits)), _value.y >> _bits);

  return shifted;
}

uint2 Xor64(uint2 _a, uint2 _b)
{
  return uint2(_a.x ^ _b.x, _a.y ^ _b.y);
}

// ------------------------------------------------------------------------------------------------
// Pcg32, from NeuronCore/Pcg32.h. PCG-XSH-RR 64/32, the reference procedure exactly: seeding is
// zero the state, take the stream from the sequence, step, add the seed, step.

static const uint2 PCG_MULTIPLIER = uint2(0x4c957f2du, 0x5851f42du);      // 6364136223846793005
static const uint2 PCG_DEFAULT_SEQUENCE = uint2(0x94b95bdbu, 0xda3e39cbu); // 0xda3e39cb94b95bdb

struct Pcg32State
{
  uint2 state;
  uint2 increment;
};

uint Pcg32Step(inout Pcg32State _rng)
{
  // The output comes from the state before the advance, as the reference does.
  const uint2 previous = _rng.state;
  _rng.state = Add64(Mul64(previous, PCG_MULTIPLIER), _rng.increment);

  const uint2 shifted = Shr64(previous, 18u);
  const uint xorshifted = Shr64(Xor64(shifted, previous), 27u).x;
  const uint rotation = Shr64(previous, 59u).x;
  return (xorshifted >> rotation) | (xorshifted << ((0u - rotation) & 31u));
}

Pcg32State Pcg32Seed(uint2 _seed)
{
  Pcg32State rng;
  rng.state = uint2(0u, 0u);
  rng.increment = uint2((PCG_DEFAULT_SEQUENCE.x << 1u) | 1u, (PCG_DEFAULT_SEQUENCE.y << 1u) | (PCG_DEFAULT_SEQUENCE.x >> 31u));
  Pcg32Step(rng);
  rng.state = Add64(rng.state, _seed);
  Pcg32Step(rng);
  return rng;
}

// The top twenty-four bits, because twenty-four is what a float holds: every value is exact and no
// value is one.
float Pcg32Float01(inout Pcg32State _rng)
{
  return float(Pcg32Step(_rng) >> 8u) * (1.0 / 16777216.0);
}

float Pcg32Signed(inout Pcg32State _rng, float _magnitude)
{
  return (Pcg32Float01(_rng) * 2.0 - 1.0) * _magnitude;
}

// ------------------------------------------------------------------------------------------------
// The two hashes, from BodyMeshBuilder.h and BodyField.cpp. Integer throughout and bit-exact.

uint CellHash(uint2 _seed, uint _face, uint _x, uint _z)
{
  uint hash = _seed.x ^ _seed.y;
  hash ^= _face * 0x9E3779B9u;
  hash = (hash ^ (hash >> 16u)) * 0x85EBCA6Bu;
  hash ^= _x * 0xC2B2AE35u;
  hash = (hash ^ (hash >> 13u)) * 0x27D4EB2Fu;
  hash ^= _z;
  return hash ^ (hash >> 16u);
}

uint2 SplitMix(uint2 _value)
{
  uint2 mixed = Xor64(_value, Shr64(_value, 30u));
  mixed = Mul64(mixed, uint2(0x1ce4e5b9u, 0xbf58476du));
  mixed = Xor64(mixed, Shr64(mixed, 27u));
  mixed = Mul64(mixed, uint2(0x133111ebu, 0x94d049bbu));
  return Xor64(mixed, Shr64(mixed, 31u));
}

uint2 DirectionSeed(uint2 _seed, float3 _direction)
{
  // The CPU floors the scaled coordinate and casts through a signed 64-bit; the cast of a negative
  // value is its two's-complement pattern, which is what sign-extending the 32-bit result gives.
  const int cellX = int(floor(_direction.x * CLIMATE_DITHER_CELLS + 0.5));
  const int cellY = int(floor(_direction.y * CLIMATE_DITHER_CELLS + 0.5));
  const int cellZ = int(floor(_direction.z * CLIMATE_DITHER_CELLS + 0.5));

  uint2 hash = SplitMix(_seed);
  hash = SplitMix(Xor64(hash, uint2(uint(cellX), uint(cellX >> 31))));
  hash = SplitMix(Xor64(hash, uint2(uint(cellY), uint(cellY >> 31))));
  return SplitMix(Xor64(hash, uint2(uint(cellZ), uint(cellZ >> 31))));
}

// ------------------------------------------------------------------------------------------------
// Noise3, from Noise3.h. The permutation is read out of the uint4 groups and masked, where the CPU
// reads a doubled table; the two are the same lookup, because the second half of that table is the
// first half repeated.

uint Permutation(uint _index)
{
  const uint slot = _index & (BAKE_PERMUTATION_SIZE - 1u);
  const uint4 group = permutation[slot >> 2u];
  const uint within = slot & 3u;
  return (within == 0u) ? group.x : ((within == 1u) ? group.y : ((within == 2u) ? group.z : group.w));
}

float NoiseFade(float _t)
{
  return _t * _t * _t * (_t * (_t * 6.0 - 15.0) + 10.0);
}

float NoiseLerp(float _from, float _to, float _t)
{
  return _from + (_to - _from) * _t;
}

float NoiseGrad(uint _hash, float _x, float _y, float _z)
{
  const uint h = _hash & 15u;
  const float u = (h < 8u) ? _x : _y;
  const float v = (h < 4u) ? _y : ((h == 12u || h == 14u) ? _x : _z);
  return ((h & 1u) == 0u ? u : -u) + ((h & 2u) == 0u ? v : -v);
}

float NoiseSample(float3 _p)
{
  const float3 base = floor(_p);
  const uint cellX = uint(int(base.x)) & 255u;
  const uint cellY = uint(int(base.y)) & 255u;
  const uint cellZ = uint(int(base.z)) & 255u;

  const float x = _p.x - base.x;
  const float y = _p.y - base.y;
  const float z = _p.z - base.z;

  const float u = NoiseFade(x);
  const float v = NoiseFade(y);
  const float w = NoiseFade(z);

  const uint a = Permutation(cellX) + cellY;
  const uint aa = Permutation(a) + cellZ;
  const uint ab = Permutation(a + 1u) + cellZ;
  const uint b = Permutation(cellX + 1u) + cellY;
  const uint ba = Permutation(b) + cellZ;
  const uint bb = Permutation(b + 1u) + cellZ;

  const float x00 = NoiseLerp(NoiseGrad(Permutation(aa), x, y, z), NoiseGrad(Permutation(ba), x - 1.0, y, z), u);
  const float x10 = NoiseLerp(NoiseGrad(Permutation(ab), x, y - 1.0, z), NoiseGrad(Permutation(bb), x - 1.0, y - 1.0, z), u);
  const float x01 = NoiseLerp(NoiseGrad(Permutation(aa + 1u), x, y, z - 1.0), NoiseGrad(Permutation(ba + 1u), x - 1.0, y, z - 1.0), u);
  const float x11 =
    NoiseLerp(NoiseGrad(Permutation(ab + 1u), x, y - 1.0, z - 1.0), NoiseGrad(Permutation(bb + 1u), x - 1.0, y - 1.0, z - 1.0), u);

  return NoiseLerp(NoiseLerp(x00, x10, v), NoiseLerp(x01, x11, v), w) * 0.5;
}

// ------------------------------------------------------------------------------------------------
// CubeSphere, from CubeSphere.h. The degree-eleven odd polynomial is the same six coefficients in
// the same Horner order, so the warp is bit-identical; the normalise uses the hardware's sqrt where
// the CPU uses a Newton iteration in double, which is where a baked direction can differ by a ULP.

float WarpTan(float _t)
{
  const float t2 = _t * _t;
  float p = 0.0220568028;
  p = p * t2 + 0.0097162480;
  p = p * t2 + 0.0589079198;
  p = p * t2 + 0.1323876022;
  p = p * t2 + 0.3334098470;
  p = p * t2 + 0.9999982415;
  return p * _t;
}

float Warp(float _s)
{
  // The ends are exactly plus and minus one and not the polynomial's answer for them: an edge sample
  // has to land on the cube's edge to the last bit or the two faces that share it disagree.
  const float warped = WarpTan(_s * PI_OVER_4);
  return (_s >= 1.0) ? 1.0 : ((_s <= -1.0) ? -1.0 : warped);
}

float InFace(uint _index, uint _samplesPerSide)
{
  const float span = float(_samplesPerSide - 1u);
  return (2.0 * float(_index) - span) / span;
}

float3 Direction(uint _face, uint _x, uint _z, uint _samplesPerSide)
{
  const float u = Warp(InFace(_x, _samplesPerSide));
  const float v = Warp(InFace(_z, _samplesPerSide));

  float3 cube;
  if (_face == 0u)
    cube = float3(1.0, v, u);
  else if (_face == 1u)
    cube = float3(-1.0, v, u);
  else if (_face == 2u)
    cube = float3(u, 1.0, v);
  else if (_face == 3u)
    cube = float3(u, -1.0, v);
  else if (_face == 4u)
    cube = float3(u, v, 1.0);
  else
    cube = float3(u, v, -1.0);

  return cube / sqrt(cube.x * cube.x + cube.y * cube.y + cube.z * cube.z);
}

// ------------------------------------------------------------------------------------------------
// The field, from BodyField.cpp. Everything here reads the block and the two maxima the reduction
// found; nothing draws a random number.

float Smoothstep(float _from, float _to, float _value)
{
  if (_to <= _from)
    return (_value >= _to) ? 1.0 : 0.0;

  const float t = clamp((_value - _from) / (_to - _from), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

float CapDistance(float3 _d, float4 _centreHalfWidth)
{
  // A cap with no width holds nothing, and two half widths is past any cap's edge.
  float away = 2.0;
  if (_centreHalfWidth.w > 0.0)
    away = acos(clamp(dot(_d, _centreHalfWidth.xyz), -1.0, 1.0)) / _centreHalfWidth.w;

  return away;
}

float CapFade(float3 _d, BakeTile _tile)
{
  const float away = CapDistance(_d, _tile.centreHalfWidth);
  const float edge = _tile.edgeDesiredPosYRidged.x;
  const float inner = 1.0 - edge;

  float fade = 0.0;
  if (away < 1.0)
  {
    // edge is above zero in the else: away is below 1 and above inner, which cannot both hold at
    // edge = 0.
    if (away <= inner)
      fade = 1.0;
    else
    {
      const float t = (1.0 - away) / edge;
      fade = t * t * (3.0 - 2.0 * t);
    }
  }

  return fade;
}

float OctaveAmplitude(uint _tile, uint _octave)
{
  const float4 group = octaveAmplitude[_tile][_octave >> 2u];
  const uint within = _octave & 3u;
  return (within == 0u) ? group.x : ((within == 1u) ? group.y : ((within == 2u) ? group.z : group.w));
}

// The tile's raw octaves, before the rescale the reduction's maximum decides. This is the one the
// first dispatch maximises and the second dispatch multiplies.
float Octaves(float3 _d, uint _tileIndex)
{
  const BakeTile tile = tiles[_tileIndex];
  const uint octaves = uint(outsideMaxHeightGrid.w);
  const bool ridged = tile.edgeDesiredPosYRidged.w != 0.0;
  const float heightScale = tile.fractal.y;
  const float smoothing = tile.fractal.z;

  float height = 0.0;
  float frequency = OCTAVE_BASE_FREQUENCY;
  for (uint octave = 0u; octave < octaves; ++octave)
  {
    float octaveValue = NoiseSample(_d * frequency + seedOffset.xyz);
    if (ridged && octave < RIDGED_OCTAVES)
      octaveValue = 0.5 - abs(octaveValue);

    octaveValue *= OctaveAmplitude(_tileIndex, octave);

    // How far up this tile's own range the terrain has come. See BodyField.cpp for why it is
    // measured against the tile's own height scale and not in the source's map units.
    const float relative = (heightScale > 0.0) ? min(abs(height) / heightScale, 1.0) : 0.0;
    octaveValue *= ROUGHNESS_FLOOR + pow(relative, smoothing) * ROUGHNESS_GAIN;

    height += octaveValue;
    frequency *= 2.0;
  }

  return height;
}

float Lumpiness(float3 _d)
{
  const float lumpiness = seedOffset.w;

  float height = 0.0;
  if (lumpiness != 0.0)
  {
    float amplitude = lumpiness;
    float frequency = LUMP_BASE_FREQUENCY;
    for (uint octave = 0u; octave < LUMP_OCTAVES; ++octave)
    {
      height += NoiseSample(_d * frequency + seedOffset.xyz + LUMP_OFFSET) * amplitude;
      amplitude *= 0.5;
      frequency *= 2.0;
    }
  }

  return height;
}

// _tileScale is what the first dispatch's maxima decide: desiredHeight over the tile's own measured
// peak, or zero for a tile that never rose above the water anywhere inside its cap.
float Field(float3 _d, float _tileScale[BAKE_MAX_TILES])
{
  const float lump = Lumpiness(_d);

  float height = outsideMaxHeightGrid.x + lump;
  for (uint i = 0u; i < counts.x; ++i)
  {
    const float fade = CapFade(_d, tiles[i]);
    if (fade <= 0.0)
      continue;

    const float tileHeight = (Octaves(_d, i) * _tileScale[i] + tiles[i].edgeDesiredPosYRidged.z) * fade + lump;
    height = max(height, tileHeight);
  }

  return height;
}

// A unit vector perpendicular to _d, from the axis _d leans on least, as BodyField.cpp's helper does.
float3 Perpendicular(float3 _d)
{
  const float ax = abs(_d.x);
  const float ay = abs(_d.y);
  const float az = abs(_d.z);
  const float3 axis = (ax <= ay && ax <= az) ? float3(1.0, 0.0, 0.0) : ((ay <= az) ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0));
  return normalize(cross(_d, axis));
}

float Flattened(float3 _d, float _tileScale[BAKE_MAX_TILES])
{
  float height = Field(_d, _tileScale);

  for (uint i = 0u; i < counts.y; ++i)
  {
    const BakeFlatten area = flatten[i];
    const float away = CapDistance(_d, area.centreHalfWidth);
    if (away >= 1.0)
      continue;

    const float value = area.modeValueThreshold.y;
    const uint mode = uint(area.modeValueThreshold.x);
    if (mode == 0u) // Absolute
    {
      height = value;
    }
    else if (mode == 1u) // Add
    {
      height += value;
    }
    else if (mode == 2u) // Subtract: a bowl, full depth at the centre and nothing at the rim
    {
      height -= value * (1.0 - away * away);
    }
    else if (mode == 3u) // Subtract2
    {
      if (height > area.modeValueThreshold.z)
        height -= value;
    }
    else // Smooth: the mean of the field a quarter of the cap away in four directions
    {
      const float3 first = Perpendicular(_d);
      const float3 second = cross(_d, first);
      const float angle = area.centreHalfWidth.w * 0.25;
      const float cosine = cos(angle);
      const float sine = sin(angle);

      float sum = 0.0;
      for (int which = 0; which < 4; ++which)
      {
        const float3 tangent = (which == 0) ? first : ((which == 1) ? -first : ((which == 2) ? second : -second));
        sum += Field(normalize(_d * cosine + tangent * sine), _tileScale);
      }

      height = sum * 0.25;
    }
  }

  return height;
}

// The polar cap, from BodyField.cpp. lat = asin(dot(d, axis)) and then |sin lat| cancel, so neither
// is evaluated; the ragged edge is added to the latitude inside the smoothstep and not to the cap
// value after it, so a body with no cap at its equator has nothing at its equator.
float PolarCap(float3 _d)
{
  const float strength = polar.x;
  if (strength == 0.0)
    return 0.0;

  const float sine = abs(dot(_d, spinAxis.xyz));

  Pcg32State rng = Pcg32Seed(DirectionSeed(uint2(counts.z, counts.w), _d));
  const float dithered = sine + Pcg32Signed(rng, polar.z);

  return Smoothstep(polar.y, 1.0, dithered);
}

// In metres, with the multiply by radiusMetres last, as the CPU has it.
float Height(float3 _d, float _tileScale[BAKE_MAX_TILES], float _maxHeightMetres)
{
  float height = Flattened(_d, _tileScale);

  const float geometry = polar.w;
  if (geometry > 0.0)
  {
    const float radius = radiusEllipsoid.x;
    const float maxHeightFraction = (radius > 0.0) ? (_maxHeightMetres / radius) : 0.0;
    height += geometry * polar.x * maxHeightFraction * PolarCap(_d);
  }

  return height * radiusEllipsoid.x;
}

float Climate(float3 _d, float _heightMetres, float _maxHeightMetres)
{
  return _heightMetres + polar.x * _maxHeightMetres * PolarCap(_d);
}

// P = d * ellipsoid * (R + h), from BodyMeshBuilder.cpp.
float3 Place(float3 _direction, float _heightMetres)
{
  const float radius = radiusEllipsoid.x + _heightMetres;
  return float3(_direction.x * radiusEllipsoid.y * radius, _direction.y * radiusEllipsoid.z * radius,
                _direction.z * radiusEllipsoid.w * radius);
}

// ------------------------------------------------------------------------------------------------
// The float maxima, reduced with InterlockedMax on an order-preserving uint image of each float.
//
// Positive floats order as their bit patterns and negatives order backwards; this flips the sign bit
// on a positive and every bit on a negative, which makes the whole range order as unsigned. Get it
// wrong and a tile's maximum comes back as its most negative sample, which rescales the continent
// upside down and reads as a noise bug rather than as a reduction bug.
uint OrderedBits(float _value)
{
  const uint bits = asuint(_value);
  return bits ^ ((uint(asint(_value) >> 31)) | 0x80000000u);
}

float FromOrderedBits(uint _ordered)
{
  const uint bits = _ordered ^ (((_ordered >> 31u) - 1u) | 0x80000000u);
  return asfloat(bits);
}

// ------------------------------------------------------------------------------------------------
// The resources both kernels bind, and the two things they read out of the reduction.

// FxVertex to the byte (FxVertex.h, Decisions/0019): a float position, then the normal, colour and
// uv packed into four words. A structured buffer packs scalars tightly, so this is 28 bytes, and
// FxVertex.h's static_assert on 28 is the other half of that.
struct FxVertexGpu
{
  float px, py, pz;
  uint2 normal; // R16G16B16A16_SNORM: x and y in the first word, z and the unused w in the second
  uint colour;  // R8G8B8A8_UNORM
  uint uv;      // R16G16_FLOAT
};

// The three rounding rules FxVertex.h spells, in the intrinsics that implement them: round() takes
// halves away from zero, which on a saturated UNORM is the C++'s half-up, and f32tof16 is IEEE
// round-to-nearest-even on every D3D12 device. Get one of these wrong and the readback comparison
// against the CPU builder is off by a byte on the vertices where it matters least, which is the
// hardest kind of difference to read.
uint PackUnorm8x4(float4 _colour)
{
  const uint4 quantised = uint4(round(saturate(_colour) * 255.0));
  return quantised.x | (quantised.y << 8) | (quantised.z << 16) | (quantised.w << 24);
}

uint2 PackSnorm16x4(float3 _normal)
{
  const int3 quantised = int3(round(clamp(_normal, -1.0, 1.0) * 32767.0));
  return uint2((uint(quantised.x) & 0xffffu) | (uint(quantised.y) << 16), uint(quantised.z) & 0xffffu);
}

uint PackHalf2(float2 _uv)
{
  return f32tof16(_uv.x) | (f32tof16(_uv.y) << 16);
}

FxVertexGpu MakeVertex(float3 _position, float3 _normal, float4 _colour, float2 _uv)
{
  FxVertexGpu vertex;
  vertex.px = _position.x;
  vertex.py = _position.y;
  vertex.pz = _position.z;
  vertex.normal = PackSnorm16x4(_normal);
  vertex.colour = PackUnorm8x4(_colour);
  vertex.uv = PackHalf2(_uv);
  return vertex;
}

RWStructuredBuffer<FxVertexGpu> Out : register(u0);
RWBuffer<uint> Maxima : register(u1);
Texture2D Ramp : register(t0);
SamplerState RampSampler : register(s0);

// x selects the pass: 0 is the tile maxima, 1 is the height maximum. Two passes and not one, because
// the height maximum is measured over the field *after* the tiles are rescaled, so it cannot be
// found in the same dispatch that finds what to rescale them by. The work order asked for one; it
// would have read the tile maxima of whichever threads happened to have run.
cbuffer BakeControl : register(b1)
{
  uint4 control;
};

void LoadTileScales(out float _scales[BAKE_MAX_TILES])
{
  for (uint i = 0u; i < BAKE_MAX_TILES; ++i)
  {
    const float peak = FromOrderedBits(Maxima[i]);
    _scales[i] = (peak > 0.0) ? (tiles[i].edgeDesiredPosYRidged.y / peak) : 0.0;
  }
}

float MaxHeightMetres()
{
  // A maxHeight the block already carries wins over the reduction's, as MeasureMaxHeight does.
  const float unitHeight =
      (outsideMaxHeightGrid.y != 0.0) ? outsideMaxHeightGrid.y : FromOrderedBits(Maxima[BAKE_MAX_TILES]);
  return unitHeight * radiusEllipsoid.x;
}

uint SamplesPerSide()
{
  return (1u << uint(outsideMaxHeightGrid.z)) + 1u;
}

// The colour of one triangle, from BodyMeshBuilder.cpp's per-triangle block.
float3 TriangleColour(float3 _normal, float3 _centroid, float _heightMetres, float _maxHeightMetres, uint _cellHash)
{
  const float climateScale = (_maxHeightMetres > 0.0) ? (1.0 / _maxHeightMetres) : 0.0;
  const float sourceUnits = (radiusEllipsoid.x > 0.0) ? (SOURCE_MAP_SIZE / radiusEllipsoid.x) : 0.0;
  const float climate = Climate(_centroid, _heightMetres, _maxHeightMetres);

  const float gradient = clamp(dot(_normal, _centroid), 0.0, 1.0);
  const float u = pow(1.0 - gradient, SLOPE_EXPONENT);
  float v = 1.0 - climate * climateScale;

  Pcg32State rng = Pcg32Seed(uint2(_cellHash, 0u));
  v += Pcg32Signed(rng, DITHER_STRENGTH / (abs(climate * sourceUnits) + DITHER_SOFTENING));

  // Half a texel, and it is not a nicety. ColourRamp::Sample clamps to [0, 1] and multiplies by 63
  // to land on a texel index; a texture sample multiplies by 64 and subtracts half a texel. Sampling
  // the obvious float2(u, v) reads a different pair of texels near both ends and a different weight
  // everywhere, which is a whole ramp row of error at the edges.
  const float2 rampUv = (clamp(float2(u, v), 0.0, 1.0) * 63.0 + 0.5) / 64.0;
  return Ramp.SampleLevel(RampSampler, rampUv, 0).rgb;
}
