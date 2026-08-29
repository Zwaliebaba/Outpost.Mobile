#pragma once

#include <cstdint>

namespace Neuron
{
// The tree's one random generator (AGENTS.md 5). PCG-XSH-RR 64/32: 64 bits of state, a 64-bit odd
// increment that selects the stream, and a 32-bit output.
//
// It is here rather than in whichever layer wanted randomness first, because the first caller was a
// visual effect and the simulation will want one too -- two generators is exactly the outcome the
// rule was written to prevent (Design/Decisions/0012).
//
// Nothing seeds itself. There is no clock read, no OS entropy, and no <random>: a seeded generator
// exists so that the same seed replays the same stream, on every machine and every build, and each
// of those three would take that away. A caller that wants variety builds a seed out of something
// it can name -- a handle, a tick -- and states what it used.
class Pcg32
{
public:
  // The reference implementation's own initialiser values, so a default-constructed generator
  // produces the published stream rather than an arbitrary one.
  static constexpr std::uint64_t DEFAULT_SEED = 0x853c49e6748fea9bull;
  static constexpr std::uint64_t DEFAULT_SEQUENCE = 0xda3e39cb94b95bdbull;

  constexpr Pcg32() noexcept
  {
    Seed(DEFAULT_SEED, DEFAULT_SEQUENCE);
  }

  constexpr explicit Pcg32(std::uint64_t _seed, std::uint64_t _sequence = DEFAULT_SEQUENCE) noexcept
  {
    Seed(_seed, _sequence);
  }

  // The reference seeding procedure, exactly: zero the state, take the stream from the sequence,
  // step, add the seed, step. Two generators seeded alike produce the same stream forever, which is
  // the whole property a seed is for -- a different procedure would still look random and would no
  // longer match anything.
  constexpr void Seed(std::uint64_t _seed, std::uint64_t _sequence = DEFAULT_SEQUENCE) noexcept
  {
    m_state = 0u;
    m_increment = (_sequence << 1u) | 1u;
    Step();
    m_state += _seed;
    Step();
  }

  [[nodiscard]] constexpr std::uint32_t Next() noexcept
  {
    return Step();
  }

  // Uniform in [0, _bound). Rejection against the reference's threshold rather than a modulo of the
  // raw output: the bias a modulo leaves is invisible on a five-way choice and plainly visible on a
  // wider one, and this header should not have to be revisited the day a caller widens theirs.
  [[nodiscard]] constexpr std::uint32_t Below(std::uint32_t _bound) noexcept
  {
    if (_bound == 0u)
      return 0u;

    const std::uint32_t threshold = (0u - _bound) % _bound;
    for (;;)
    {
      const std::uint32_t draw = Step();
      if (draw >= threshold)
        return draw % _bound;
    }
  }

  // Uniform in [0, 1). The top 24 bits are taken because 24 is what a float holds: every value is
  // exact, no value is 1.0, and the result is identical under /fp:precise on any machine.
  [[nodiscard]] constexpr float Float01() noexcept
  {
    return static_cast<float>(Step() >> 8u) * (1.0f / 16777216.0f);
  }

  // Uniform in [-_magnitude, +_magnitude).
  [[nodiscard]] constexpr float Signed(float _magnitude) noexcept
  {
    return (Float01() * 2.0f - 1.0f) * _magnitude;
  }

private:
  static constexpr std::uint64_t MULTIPLIER = 6364136223846793005ull;

  constexpr std::uint32_t Step() noexcept
  {
    // The output is computed from the state before the advance, as the reference does, so the two
    // are independent work rather than a dependency chain.
    const std::uint64_t previous = m_state;
    m_state = previous * MULTIPLIER + m_increment;

    const std::uint32_t xorshifted = static_cast<std::uint32_t>(((previous >> 18u) ^ previous) >> 27u);
    const std::uint32_t rotation = static_cast<std::uint32_t>(previous >> 59u);
    return (xorshifted >> rotation) | (xorshifted << ((0u - rotation) & 31u));
  }

  std::uint64_t m_state = 0;
  std::uint64_t m_increment = 0;
};
} // namespace Neuron
