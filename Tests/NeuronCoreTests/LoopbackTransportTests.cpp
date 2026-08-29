#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NeuronCoreTests
{
namespace
{
// Every test here drives both ends from one tick, the way the composition root does.
void AdvanceBoth(Neuron::LoopbackTransport& _a, Neuron::LoopbackTransport& _b, std::uint64_t _tick) noexcept
{
  _a.AdvanceTo(_tick);
  _b.AdvanceTo(_tick);
  _a.Poll();
  _b.Poll();
}

[[nodiscard]] bool SendByte(Neuron::LoopbackTransport& _from, std::uint8_t _value)
{
  return _from.Send(&_value, 1);
}

// The size a caller must always offer, since it is the most a datagram can be.
using Buffer = std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES>;
} // namespace

TEST_CLASS(LoopbackTransportTests)
{
public:
  TEST_METHOD(ZeroLatencyDeliversWithinTheSameTick)
  {
    // The single-player default, and the one that decides whether this slice changed the game. If a
    // datagram sent this tick is not readable this tick, the game gains a frame of lag it did not
    // have and "changes no gameplay" is false (Design/Collision-slice-2b.md 5.4).
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport::Connect(client, server, {});

    AdvanceBoth(client, server, 100);
    Assert::IsTrue(SendByte(server, 42), L"a send on an idle queue failed");
    server.AdvanceTo(100);
    client.AdvanceTo(100);
    client.Poll();

    Buffer got{};
    Assert::AreEqual(1u, client.Receive(got.data(), static_cast<std::uint32_t>(got.size())),
                     L"a zero-latency datagram was not readable on the tick it was sent");
    Assert::AreEqual(42, static_cast<int>(got[0]), L"the payload changed in flight");
  }

  TEST_METHOD(LatencyIsCountedInWholeTicks)
  {
    // Seven ticks means seven: not readable at six, readable at seven. Counted in ticks rather than
    // seconds so the measurement reproduces on a slower machine (slice-2b 2.1).
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport::Connect(client, server, {7, 256, 0});

    AdvanceBoth(client, server, 0);
    Assert::IsTrue(SendByte(server, 9), L"the send failed");

    Buffer got{};
    for (std::uint64_t tick = 0; tick < 7; ++tick)
    {
      AdvanceBoth(client, server, tick);
      Assert::AreEqual(0u, client.Receive(got.data(), static_cast<std::uint32_t>(got.size())),
                       L"a datagram arrived before its latency had elapsed");
    }

    AdvanceBoth(client, server, 7);
    Assert::AreEqual(1u, client.Receive(got.data(), static_cast<std::uint32_t>(got.size())),
                     L"a datagram did not arrive on the tick its latency expired");
  }

  TEST_METHOD(DatagramsArriveInSendOrder)
  {
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport::Connect(client, server, {2, 256, 0});

    AdvanceBoth(client, server, 0);
    for (std::uint8_t value = 0; value < 16; ++value)
      Assert::IsTrue(SendByte(server, value), L"a send failed with the queue far from full");

    AdvanceBoth(client, server, 2);
    Buffer got{};
    for (int expected = 0; expected < 16; ++expected)
    {
      Assert::AreEqual(1u, client.Receive(got.data(), static_cast<std::uint32_t>(got.size())), L"a datagram went missing");
      Assert::AreEqual(expected, static_cast<int>(got[0]), L"datagrams arrived out of send order");
    }
  }

  TEST_METHOD(AFullQueueDropsTheNewestAndKeepsTheRest)
  {
    // Transport.h calls a failed send normal rather than an error: the caller drops the message
    // instead of blocking a frame on it. What must not happen is losing what is already in flight.
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport::Connect(client, server, {0, 4, 0});

    AdvanceBoth(client, server, 0);
    for (std::uint8_t value = 0; value < 4; ++value)
      Assert::IsTrue(SendByte(server, value), L"a send failed before the queue was full");

    Assert::IsFalse(SendByte(server, 99), L"a send onto a full queue reported success");
    Assert::AreEqual(4u, client.QueuedCount(), L"a refused send changed what was already queued");

    AdvanceBoth(client, server, 0);
    Buffer got{};
    for (int expected = 0; expected < 4; ++expected)
    {
      Assert::AreEqual(1u, client.Receive(got.data(), static_cast<std::uint32_t>(got.size())), L"a queued datagram was lost");
      Assert::AreEqual(expected, static_cast<int>(got[0]), L"the queue kept the wrong datagrams");
    }
  }

  TEST_METHOD(CountedLossDropsTheSameOnesEveryRun)
  {
    // Counted rather than random, so a measurement taken under loss can be taken again. AGENTS.md 5
    // bans unseeded randomness and this is why that rule is worth having in a transport too.
    const auto run = [](int _sends)
    {
      Neuron::LoopbackTransport client;
      Neuron::LoopbackTransport server;
      Neuron::LoopbackTransport::Connect(client, server, {0, 256, 3});
      AdvanceBoth(client, server, 0);
      for (std::uint8_t value = 0; value < static_cast<std::uint8_t>(_sends); ++value)
        (void)SendByte(server, value);
      AdvanceBoth(client, server, 0);

      std::vector<int> arrived;
      Buffer got{};
      while (client.Receive(got.data(), static_cast<std::uint32_t>(got.size())) != 0)
        arrived.push_back(static_cast<int>(got[0]));
      return arrived;
    };

    const std::vector<int> first = run(9);
    const std::vector<int> second = run(9);

    Assert::AreEqual(static_cast<std::size_t>(6), first.size(), L"one in three was not dropped");
    Assert::IsTrue(first == second, L"the same send sequence dropped different datagrams twice");
    // Every third send -- the 3rd, 6th and 9th, which carry values 2, 5 and 8 -- is the one lost.
    const std::vector<int> expected{0, 1, 3, 4, 6, 7};
    Assert::IsTrue(first == expected, L"loss did not fall on every third datagram");
  }

  TEST_METHOD(AFullSizedDatagramSurvivesAndALargerOneIsRefused)
  {
    // Refused rather than truncated: a silently shortened datagram turns a format bug into a
    // corrupt-payload bug, which is far harder to find.
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport::Connect(client, server, {});
    AdvanceBoth(client, server, 0);

    Buffer sent{};
    for (std::size_t at = 0; at < sent.size(); ++at)
      sent[at] = static_cast<std::uint8_t>(at & 0xFFu);

    Assert::IsTrue(server.Send(sent.data(), Neuron::MAX_DATAGRAM_BYTES), L"a datagram of exactly the maximum size was refused");
    Assert::IsFalse(server.Send(sent.data(), Neuron::MAX_DATAGRAM_BYTES + 1), L"an oversized datagram was accepted");

    AdvanceBoth(client, server, 0);
    Buffer got{};
    Assert::AreEqual(Neuron::MAX_DATAGRAM_BYTES, client.Receive(got.data(), static_cast<std::uint32_t>(got.size())),
                     L"a full-sized datagram did not come back whole");
    Assert::IsTrue(sent == got, L"a full-sized datagram changed in flight");
  }

  TEST_METHOD(BothDirectionsAreIndependent)
  {
    // One object, two ends, two queues. A client order travelling up must not disturb the snapshot
    // travelling down, which is the whole reason the queues are separate.
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport::Connect(client, server, {});
    AdvanceBoth(client, server, 0);

    Assert::IsTrue(SendByte(server, 1), L"the downstream send failed");
    Assert::IsTrue(SendByte(client, 2), L"the upstream send failed");
    AdvanceBoth(client, server, 0);

    Buffer got{};
    Assert::AreEqual(1u, client.Receive(got.data(), static_cast<std::uint32_t>(got.size())), L"the client received nothing");
    Assert::AreEqual(1, static_cast<int>(got[0]), L"the client received the datagram it sent");
    Assert::AreEqual(1u, server.Receive(got.data(), static_cast<std::uint32_t>(got.size())), L"the server received nothing");
    Assert::AreEqual(2, static_cast<int>(got[0]), L"the server received the datagram it sent");
  }

  TEST_METHOD(AnUnconnectedEndRefusesToSend)
  {
    Neuron::LoopbackTransport lonely;
    Assert::IsTrue(lonely.State() == Neuron::ConnectionState::Disconnected, L"an unwired end reported itself connected");
    Assert::IsFalse(SendByte(lonely, 1), L"an unwired end accepted a datagram");
  }
};
} // namespace NeuronCoreTests
