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

// The same, for the reliable lane, which has a bound of its own.
using ReliableBuffer = std::array<std::uint8_t, Neuron::MAX_RELIABLE_BYTES>;

[[nodiscard]] bool SendReliableByte(Neuron::LoopbackTransport& _from, std::uint8_t _value)
{
  return _from.SendReliable(&_value, 1);
}
} // namespace

TEST_CLASS(LoopbackTransportTests)
{
public:
  TEST_METHOD(ZeroLatencyDeliversWithinTheSameTick)
  {
    // The single-player default, and the one that decides whether this slice changed the game. If a
    // datagram sent this tick is not readable this tick, the game gains a frame of lag it did not
    // have and "changes no gameplay" is false (Design/Archive/Collision-slice-2b.md 5.4).
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
    Neuron::LoopbackTransport::Connect(client, server, {.latencyTicks = 7});

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
    Neuron::LoopbackTransport::Connect(client, server, {.latencyTicks = 2});

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
    Neuron::LoopbackTransport::Connect(client, server, {.capacityDatagrams = 4});

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
      Neuron::LoopbackTransport::Connect(client, server, {.dropOneInN = 3});
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

  TEST_METHOD(TheReliableLaneKeepsWhatTheDatagramLaneDrops)
  {
    // The row this slice exists for. Every datagram is dropped -- dropOneInN = 1 -- and every
    // reliable message still arrives, in order. Finding E1 in Design/MmoScalabilityReview.md is a
    // lost leave being permanent; this is the property that retires it.
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport::Desc desc;
    desc.dropOneInN = 1;
    Neuron::LoopbackTransport::Connect(client, server, desc);

    AdvanceBoth(client, server, 0);
    for (std::uint8_t at = 0; at < 8; ++at)
    {
      Assert::IsTrue(SendByte(server, at), L"a dropped datagram is not a send failure");
      Assert::IsTrue(SendReliableByte(server, at), L"a reliable send failed on an empty lane");
    }
    AdvanceBoth(client, server, 0);

    Buffer datagram{};
    Assert::AreEqual(0u, client.Receive(datagram.data(), static_cast<std::uint32_t>(datagram.size())),
                     L"a datagram survived dropOneInN = 1");

    ReliableBuffer message{};
    for (std::uint8_t at = 0; at < 8; ++at)
    {
      const std::uint32_t size = client.ReceiveReliable(message.data(), static_cast<std::uint32_t>(message.size()));
      Assert::AreEqual(1u, size, L"a reliable message was lost while the datagram lane was dropping everything");
      Assert::AreEqual(at, message[0], L"the reliable lane delivered out of order");
    }
    Assert::AreEqual(0u, client.ReceiveReliable(message.data(), static_cast<std::uint32_t>(message.size())),
                     L"the lane delivered a message nobody sent");
  }

  TEST_METHOD(TheReliableLaneCarriesTheSameLatency)
  {
    // A wire's delay is the wire's, whichever lane a message took. Only the loss differs.
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport::Desc desc;
    desc.latencyTicks = 3;
    Neuron::LoopbackTransport::Connect(client, server, desc);

    AdvanceBoth(client, server, 10);
    Assert::IsTrue(SendReliableByte(server, 7), L"the reliable send failed");

    ReliableBuffer message{};
    AdvanceBoth(client, server, 12);
    Assert::AreEqual(0u, client.ReceiveReliable(message.data(), static_cast<std::uint32_t>(message.size())),
                     L"a reliable message arrived before its latency had elapsed");

    AdvanceBoth(client, server, 13);
    Assert::AreEqual(1u, client.ReceiveReliable(message.data(), static_cast<std::uint32_t>(message.size())),
                     L"a reliable message did not arrive on the tick it was due");
    Assert::AreEqual(static_cast<std::uint8_t>(7), message[0], L"the wrong message arrived");
  }

  TEST_METHOD(AFullSizedReliableMessageSurvivesAndALargerOneIsRefused)
  {
    // Refused rather than truncated, which is Send's rule on the other lane and the bug a wire that
    // silently shortens a message turns into a corrupt-payload hunt.
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport::Connect(client, server, {});
    AdvanceBoth(client, server, 0);

    std::vector<std::uint8_t> full(Neuron::MAX_RELIABLE_BYTES, 0xABu);
    Assert::IsTrue(server.SendReliable(full.data(), Neuron::MAX_RELIABLE_BYTES), L"a full-sized reliable message was refused");

    const std::vector<std::uint8_t> tooBig(Neuron::MAX_RELIABLE_BYTES + 1, 0xCDu);
    Assert::IsFalse(server.SendReliable(tooBig.data(), Neuron::MAX_RELIABLE_BYTES + 1), L"an oversized reliable message was accepted");

    AdvanceBoth(client, server, 0);
    ReliableBuffer message{};
    Assert::AreEqual(Neuron::MAX_RELIABLE_BYTES, client.ReceiveReliable(message.data(), static_cast<std::uint32_t>(message.size())),
                     L"the full-sized message did not survive");
    Assert::AreEqual(static_cast<std::uint8_t>(0xABu), message[Neuron::MAX_RELIABLE_BYTES - 1], L"the last byte did not survive");
  }

  TEST_METHOD(AFullReliableLaneRefusesRatherThanDropping)
  {
    // The one false SendReliable may return. A datagram lane drops the newest and says nothing,
    // because a lost datagram is a normal thing; this lane cannot drop, so it pushes back instead
    // and the sender learns before the message is gone.
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport::Desc desc;
    desc.capacityReliableMessages = 4;
    Neuron::LoopbackTransport::Connect(client, server, desc);
    AdvanceBoth(client, server, 0);

    for (std::uint8_t at = 0; at < 4; ++at)
      Assert::IsTrue(SendReliableByte(server, at), L"a send into an unfull lane failed");
    Assert::IsFalse(SendReliableByte(server, 4), L"a full lane accepted a message it had no room for");
    Assert::AreEqual(4u, client.QueuedReliableCount(), L"the lane did not hold exactly what it accepted");

    // Draining one makes room for exactly one more, and what was queued is untouched.
    AdvanceBoth(client, server, 0);
    ReliableBuffer message{};
    Assert::AreEqual(1u, client.ReceiveReliable(message.data(), static_cast<std::uint32_t>(message.size())), L"the lane delivered nothing");
    Assert::AreEqual(static_cast<std::uint8_t>(0), message[0], L"the lane dropped the oldest rather than refusing the newest");
    Assert::IsTrue(SendReliableByte(server, 4), L"the lane did not recover its room");
  }

  TEST_METHOD(TheTwoLanesDoNotDisturbEachOther)
  {
    // Separate rings, so a full lane refuses only itself; and the datagram lane's counted loss must
    // not shift because a reliable message went out between two datagrams.
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport::Desc desc;
    desc.dropOneInN = 2; // every second datagram
    Neuron::LoopbackTransport::Connect(client, server, desc);
    AdvanceBoth(client, server, 0);

    for (std::uint8_t at = 0; at < 4; ++at)
    {
      Assert::IsTrue(SendByte(server, at), L"the datagram send failed");
      Assert::IsTrue(SendReliableByte(server, at), L"the reliable send failed");
    }
    AdvanceBoth(client, server, 0);

    // 0 and 2 survive; 1 and 3 are the second and fourth datagrams and are dropped.
    Buffer datagram{};
    Assert::AreEqual(1u, client.Receive(datagram.data(), static_cast<std::uint32_t>(datagram.size())), L"the first datagram was lost");
    Assert::AreEqual(static_cast<std::uint8_t>(0), datagram[0], L"the loss pattern moved");
    Assert::AreEqual(1u, client.Receive(datagram.data(), static_cast<std::uint32_t>(datagram.size())), L"the third datagram was lost");
    Assert::AreEqual(static_cast<std::uint8_t>(2), datagram[0], L"the loss pattern moved");
    Assert::AreEqual(0u, client.Receive(datagram.data(), static_cast<std::uint32_t>(datagram.size())), L"a dropped datagram arrived");

    ReliableBuffer message{};
    for (std::uint8_t at = 0; at < 4; ++at)
    {
      Assert::AreEqual(1u, client.ReceiveReliable(message.data(), static_cast<std::uint32_t>(message.size())),
                       L"a reliable message was lost");
      Assert::AreEqual(at, message[0], L"the reliable lane delivered out of order");
    }
  }

  TEST_METHOD(AnUnconnectedEndRefusesReliableSends)
  {
    // The default in Transport.h refuses, and so does a real implementation with no peer. A caller
    // reads the two the same way, which is the point of the refusing default.
    Neuron::LoopbackTransport lonely;
    Assert::IsFalse(SendReliableByte(lonely, 1), L"an unconnected end accepted a reliable message");

    ReliableBuffer message{};
    Assert::AreEqual(0u, lonely.ReceiveReliable(message.data(), static_cast<std::uint32_t>(message.size())),
                     L"an unconnected end delivered a reliable message");
  }

  TEST_METHOD(AnUnconnectedEndRefusesToSend)
  {
    Neuron::LoopbackTransport lonely;
    Assert::IsTrue(lonely.State() == Neuron::ConnectionState::Disconnected, L"an unwired end reported itself connected");
    Assert::IsFalse(SendByte(lonely, 1), L"an unwired end accepted a datagram");
  }
};
} // namespace NeuronCoreTests
