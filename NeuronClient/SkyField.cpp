#include "pch.h"
#include "SkyField.h"

#include "Noise3.h"

#include "Pcg32.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace Neuron
{
namespace
{
// --- how bright a star is ------------------------------------------------------------------------
// The naked-eye magnitude range: Sirius at the top, the limit of a dark-sky eye at the bottom. The
// count of stars brighter than m goes as 10^(0.6m), so inverting that over this range is what makes
// faint stars ordinary and a first-magnitude star rare. A uniform draw over the same range produces
// a sky of identical dots, which is the single most common way a generated star field gives itself
// away (Design/Skybox.md 5.1).
constexpr float MAGNITUDE_BRIGHTEST = -1.0f;
constexpr float MAGNITUDE_FAINTEST = 6.5f;
constexpr float MAGNITUDE_COUNT_SLOPE = 0.6f;

// Flux spans a factor of a thousand over that range and a screen spans a factor of 255, so it is
// compressed rather than clipped: a star's place on the ramp is the flux ratio raised to this.
constexpr float TONE_EXPONENT = 0.42f;

// **Size carries the magnitude; brightness mostly does not**, and getting that the wrong way round is
// what a first attempt at this looks like. Mapping magnitude straight onto intensity gives a sky of
// gray smudges: the faint stars, which are almost all of them, land in a narrow band near the floor
// and are then indistinguishable from each other and from noise. Mapping it onto *size* instead --
// with intensity starting well clear of the floor so every star is a crisp dot -- is what the eye
// actually reads as magnitude, and it leaves color free to be the other axis.
//
// At a 45 degree vertical field of view over 900 rows one pixel is 8.7e-4 rad, so the faintest star
// is under three pixels across and the brightest about twenty-four. They are angles, not pixels, so a
// star does not change size when the window does.
constexpr float STAR_FLOOR_INTENSITY = 0.34f;
constexpr float STAR_MIN_HALF_ANGLE_RAD = 0.0012f;
constexpr float STAR_MAX_HALF_ANGLE_RAD = 0.0105f;

// --- what color a star is ------------------------------------------------------------------------
// The spectral mix of the stars a person can actually see, which is not the mix of stars that exist:
// the sky is full of M dwarfs and none of them is visible, while the O and B stars are a rounding
// error by count and half of what stands out. Weights sum to one.
struct SpectralClass
{
  float weight;
  float minKelvin;
  float maxKelvin;
};
constexpr SpectralClass SPECTRAL_CLASSES[] = {
  {0.03f, 10000.0f, 26000.0f}, // O and B -- blue-white
  {0.12f, 7500.0f, 10000.0f},  // A
  {0.17f, 6000.0f, 7500.0f},   // F
  {0.23f, 5200.0f, 6000.0f},   // G -- the Sun's class
  {0.31f, 3700.0f, 5200.0f},   // K
  {0.14f, 2600.0f, 3700.0f},   // M -- orange-red
};

// A bright star is a hot star far more often than chance allows, because brightness in the sky is
// luminosity as much as distance. The draw into the table above is squeezed towards its hot end in
// proportion to how bright the star already is, which is what puts the blue-white stars among the
// ones the eye picks out and leaves the ruddy ones in the faint background where they belong.
constexpr float HOT_BIAS = 0.80f;

// A faint star has less color to the eye than a bright one -- the retina's color receptors need
// light this dim does not carry -- so saturation still rises with brightness. It rises from a floor
// high enough to see, though, rather than from nothing: a sky whose stars are all white is the other
// half of the mistake the size ramp above describes, and the two together are what makes a generated
// field read as gray dust.
constexpr float STAR_MIN_SATURATION = 0.45f;
constexpr float STAR_MAX_SATURATION = 1.0f;

// --- the flare over a bright star ----------------------------------------------------------------
constexpr float BURST_SIZE_FACTOR = 5.0f;      // times the star's own half-angle
constexpr float BURST_INTENSITY_FACTOR = 0.6f; // times the star's own intensity

// --- twinkle -------------------------------------------------------------------------------------
// Scintillation is an atmosphere doing it, not the star, so a faint star twinkles more than a bright
// one: the same wobble in the wavefront moves a larger fraction of a smaller signal. The rate is a
// fraction of the maximum the frame constants carry, so the number itself lives in one place on the
// C++ side and the shader is told it (SkyRenderer::Frame).
constexpr float TWINKLE_MIN_AMOUNT = 0.04f;
constexpr float TWINKLE_MAX_AMOUNT = 0.34f;
constexpr float TWINKLE_MIN_RATE_FRACTION = 0.25f;

// --- the band, its core and its dust -------------------------------------------------------------
// Dust lanes are cut with the same gradient noise the planets are made of, sampled on the direction
// itself so a lane is a shape on the sky rather than a shape in a texture. It only bites near the
// plane, because that is where the dust is.
constexpr float DUST_FREQUENCY = 3.2f;
constexpr float DUST_CONTRAST = 1.9f;
constexpr float DUST_STRENGTH = 0.85f;

// The band is brightest towards the galactic center and thinnest away from it. Stars get this as
// density -- more of them are drawn towards the center -- and the nebulosity gets it as brightness,
// which is what the two really are: unresolved stars are a glow, resolved ones are a count.
constexpr float CORE_SHARE = 0.45f;      // share of band stars drawn towards the center rather than uniformly
constexpr float CORE_SPREAD_RAD = 1.55f; // half-width of the triangular draw that places them
constexpr float CORE_WIDTH_RAD = 0.95f;  // how fast the nebulosity's core boost falls off with longitude
constexpr float CORE_GAIN = 1.3f;

// --- nebulosity ----------------------------------------------------------------------------------
// Very dim and very large, and there are a lot of them: one cloud at this brightness is invisible and
// a hundred overlapping ones are the Milky Way. The base intensity is deliberately near the bottom of
// what an 8-bit color can carry -- CloudyGlow's own falloff supplies the gradient inside each patch,
// so the quantization lands on the patch's peak and never on its shape.
constexpr float NEBULA_MIN_HALF_ANGLE_RAD = 0.05f;
constexpr float NEBULA_MAX_HALF_ANGLE_RAD = 0.28f;
constexpr float NEBULA_BASE_INTENSITY = 0.19f;
constexpr float NEBULA_FIELD_FRACTION = 0.22f;  // share placed anywhere rather than on the band
constexpr float NEBULA_SCALE_HEIGHT_RAD = 0.2f; // the clouds spread wider than the stars do
constexpr float NEBULA_OFF_BAND_FLOOR = 0.22f;  // what is left of a cloud's brightness far from the band

// The colors dust takes: cold blue where it scatters starlight, magenta where hydrogen glows through
// it, rust where it is lit from behind, teal in between. A cloud on the band is whitened towards the
// unresolved starlight of the band itself, because that is what the band is made of.
// Spelled as a plain aggregate rather than an XMFLOAT3 because these are constexpr and
// DirectXMath's constexpr constructors are a version detail this file has no reason to depend on.
struct Colour3
{
  float r, g, b;
};
constexpr Colour3 NEBULA_COLOURS[] = {
  {0.30f, 0.44f, 0.88f},
  {0.58f, 0.34f, 0.74f},
  {0.80f, 0.50f, 0.32f},
  {0.32f, 0.62f, 0.68f},
};
constexpr Colour3 BAND_COLOUR{0.74f, 0.72f, 0.63f};
constexpr float BAND_WHITENING = 0.75f;

// The six corners of a quad, as two triangles. Winding is free: the sky pipeline culls nothing.
constexpr float CORNER_X[6] = {-1.0f, 1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
constexpr float CORNER_Y[6] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f};
constexpr std::uint32_t VERTS_PER_QUAD = 6;

// An orthonormal frame for the galaxy: the pole, the direction of the galactic center, and the third
// axis. Both inputs are taken as hints -- the center is projected off the pole and renormalized --
// so a caller can name two directions that read well without having to make them perpendicular.
struct GalacticFrame
{
  XMFLOAT3 pole;
  XMFLOAT3 towardsCentre;
  XMFLOAT3 side;
};

[[nodiscard]] GalacticFrame MakeFrame(const XMFLOAT3& _pole, const XMFLOAT3& _centre) noexcept
{
  XMVECTOR pole = XMVector3Normalize(XMLoadFloat3(&_pole));
  XMVECTOR centre = XMLoadFloat3(&_centre);
  centre = XMVectorSubtract(centre, XMVectorMultiply(pole, XMVector3Dot(pole, centre)));
  if (XMVectorGetX(XMVector3LengthSq(centre)) < 1e-8f)
    centre = XMVector3Orthogonal(pole); // the caller named the pole twice; any perpendicular will do
  centre = XMVector3Normalize(centre);

  GalacticFrame frame;
  XMStoreFloat3(&frame.pole, pole);
  XMStoreFloat3(&frame.towardsCentre, centre);
  XMStoreFloat3(&frame.side, XMVector3Cross(pole, centre));
  return frame;
}

// A direction from a galactic latitude and a longitude, longitude zero being the galactic center.
[[nodiscard]] XMFLOAT3 FrameDirection(const GalacticFrame& _frame, float _latitudeRad, float _longitudeRad) noexcept
{
  const float cosLat = std::cos(_latitudeRad);
  const XMVECTOR inPlane = XMVectorAdd(XMVectorScale(XMLoadFloat3(&_frame.towardsCentre), std::cos(_longitudeRad) * cosLat),
                                       XMVectorScale(XMLoadFloat3(&_frame.side), std::sin(_longitudeRad) * cosLat));
  XMFLOAT3 direction;
  XMStoreFloat3(&direction, XMVector3Normalize(XMVectorAdd(inPlane, XMVectorScale(XMLoadFloat3(&_frame.pole), std::sin(_latitudeRad)))));
  return direction;
}

// z = 1 - 2u is the inverse of the sphere's own area measure. Sampling the polar angle uniformly
// instead is the classic mistake and piles stars up at both poles.
[[nodiscard]] float UniformLatitude(Pcg32& _rng) noexcept
{
  return std::asin(std::clamp(1.0f - 2.0f * _rng.Float01(), -1.0f, 1.0f));
}

// Latitude with density proportional to exp(-|b| / h): the band. Drawn by inverting that directly
// rather than by rejection, so the number of draws per star is fixed and the stream is predictable.
[[nodiscard]] float BandLatitude(Pcg32& _rng, float _scaleHeightRad) noexcept
{
  const float sign = (_rng.Float01() < 0.5f) ? -1.0f : 1.0f;
  const float unit = std::min(_rng.Float01(), 0.999999f);
  return std::clamp(sign * -_scaleHeightRad * std::log(1.0f - unit), -XM_PIDIV2, XM_PIDIV2);
}

// Longitude, either uniform or squeezed towards the galactic center by a triangular draw. Triangular
// rather than uniform-on-an-interval so the density falls off instead of ending at a wall.
[[nodiscard]] float BandLongitude(Pcg32& _rng, bool _towardsCentre) noexcept
{
  if (!_towardsCentre)
    return (_rng.Float01() * 2.0f - 1.0f) * XM_PI;

  const float triangular = _rng.Float01() + _rng.Float01() - 1.0f;
  return std::clamp(triangular * CORE_SPREAD_RAD, -XM_PI, XM_PI);
}

// How much of a direction's light the dust in front of it takes away, in 0..1. One where there is no
// dust; near zero in the heart of a lane. Both the stars and the nebulosity go through this, which is
// what makes a lane read as one shape crossing both rather than as two coincidences.
[[nodiscard]] float Extinction(const Noise3& _dust, const XMFLOAT3& _direction, float _bandCloseness) noexcept
{
  const float sample = _dust.Sample(_direction.x * DUST_FREQUENCY, _direction.y * DUST_FREQUENCY, _direction.z * DUST_FREQUENCY);
  const float clear = std::clamp(0.5f + sample * DUST_CONTRAST, 0.0f, 1.0f);
  return 1.0f - DUST_STRENGTH * _bandCloseness * (1.0f - clear);
}

[[nodiscard]] float Lerp(float _from, float _to, float _t) noexcept
{
  return _from + (_to - _from) * _t;
}

void EmitQuad(std::vector<SkyVertex>& _out, const XMFLOAT3& _direction, const XMFLOAT3& _colour, float _halfAngleRad, float _rollRad,
              float _amount, float _rateFraction, float _phaseFraction)
{
  for (std::uint32_t i = 0; i < VERTS_PER_QUAD; ++i)
  {
    _out.push_back(SkyVertex::Make(_direction, XMFLOAT2(CORNER_X[i], CORNER_Y[i]), _colour, _halfAngleRad, _rollRad, _amount, _rateFraction,
                                   _phaseFraction));
  }
}

// One generated star, before it becomes six vertices. Kept because the flares are the brightest stars
// of the draw and which those are is not known until the draw is over.
struct Star
{
  XMFLOAT3 direction;
  XMFLOAT3 colour; // hue already multiplied by intensity
  float magnitude;
  float halfAngleRad;
  float intensity;
  float twinkleAmount;
  float twinkleRateFraction;
  float twinklePhaseFraction;
};
} // namespace

XMFLOAT3 SkyField::TemperatureColour(float _kelvin) noexcept
{
  // Tanner Helland's piecewise fit to the Planckian locus, which is accurate to a few percent from
  // 1 000 K to 40 000 K and is three logarithms rather than a spectral integral. Its one property
  // worth knowing is that 6 600 K comes out exactly white, which is where the two branches meet and
  // what a test pins.
  const float t = std::clamp(_kelvin, 1000.0f, 40000.0f) * 0.01f;

  float r = 255.0f;
  if (t > 66.0f)
    r = 329.698727446f * std::pow(t - 60.0f, -0.1332047592f);

  float g = 0.0f;
  if (t <= 66.0f)
    g = 99.4708025861f * std::log(t) - 161.1195681661f;
  else
    g = 288.1221695283f * std::pow(t - 60.0f, -0.0755148492f);

  float b = 255.0f;
  if (t < 66.0f)
    b = (t <= 19.0f) ? 0.0f : (138.5177312231f * std::log(t - 10.0f) - 305.0447927307f);

  r = std::clamp(r, 0.0f, 255.0f);
  g = std::clamp(g, 0.0f, 255.0f);
  b = std::clamp(b, 0.0f, 255.0f);

  // Normalized on the largest channel, not on 255: the vertex carries brightness of its own and a
  // hue that dimmed itself would count the star's magnitude twice.
  const float peak = std::max(r, std::max(g, b));
  if (peak <= 0.0f)
    return XMFLOAT3(1.0f, 1.0f, 1.0f);
  return XMFLOAT3(r / peak, g / peak, b / peak);
}

void SkyField::Build(const Desc& _desc, SkyMesh& _out)
{
  _out.verts.clear();
  for (std::uint32_t i = 0; i < SKY_LAYER_COUNT; ++i)
    _out.layerVertexCount[i] = 0;

  const std::uint32_t brightCount = std::min(_desc.brightStarCount, _desc.starCount);
  _out.verts.reserve(static_cast<std::size_t>(_desc.nebulaCount + _desc.starCount + brightCount) * VERTS_PER_QUAD);

  Pcg32 rng(_desc.seed);
  // The dust is shuffled out of the same generator, first, and costs 255 draws of it. Stated here
  // because everything after it depends on where the stream is, and a reader comparing two skies
  // needs to know the draw order is the sky's identity (Noise3.h says the same about its cost).
  const Noise3 dust(rng);
  const GalacticFrame frame = MakeFrame(_desc.galacticPole, _desc.galacticCentre);

  const float bandScaleHeight = std::max(_desc.bandScaleHeightRad, 1e-3f);
  const float fluxSpan = std::pow(10.0f, -0.4f * (MAGNITUDE_BRIGHTEST - MAGNITUDE_FAINTEST));
  const float countAtBrightest = std::pow(10.0f, MAGNITUDE_COUNT_SLOPE * MAGNITUDE_BRIGHTEST);
  const float countAtFaintest = std::pow(10.0f, MAGNITUDE_COUNT_SLOPE * MAGNITUDE_FAINTEST);

  // --- the nebulosity, first, because it is the first layer drawn ---------------------------------
  for (std::uint32_t i = 0; i < _desc.nebulaCount; ++i)
  {
    const bool onBand = rng.Float01() >= NEBULA_FIELD_FRACTION;
    const float latitude = onBand ? BandLatitude(rng, NEBULA_SCALE_HEIGHT_RAD) : UniformLatitude(rng);
    const float longitude = BandLongitude(rng, onBand && rng.Float01() < CORE_SHARE);
    const XMFLOAT3 direction = FrameDirection(frame, latitude, longitude);

    const float bandCloseness = std::exp(-std::abs(latitude) / NEBULA_SCALE_HEIGHT_RAD);
    const float coreCloseness = std::exp(-std::abs(longitude) / CORE_WIDTH_RAD);
    const float glow = NEBULA_OFF_BAND_FLOOR + (1.0f - NEBULA_OFF_BAND_FLOOR) * bandCloseness;
    const float intensity = NEBULA_BASE_INTENSITY * (0.45f + 0.55f * rng.Float01()) * glow * (1.0f + CORE_GAIN * coreCloseness) *
                            Extinction(dust, direction, bandCloseness);

    const Colour3& palette = NEBULA_COLOURS[rng.Below(static_cast<std::uint32_t>(std::size(NEBULA_COLOURS)))];
    const float whiten = bandCloseness * BAND_WHITENING;
    const XMFLOAT3 colour(Lerp(palette.r, BAND_COLOUR.r, whiten) * intensity, Lerp(palette.g, BAND_COLOUR.g, whiten) * intensity,
                          Lerp(palette.b, BAND_COLOUR.b, whiten) * intensity);

    const float halfAngle = NEBULA_MIN_HALF_ANGLE_RAD + rng.Float01() * (NEBULA_MAX_HALF_ANGLE_RAD - NEBULA_MIN_HALF_ANGLE_RAD);
    // A roll per cloud, and it is the whole reason a hundred copies of one 128-pixel picture read as
    // weather rather than as a hundred copies of one 128-pixel picture.
    EmitQuad(_out.verts, direction, colour, halfAngle, rng.Float01() * XM_2PI, 0.0f, 0.0f, 0.0f);
  }
  _out.layerVertexCount[static_cast<std::uint32_t>(SkyLayer::Nebula)] = static_cast<std::uint32_t>(_out.verts.size());

  // --- the stars ----------------------------------------------------------------------------------
  std::vector<Star> stars;
  stars.reserve(_desc.starCount);
  for (std::uint32_t i = 0; i < _desc.starCount; ++i)
  {
    const bool onBand = rng.Float01() < _desc.bandFraction;
    const float latitude = onBand ? BandLatitude(rng, bandScaleHeight) : UniformLatitude(rng);
    const float longitude = BandLongitude(rng, onBand && rng.Float01() < CORE_SHARE);

    Star star;
    star.direction = FrameDirection(frame, latitude, longitude);

    // Magnitude, by inverting the count law over the visible range.
    const float countDraw = countAtBrightest + rng.Float01() * (countAtFaintest - countAtBrightest);
    star.magnitude = std::log10(countDraw) / MAGNITUDE_COUNT_SLOPE;

    // Flux relative to the faintest star in the range, then compressed to something a screen holds.
    const float flux = std::pow(10.0f, -0.4f * (star.magnitude - MAGNITUDE_FAINTEST));
    const float luminance = std::clamp(std::pow(flux / fluxSpan, TONE_EXPONENT), 0.0f, 1.0f);

    const float bandCloseness = std::exp(-std::abs(latitude) / bandScaleHeight);
    star.intensity = (STAR_FLOOR_INTENSITY + (1.0f - STAR_FLOOR_INTENSITY) * luminance) * Extinction(dust, star.direction, bandCloseness);
    star.halfAngleRad = STAR_MIN_HALF_ANGLE_RAD + (STAR_MAX_HALF_ANGLE_RAD - STAR_MIN_HALF_ANGLE_RAD) * luminance;

    // The class, drawn out of the table with the bright end favored in proportion to brightness.
    float pick = rng.Float01() * (1.0f - HOT_BIAS * luminance);
    std::size_t classIndex = std::size(SPECTRAL_CLASSES) - 1;
    for (std::size_t c = 0; c < std::size(SPECTRAL_CLASSES); ++c)
    {
      if (pick < SPECTRAL_CLASSES[c].weight)
      {
        classIndex = c;
        break;
      }
      pick -= SPECTRAL_CLASSES[c].weight;
    }
    const SpectralClass& spectral = SPECTRAL_CLASSES[classIndex];
    const float kelvin = spectral.minKelvin + rng.Float01() * (spectral.maxKelvin - spectral.minKelvin);
    const XMFLOAT3 hue = TemperatureColour(kelvin);

    const float saturation = STAR_MIN_SATURATION + (STAR_MAX_SATURATION - STAR_MIN_SATURATION) * luminance;
    star.colour = XMFLOAT3(Lerp(1.0f, hue.x, saturation) * star.intensity, Lerp(1.0f, hue.y, saturation) * star.intensity,
                           Lerp(1.0f, hue.z, saturation) * star.intensity);

    star.twinkleAmount = TWINKLE_MAX_AMOUNT + (TWINKLE_MIN_AMOUNT - TWINKLE_MAX_AMOUNT) * luminance;
    star.twinkleRateFraction = TWINKLE_MIN_RATE_FRACTION + rng.Float01() * (1.0f - TWINKLE_MIN_RATE_FRACTION);
    star.twinklePhaseFraction = rng.Float01();
    stars.push_back(star);
  }

  for (const Star& star : stars)
  {
    EmitQuad(_out.verts, star.direction, star.colour, star.halfAngleRad, 0.0f, star.twinkleAmount, star.twinkleRateFraction,
             star.twinklePhaseFraction);
  }
  _out.layerVertexCount[static_cast<std::uint32_t>(SkyLayer::Star)] = static_cast<std::uint32_t>(stars.size()) * VERTS_PER_QUAD;

  // --- the flares over the brightest ---------------------------------------------------------------
  // The brightest stars of the draw, not a second population placed on their own: a sky whose
  // standout stars sit somewhere other than where its brightest stars are has two skies in it.
  std::vector<std::uint32_t> order(stars.size());
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(stars.size()); ++i)
    order[i] = i;
  // A full sort, and the index breaks a tie, so the result is one order and not whichever order the
  // library's introsort happened to leave two equal magnitudes in.
  std::sort(order.begin(), order.end(),
            [&stars](std::uint32_t _a, std::uint32_t _b)
            {
              if (stars[_a].magnitude != stars[_b].magnitude)
                return stars[_a].magnitude < stars[_b].magnitude;
              return _a < _b;
            });

  for (std::uint32_t i = 0; i < brightCount; ++i)
  {
    const Star& star = stars[order[i]];
    const XMFLOAT3 colour(star.colour.x * BURST_INTENSITY_FACTOR, star.colour.y * BURST_INTENSITY_FACTOR,
                          star.colour.z * BURST_INTENSITY_FACTOR);
    // Rolled, unlike a star: Starburst.dds is one asymmetric flare, and nine unrolled copies of it
    // are nine copies of one picture in the places the eye is most likely to look.
    EmitQuad(_out.verts, star.direction, colour, star.halfAngleRad * BURST_SIZE_FACTOR, rng.Float01() * XM_2PI, star.twinkleAmount,
             star.twinkleRateFraction, star.twinklePhaseFraction);
  }
  _out.layerVertexCount[static_cast<std::uint32_t>(SkyLayer::Burst)] = brightCount * VERTS_PER_QUAD;
}
} // namespace Neuron
