# Work order — Spaceship explosion slice 1: `Pcg32`

Implements slice 1 of [`SpaceshipExplosion.md`](SpaceshipExplosion.md) §14: the tree's one seeded
random generator, in `NeuronCore`, so the explosion has something to draw from and `GameLogic` has
something to adopt when it needs one.

**Layer:** `NeuronCore` and `NeuronCoreTests`.
**Depends on:** nothing.
**Blocks:** slice 2.

---

## 1. Why this is a slice and not a helper in slice 2

AGENTS.md §5 says randomness arrives as "one seeded PCG32", and `GameLogic/GameLogic.h:11` says the
same in its own words. The first thing in the tree to want randomness is a visual effect, which
could hide a generator in the executable and nobody would notice — until the simulation needs
one, finds it cannot reach the executable's, and writes a second. The rule was written to prevent
exactly that, so the generator goes where both can reach it, and a library gaining a
responsibility takes a decision record (AGENTS.md §9). That is the whole slice: one header, one
test file, one record.

---

## 2. Scope

### 2.1 `NeuronCore/Pcg32.h`

Header-only, namespace `Neuron`, the reference `pcg32` (PCG-XSH-RR, 64-bit state, 64-bit
increment). Public surface, and nothing beyond it:

```cpp
class Pcg32
{
public:
  static constexpr std::uint64_t DEFAULT_SEED = 0x853c49e6748fea9bull;
  static constexpr std::uint64_t DEFAULT_SEQUENCE = 0xda3e39cb94b95bdbull;

  constexpr Pcg32() noexcept;                                                   // DEFAULT_SEED, DEFAULT_SEQUENCE
  constexpr explicit Pcg32(std::uint64_t _seed, std::uint64_t _sequence = DEFAULT_SEQUENCE) noexcept;

  constexpr void Seed(std::uint64_t _seed, std::uint64_t _sequence = DEFAULT_SEQUENCE) noexcept;

  [[nodiscard]] constexpr std::uint32_t Next() noexcept;                         // full 32 bits
  [[nodiscard]] constexpr std::uint32_t Below(std::uint32_t _bound) noexcept;    // [0, _bound), unbiased; _bound == 0 returns 0
  [[nodiscard]] constexpr float Float01() noexcept;                              // [0, 1)
  [[nodiscard]] constexpr float Signed(float _magnitude) noexcept;               // [-_magnitude, +_magnitude)

private:
  std::uint64_t m_state = 0;
  std::uint64_t m_increment = 0;
};
```

Rules the implementation follows:

- **Seeding is the reference procedure**: `m_increment = (_sequence << 1) | 1; m_state = 0;
  Next(); m_state += _seed; Next();`. Two generators seeded alike produce the same stream, on
  every machine, forever; that is what makes a seed a replay key.
- **`Below` is unbiased** — Lemire's multiply-and-reject or the reference's threshold rejection,
  not `Next() % _bound`. A modulo bias on a 5-way tumbler choice is invisible; on a 25-way
  colour choice it is not, and the header should not need revisiting for it.
- **`Float01` takes the top 24 bits**: `(Next() >> 8) * (1.0f / 16777216.0f)`. Exact in `float`,
  never returns 1.0, and identical under `/fp:precise` everywhere.
- **No `<random>`**, no `std::uniform_*` — their output is implementation-defined, which is the
  opposite of what a seeded generator is for.
- **No OS entropy, no clock, no default-random seeding of any kind.** The default constructor
  gives the reference stream; a caller that wants variety seeds it.
- **`constexpr` throughout**, so a `static_assert` can pin the first output (§5).

### 2.2 The umbrella

`NeuronCore/NeuronCore.h` includes `Pcg32.h` after `Ease.h`. It has no dependencies beyond
`<cstdint>`, so its position in the list does not matter, but it goes with the other pure headers.

### 2.3 Project files

`NeuronCore.vcxproj` and `NeuronCore.vcxproj.filters` gain `Pcg32.h`; `NeuronCoreTests.vcxproj`
and its `.filters` gain `Pcg32Tests.cpp`. Both, in the same commit; `Build/CheckProjectFiles.py`
checks.

