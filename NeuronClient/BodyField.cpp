#include "pch.h"
#include "BodyField.h"

#include "CubeSphere.h"

using namespace DirectX;

namespace Neuron
{
namespace
{
// The source's compensatedHeightScale, verbatim (Design/PlanetRenderer.md 5.2):
//   amp = heightScale * 30.7 * e^(-6.5 * fractalDimension) * 15.353 * e^(-3.1 * lowlandSmoothing)
// Two exponentials that fall steeply, so a tenth on either parameter is a visible change in how much
// terrain there is. They are the source's numbers and are not to be rounded.
constexpr float AMPLITUDE_A = 30.7f;
constexpr float AMPLITUDE_A_RATE = -6.5f;
constexpr float AMPLITUDE_B = 15.353f;
constexpr float AMPLITUDE_B_RATE = -3.1f;

// The source's per-level feature size: len = 256 * 0.5^octave, and the octave's amplitude is
// pow(len * 10, fractalDimension). 256 is the diamond-square half-size the constants were tuned at.
constexpr float SOURCE_LEVEL_LENGTH = 256.0f;
constexpr float SOURCE_LEVEL_GAIN = 10.0f;

// The height-dependent term from GenerateNoise: 0.1 + pow(|h|, lowlandSmoothing) * 0.15. This is the
// law that makes lowlands smooth and peaks rugged, and it is most of why the source's terrain looks
// the way it does.
//
// Its one free reading had to be pinned. The design writes h in the source's map units -- a fraction
// of the radius times a 2 000-unit map -- and in that form the term is a positive feedback with no
// fixed point: the first octave lifts h to a few hundredths of the radius, which is tens of source
// units, which multiplies the second octave by seventy, and three octaves later the body is noise
// rather than terrain. Measured on the description BodyFieldTests uses, the literal form runs
// 0.014, -1.5, -156, 3.1e+04, -7.5e+06, 3.0e+08 over its six octaves -- three hundred million times
// the body's radius. The source never met that, because a diamond-square level can only displace by
// the amplitude it has left; a 3-D octave has no such bound.
//
// So h is taken relative to the tile's own target amplitude instead -- 0 in the lowlands, 1 at a peak
// of the height this tile is aiming for -- and the term stays inside [0.1, 0.25], which is the
// two-and-a-half times between smooth and rugged that the source's two constants are worth. The
// design's 5.2 records the correction.
constexpr float ROUGHNESS_FLOOR = 0.1f;
constexpr float ROUGHNESS_GAIN = 0.15f;

// The coarsest octave puts about one feature on a hemisphere, and each one after it halves that. At
// gridPower octaves the finest lands a little over two grid samples per feature, which is as fine as
// the mesh can carry; a higher base frequency would alias into the facets rather than shape them.
constexpr float OCTAVE_BASE_FREQUENCY = 0.5f;

// Only the two coarsest octaves are ridged, which is what stands in for the source's generationMethod
// 1 and 2: ridging the fine octaves turns a mountain range into gravel.
constexpr std::uint32_t RIDGED_OCTAVES = 2;

// The whole-body deformation an asteroid is lumpy with, applied before the tiles: three octaves from
// one cycle over the body, each half the amplitude of the last. The offset keeps the lumps from
// being the same function as the terrain that sits on them.
constexpr std::uint32_t LUMP_OCTAVES = 3;
constexpr float LUMP_BASE_FREQUENCY = 1.0f;
constexpr float LUMP_OFFSET = 97.0f;

// The noise repeats every PERMUTATION_SIZE units on each axis, so an offset drawn across one period
// reaches every distinct region of it and no body can accidentally share another's terrain.
constexpr float SEED_OFFSET_RANGE = static_cast<float>(Noise3::PERMUTATION_SIZE);

// The cap-edge dither is a pure function of direction, so it is seeded from the direction: quantised
// to this many cells per unit, which is several cells across a triangle at any grid power the tree
// builds, so neighbouring triangles get different draws and one triangle always gets the same one.
constexpr float CLIMATE_DITHER_CELLS = 256.0f;

// Every transcendental in the field goes through these two. Two vendors' float pow and exp differ in
// the last bit; their double results differ in the last bit of a double, which is twenty-nine bits
// below the first bit a float can tell apart, so the narrowed answer is the same float on every
// machine. That is what makes the pinned height in BodyFieldTests a replay key rather than one
// machine's opinion (Design/PlanetRenderer.md 10).
[[nodiscard]] float Pow(float _base, float _exponent) noexcept
{
  return static_cast<float>(std::pow(static_cast<double>(_base), static_cast<double>(_exponent)));
}

[[nodiscard]] float Exp(float _value) noexcept
{
  return static_cast<float>(std::exp(static_cast<double>(_value)));
}

[[nodiscard]] float Acos(float _value) noexcept
{
  return static_cast<float>(std::acos(static_cast<double>(std::clamp(_value, -1.0f, 1.0f))));
}

[[nodiscard]] float Dot(const XMFLOAT3& _a, const XMFLOAT3& _b) noexcept
{
  return _a.x * _b.x + _a.y * _b.y + _a.z * _b.z;
}

[[nodiscard]] float Dot(const XMFLOAT3& _a, const XMFLOAT4& _b) noexcept
{
  return _a.x * _b.x + _a.y * _b.y + _a.z * _b.z;
}

[[nodiscard]] float Smoothstep(float _from, float _to, float _value) noexcept
{
  if (_to <= _from)
    return (_value >= _to) ? 1.0f : 0.0f;

  const float t = std::clamp((_value - _from) / (_to - _from), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// The angular distance from a cap's centre, over its half width: below 1 is inside the cap.
[[nodiscard]] float CapDistance(const XMFLOAT3& _d, const XMFLOAT4& _centreHalfWidth) noexcept
{
  if (_centreHalfWidth.w <= 0.0f)
    return 2.0f;

  return Acos(Dot(_d, _centreHalfWidth)) / _centreHalfWidth.w;
}

// splitmix64's finaliser, over the seed and the quantised direction. Its job is only that two
// neighbouring cells get unrelated draws out of Pcg32, which this does and an addition does not.
[[nodiscard]] std::uint64_t Mix(std::uint64_t _value) noexcept
{
  _value ^= _value >> 30;
  _value *= 0xbf58476d1ce4e5b9ull;
  _value ^= _value >> 27;
  _value *= 0x94d049bb133111ebull;
  return _value ^ (_value >> 31);
}

[[nodiscard]] std::uint64_t DirectionSeed(std::uint64_t _seed, const XMFLOAT3& _d) noexcept
{
  const auto cell = [](float _v) noexcept
  { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(_v * CLIMATE_DITHER_CELLS + 0.5f))); };

  std::uint64_t hash = Mix(_seed);
  hash = Mix(hash ^ cell(_d.x));
  hash = Mix(hash ^ cell(_d.y));
  return Mix(hash ^ cell(_d.z));
}

// A unit vector perpendicular to _d, taken from whichever axis _d leans on least so the cross
// product never collapses.
[[nodiscard]] XMVECTOR Perpendicular(FXMVECTOR _d) noexcept
{
  XMFLOAT3 direction;
  XMStoreFloat3(&direction, _d);

  const float ax = std::fabs(direction.x);
  const float ay = std::fabs(direction.y);
  const float az = std::fabs(direction.z);
  const XMVECTOR axis = (ax <= ay && ax <= az) ? g_XMIdentityR0.v : ((ay <= az) ? g_XMIdentityR1.v : g_XMIdentityR2.v);
  return XMVector3Normalize(XMVector3Cross(_d, axis));
}
} // namespace

BodyField::BodyField(const BodyDesc& _desc)
  : m_params(FlattenDesc(_desc)),
    m_noise(std::span<const std::uint32_t, Noise3::PERMUTATION_SIZE>(m_params.permutation))
{
  // Third and fourth in the construction order, after the seed offset and the permutation: the tiles
  // are rescaled to their desired heights, and only then can the field's own maximum be measured.
  MeasureTiles();
  MeasureMaxHeight();
}

BodyParams BodyField::FlattenDesc(const BodyDesc& _desc)
{
  BodyParams params;
  Pcg32 rng(_desc.seed);

  // The draw order is the determinism guarantee (Design/PlanetRenderer.md 10), and it is this: the
  // seed offset, then the noise permutation, and nothing else. The tile and flatten centres are not
  // drawn here -- the catalogue that wrote the description drew them from its own generator. Insert
  // a draw into the middle of this and every body in the game becomes a different body.
  //
  // Three statements and not three arguments: the order a compiler evaluates a function's arguments
  // in is unspecified, so three draws inside one XMFLOAT4 is three draws in whichever order the
  // compiler felt like. This is measured rather than feared -- gcc and clang built two different
  // worlds out of one description until the draws were named.
  const float offsetX = rng.Float01() * SEED_OFFSET_RANGE;
  const float offsetY = rng.Float01() * SEED_OFFSET_RANGE;
  const float offsetZ = rng.Float01() * SEED_OFFSET_RANGE;
  params.seedOffset = XMFLOAT4(offsetX, offsetY, offsetZ, _desc.lumpiness);

  // Shuffled through a Noise3 rather than in a second Fisher-Yates written out here, so there is one
  // shuffle in the tree and the block and the noise cannot drift apart.
  const Noise3 noise(rng);
  const std::span<const std::uint32_t, Noise3::PERMUTATION_SIZE> permutation = noise.Permutation();
  for (std::uint32_t i = 0; i < Noise3::PERMUTATION_SIZE; ++i)
    params.permutation[i] = permutation[i];

  std::uint32_t gridPower = std::clamp(_desc.gridPower, BodyField::MIN_GRID_POWER, BodyField::MAX_GRID_POWER);
  if (gridPower != _desc.gridPower)
    DebugTrace("body {}: grid power {} is outside [{}, {}] and was clamped to {}\n", _desc.seed, _desc.gridPower, BodyField::MIN_GRID_POWER,
               BodyField::MAX_GRID_POWER, gridPower);

  params.seedLow = static_cast<std::uint32_t>(_desc.seed & 0xffffffffull);
  params.seedHigh = static_cast<std::uint32_t>(_desc.seed >> 32);
  params.radiusEllipsoid = XMFLOAT4(_desc.radiusMetres, _desc.ellipsoid.x, _desc.ellipsoid.y, _desc.ellipsoid.z);
  params.outsideMaxHeightGrid =
    XMFLOAT4(_desc.outsideHeight, _desc.maxHeight, static_cast<float>(gridPower), static_cast<float>(gridPower));
  params.polar = XMFLOAT4(_desc.polarStrength, _desc.capStart, _desc.capNoise, _desc.polarGeometry);
  params.spinAxis = XMFLOAT4(_desc.spinAxis.x, _desc.spinAxis.y, _desc.spinAxis.z, 0.0f);

  // Clipped and traced, never truncated in silence: a catalogue that asks for nine continents has a
  // bug in it, and the ninth one going missing without a word is how that bug survives a review.
  if (_desc.tiles.size() > BodyParams::MAX_TILES)
    DebugTrace("body {}: {} tiles, of which {} fit; the rest are dropped\n", _desc.seed, _desc.tiles.size(), BodyParams::MAX_TILES);
  if (_desc.flatten.size() > BodyParams::MAX_FLATTEN)
    DebugTrace("body {}: {} flatten areas, of which {} fit; the rest are dropped\n", _desc.seed, _desc.flatten.size(),
               BodyParams::MAX_FLATTEN);

  params.tileCount = static_cast<std::uint32_t>(std::min<std::size_t>(_desc.tiles.size(), BodyParams::MAX_TILES));
  for (std::uint32_t i = 0; i < params.tileCount; ++i)
  {
    const BodyTile& tile = _desc.tiles[i];
    BodyParams::Tile& out = params.tiles[i];

    // A centre that is not a unit direction would make every angle from it wrong, and a catalogue
    // building one out of three random numbers is the likely way in, so it is normalised here.
    XMFLOAT3 centre;
    XMStoreFloat3(&centre, XMVector3Normalize(XMLoadFloat3(&tile.centre)));

    out.centreHalfWidth = XMFLOAT4(centre.x, centre.y, centre.z, tile.halfWidthRad);
    out.edgeDesiredPosYRidged =
      XMFLOAT4(std::clamp(tile.edgeFraction, 0.0f, 1.0f), tile.desiredHeight, tile.posY, tile.ridged ? 1.0f : 0.0f);

    const float amplitude = tile.heightScale * AMPLITUDE_A * Exp(AMPLITUDE_A_RATE * tile.fractalDimension) * AMPLITUDE_B *
                            Exp(AMPLITUDE_B_RATE * tile.lowlandSmoothing);
    out.fractal = XMFLOAT4(tile.fractalDimension, tile.heightScale, tile.lowlandSmoothing, amplitude);
  }

  params.flattenCount = static_cast<std::uint32_t>(std::min<std::size_t>(_desc.flatten.size(), BodyParams::MAX_FLATTEN));
  for (std::uint32_t i = 0; i < params.flattenCount; ++i)
  {
    const BodyFlatten& area = _desc.flatten[i];
    BodyParams::Flatten& out = params.flatten[i];

    XMFLOAT3 centre;
    XMStoreFloat3(&centre, XMVector3Normalize(XMLoadFloat3(&area.centre)));

    out.centreHalfWidth = XMFLOAT4(centre.x, centre.y, centre.z, area.halfWidthRad);
    out.modeValueThreshold = XMFLOAT4(static_cast<float>(area.mode), area.value, area.threshold, 0.0f);
  }

  return params;
}

void BodyField::MeasureTiles() noexcept
{
  const std::uint32_t octaves = static_cast<std::uint32_t>(m_params.outsideMaxHeightGrid.w);

  for (std::uint32_t i = 0; i < m_params.tileCount; ++i)
  {
    const BodyParams::Tile& tile = m_params.tiles[i];
    float length = SOURCE_LEVEL_LENGTH;
    for (std::uint32_t octave = 0; octave < octaves; ++octave)
    {
      m_octaveAmplitude[i][octave] = Pow(length * SOURCE_LEVEL_GAIN, tile.fractal.x) * tile.fractal.w;
      length *= 0.5f;
    }
  }

  // The tile's maximum is measured with its edge fade already applied, because the fade is part of
  // what the tile contributes; scaling by a maximum found without it would leave a tile whose peak is
  // on the rim short of its desired height. This is the reduction Design/PlanetRenderer.md 17.1
  // names -- on the CPU it is a loop over the same grid the mesh will be built on.
  float peak[BodyParams::MAX_TILES] = {};
  const std::uint32_t samples = CubeSphere::SamplesPerSide(static_cast<std::uint32_t>(m_params.outsideMaxHeightGrid.z));
  for (std::uint32_t face = 0; face < CUBE_FACE_COUNT; ++face)
  {
    for (std::uint32_t x = 0; x < samples; ++x)
    {
      for (std::uint32_t z = 0; z < samples; ++z)
      {
        const XMFLOAT3 direction = CubeSphere::Direction(static_cast<CubeFace>(face), x, z, samples);
        for (std::uint32_t i = 0; i < m_params.tileCount; ++i)
        {
          const float fade = CapFade(direction, m_params.tiles[i]);
          if (fade <= 0.0f)
            continue;

          peak[i] = std::max(peak[i], Octaves(direction, i) * fade);
        }
      }
    }
  }

  for (std::uint32_t i = 0; i < m_params.tileCount; ++i)
  {
    // A tile whose octaves never rise above zero anywhere inside its cap is flat, and there is
    // nothing to rescale; it keeps whatever posY lifted it to.
    m_tileScale[i] = (peak[i] > 0.0f) ? (m_params.tiles[i].edgeDesiredPosYRidged.y / peak[i]) : 0.0f;
  }
}

void BodyField::MeasureMaxHeight() noexcept
{
  if (m_params.outsideMaxHeightGrid.y != 0.0f)
  {
    m_maxHeight = m_params.outsideMaxHeightGrid.y * m_params.radiusEllipsoid.x;
    return;
  }

  // Measured over the field before the polar lift, and deliberately so: the lift is a fraction of
  // this number, and measuring it after would be a definition of maxHeight in terms of itself.
  //
  // outsideHeight is where the search starts because it is the field's own floor: Field begins there
  // and only ever merges upward, so no sample can be below it before the flatten areas run.
  float peak = m_params.outsideMaxHeightGrid.x;
  const std::uint32_t samples = CubeSphere::SamplesPerSide(static_cast<std::uint32_t>(m_params.outsideMaxHeightGrid.z));
  for (std::uint32_t face = 0; face < CUBE_FACE_COUNT; ++face)
  {
    for (std::uint32_t x = 0; x < samples; ++x)
    {
      for (std::uint32_t z = 0; z < samples; ++z)
        peak = std::max(peak, Flattened(CubeSphere::Direction(static_cast<CubeFace>(face), x, z, samples)));
    }
  }

  m_maxHeight = peak * m_params.radiusEllipsoid.x;
}

float BodyField::Octaves(const XMFLOAT3& _d, std::uint32_t _tile) const noexcept
{
  const BodyParams::Tile& tile = m_params.tiles[_tile];
  const std::uint32_t octaves = static_cast<std::uint32_t>(m_params.outsideMaxHeightGrid.w);
  const bool ridged = tile.edgeDesiredPosYRidged.w != 0.0f;
  const float heightScale = tile.fractal.y;
  const float smoothing = tile.fractal.z;

  float height = 0.0f;
  float frequency = OCTAVE_BASE_FREQUENCY;
  for (std::uint32_t octave = 0; octave < octaves; ++octave)
  {
    float noise = m_noise.Sample(_d.x * frequency + m_params.seedOffset.x, _d.y * frequency + m_params.seedOffset.y,
                                 _d.z * frequency + m_params.seedOffset.z);
    if (ridged && octave < RIDGED_OCTAVES)
      noise = 0.5f - std::fabs(noise);

    noise *= m_octaveAmplitude[_tile][octave];

    // How far up this tile's own range the terrain has come: 0 in the lowlands, 1 at a peak of the
    // height it is aiming for. See ROUGHNESS_FLOOR above for why it is measured that way.
    const float relative = (heightScale > 0.0f) ? std::min(std::fabs(height) / heightScale, 1.0f) : 0.0f;
    noise *= ROUGHNESS_FLOOR + Pow(relative, smoothing) * ROUGHNESS_GAIN;

    height += noise;
    frequency *= 2.0f;
  }

  return height;
}

float BodyField::Lumpiness(const XMFLOAT3& _d) const noexcept
{
  const float lumpiness = m_params.seedOffset.w;
  if (lumpiness == 0.0f)
    return 0.0f;

  float height = 0.0f;
  float amplitude = lumpiness;
  float frequency = LUMP_BASE_FREQUENCY;
  for (std::uint32_t octave = 0; octave < LUMP_OCTAVES; ++octave)
  {
    height += m_noise.Sample(_d.x * frequency + m_params.seedOffset.x + LUMP_OFFSET, _d.y * frequency + m_params.seedOffset.y + LUMP_OFFSET,
                             _d.z * frequency + m_params.seedOffset.z + LUMP_OFFSET) *
              amplitude;
    amplitude *= 0.5f;
    frequency *= 2.0f;
  }

  return height;
}

float BodyField::CapFade(const XMFLOAT3& _d, const BodyParams::Tile& _tile) const noexcept
{
  const float distance = CapDistance(_d, _tile.centreHalfWidth);
  if (distance >= 1.0f)
    return 0.0f;

  // Softened over the outer edgeFraction of the cap, or a continent ends in a vertical wall.
  const float edge = _tile.edgeDesiredPosYRidged.x;
  const float inner = 1.0f - edge;
  if (distance <= inner)
    return 1.0f;

  // edge is above zero here: distance is below 1 and above inner, which cannot both hold at edge = 0.
  const float t = (1.0f - distance) / edge;
  return t * t * (3.0f - 2.0f * t);
}

float BodyField::Field(const XMFLOAT3& _d) const noexcept
{
  const float lump = Lumpiness(_d);

  // Outside every tile the body is at outsideHeight -- below zero for an ocean world, at or above it
  // for a dry one -- and a tile is merged in by taking whichever is higher, which is the source's
  // MergeTileIntoLandscape exactly.
  float height = m_params.outsideMaxHeightGrid.x + lump;
  for (std::uint32_t i = 0; i < m_params.tileCount; ++i)
  {
    const float fade = CapFade(_d, m_params.tiles[i]);
    if (fade <= 0.0f)
      continue;

    const BodyParams::Tile& tile = m_params.tiles[i];
    const float tileHeight = (Octaves(_d, i) * m_tileScale[i] + tile.edgeDesiredPosYRidged.z) * fade + lump;
    height = std::max(height, tileHeight);
  }

  return height;
}

float BodyField::Flattened(const XMFLOAT3& _d) const noexcept
{
  float height = Field(_d);

  // In list order, as the source applied them: a pad dug into a crater is the author's decision and
  // the order they wrote is what says which won.
  for (std::uint32_t i = 0; i < m_params.flattenCount; ++i)
  {
    const BodyParams::Flatten& area = m_params.flatten[i];
    const float distance = CapDistance(_d, area.centreHalfWidth);
    if (distance >= 1.0f)
      continue;

    const float value = area.modeValueThreshold.y;
    switch (static_cast<FlattenMode>(static_cast<std::uint8_t>(area.modeValueThreshold.x)))
    {
    case FlattenMode::Absolute:
      height = value;
      break;
    case FlattenMode::Add:
      height += value;
      break;
    case FlattenMode::Subtract:
      // A bowl rather than a step: full depth at the centre, nothing at the rim. This is the crater
      // an asteroid is covered in.
      height -= value * (1.0f - distance * distance);
      break;
    case FlattenMode::Subtract2:
      if (height > area.modeValueThreshold.z)
        height -= value;
      break;
    case FlattenMode::Smooth:
    {
      // The mean of the field a quarter of the cap away in four directions -- the source's five
      // samples, over a cap. The four are taken from the field *before* any flatten area, so a
      // Smooth cannot recurse into another one; overlapping it with an earlier Subtract is a
      // content mistake rather than a bug here, and it costs four evaluations either way.
      const XMVECTOR direction = XMLoadFloat3(&_d);
      const XMVECTOR first = Perpendicular(direction);
      const XMVECTOR second = XMVector3Cross(direction, first);
      const float angle = area.centreHalfWidth.w * 0.25f;
      const float cosine = std::cos(angle);
      const float sine = std::sin(angle);

      float sum = 0.0f;
      for (int step = 0; step < 4; ++step)
      {
        const XMVECTOR tangent =
          (step == 0) ? first : ((step == 1) ? XMVectorNegate(first) : ((step == 2) ? second : XMVectorNegate(second)));
        XMFLOAT3 sample;
        XMStoreFloat3(&sample, XMVector3Normalize(XMVectorAdd(XMVectorScale(direction, cosine), XMVectorScale(tangent, sine))));
        sum += Field(sample);
      }

      height = sum * 0.25f;
      break;
    }
    }
  }

  return height;
}

float BodyField::PolarCap(const XMFLOAT3& _d) const noexcept
{
  const float strength = m_params.polar.x;
  if (strength == 0.0f)
    return 0.0f;

  // The design writes this as lat = asin(dot(d, axis)) and then |sin lat|; the two cancel, so
  // neither is evaluated. Even in the latitude, so both poles come out of one expression.
  const float sine = std::fabs(Dot(_d, m_params.spinAxis));

  // The design adds the ragged-edge draw to the cap value; it is added to the latitude instead,
  // inside the smoothstep, so the raggedness stays *on the cap's edge*. Added afterwards it would
  // speckle the equator and the pole, where there is no edge to be ragged, and Climate would stop
  // equalling Height at the equator of a body that has no cap there. What is visible at the cap edge
  // is the same.
  const std::uint64_t seed = (static_cast<std::uint64_t>(m_params.seedHigh) << 32) | m_params.seedLow;
  Pcg32 rng(DirectionSeed(seed, _d));
  const float dithered = sine + rng.Signed(m_params.polar.z);

  return Smoothstep(m_params.polar.y, 1.0f, dithered);
}

float BodyField::Height(const XMFLOAT3& _d) const noexcept
{
  float height = Flattened(_d);

  // Caps with thickness as well as colour, when a class asks for them. Expressed as a fraction of
  // the radius like everything else, so that the multiply by radiusMetres stays the last thing that
  // happens to a height.
  const float geometry = m_params.polar.w;
  if (geometry > 0.0f)
  {
    const float radius = m_params.radiusEllipsoid.x;
    const float maxHeightFraction = (radius > 0.0f) ? (m_maxHeight / radius) : 0.0f;
    height += geometry * m_params.polar.x * maxHeightFraction * PolarCap(_d);
  }

  return height * m_params.radiusEllipsoid.x;
}

float BodyField::Climate(const XMFLOAT3& _d, float _height) const noexcept
{
  return _height + m_params.polar.x * m_maxHeight * PolarCap(_d);
}

float BodyField::MaxHeight() const noexcept
{
  return m_maxHeight;
}

const BodyParams& BodyField::Params() const noexcept
{
  return m_params;
}
} // namespace Neuron