### 2.4 The decision record

`Design/Decisions/0011-randomness-is-one-pcg32-in-neuroncore.md`, in the README's format, indexed.
Context: the rulebook's sentence and the first caller being a visual effect. Decision: one
generator type, in `NeuronCore`, seeded by whoever holds it — the simulation will hold one, the
view holds as many as it likes. Alternatives: a private generator in the executable (loses the
day `GameLogic` needs one); `<random>` (not reproducible across implementations); a generator in
`GameLogic` (then the client cannot reach it without breaking §2's rules). Consequences:
`NeuronCore` gains a header; every future randomness question has one answer; nothing seeds from
entropy, and a caller that wants an unpredictable stream has to build its seed from something it
can name.

---

## 3. Out of scope

- **Using it anywhere.** No caller lands in this slice. `GameLogic` does not adopt it; slice 2 is
  the first consumer.
- **Any other distribution** — no Gaussian, no shuffle, no unit vector. Slice 2 builds what the
  effect needs from `Float01` and `Signed`, in the effect.
- **64-bit output, jump-ahead, multiple streams beyond the sequence parameter.** The reference
  has them; nothing here wants them.
- **`AGENTS.md` §5's sentence.** "One seeded PCG32 when randomness arrives" becomes true rather
  than false, so it does not change. Check rather than assume.

---

## 4. What to build on

| File | What it already gives you |
|---|---|
| `NeuronCore/Ease.h` | The shape of a header-only, `noexcept`, `[[nodiscard]]` maths utility in this tree |
| `NeuronCore/NeuronCore.h` | The umbrella and the include order |
| `Tests/NeuronCoreTests/EaseTests.cpp` | A test file in the house style, with the property-per-test comments |
| `Design/Decisions/README.md` | The record template and the index |
| AGENTS.md §1 R3, R8 | `UPPER_CASE` constants, `m_` on class state |

---

## 5. Acceptance

Tests in `Tests/NeuronCoreTests/Pcg32Tests.cpp`:

- **The reference stream.** `Pcg32(42, 54)` produces, as its first five `Next()` values, the
  numbers the reference implementation produces for that seed and sequence: `0xa15c02b7`,
  `0x7b47f409`, `0xba1d3330`, `0x83d2f293`, `0xbfa4784b`. These are the published
  `pcg32_random_r` test vectors for `pcg32_srandom_r(42u, 54u)`; if the implementation differs
  from them it is not PCG32 and the test name is wrong. Also a `static_assert` in the test on the
  first value, proving the generator is `constexpr`.
- **Determinism.** Two generators seeded alike produce identical 1000-value streams; two seeded
  differently (same seed, different sequence) diverge within the first value.
- **`Below` range and coverage.** Over 100 000 draws, `Below(5)` returns only 0–4 and returns each
  at least 15 000 times; `Below(1)` is always 0; `Below(0)` is 0 and does not trap.
- **`Float01` range.** Over 100 000 draws, every value is in `[0, 1)`, and the mean is within
  0.01 of 0.5.
- **`Signed` range.** Over 100 000 draws of `Signed(100)`, every value is in `[−100, 100)`, and
  the mean is within 1 of 0.
- **The reference default.** `Pcg32()` equals `Pcg32(DEFAULT_SEED, DEFAULT_SEQUENCE)` stream-wise.

The tree:

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds; `NeuronCoreTests` runs and every existing test still passes.
- ADR 0011 exists and `Design/Decisions/README.md` indexes it.
- No screenshot: nothing visual.

---

## 6. Assumptions the implementer may make

- The five test vectors above are taken from the PCG reference's own `sample/pcg32-demo` output
  for round 1 with `pcg32_srandom_r(&rng, 42u, 54u)`. If the implementer's first output differs,
  suspect the seeding procedure (§2.1) before suspecting the vectors.
- A `constexpr` 64-bit multiply is fine on MSVC v145 at `/std:c++latest`.
- Nothing in this slice runs at frame rate, so no performance claim is due.
