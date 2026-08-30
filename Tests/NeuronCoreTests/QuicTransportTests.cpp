#include "pch.h"

// Not reached through the umbrella: NeuronCore.h deliberately does not carry the credential, so that
// wincrypt and ncrypt stay out of every translation unit that only wants a Transport
// (Design/Archive/QuicTransport-slice-1.md 2.5). The test that decides the credential works has to
// name it, so it names it here.
#include "DevCertificate.h"

#include <array>
#include <chrono>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NeuronCoreTests
{
namespace
{
// The size a caller must always offer, since it is the most a datagram can be.
using Buffer = std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES>;

// The same, for the reliable lane. Heap-allocated where a test needs one, because 8 KB is more than
// belongs on a stack frame the framework already owns.
using ReliableBuffer = std::vector<std::uint8_t>;

// The ring's own default, not a number this file chose. A test that spelled 256 would go on passing
// the day the class stopped using it.
constexpr std::uint32_t DEFAULT_CAPACITY = Neuron::QuicTransport::Desc{}.capacityDatagrams;

// One snapshot's worth of fragments (GameLogic's ShipsPerSnapshotFragment() covers 13 ships), which
// is the burst the wire actually sees at 10 Hz.
constexpr int SNAPSHOT_FRAGMENTS = 13;

[[nodiscard]] std::wstring Widen(const char* _text)
{
  const std::string narrow(_text != nullptr ? _text : "");
  return std::wstring(narrow.begin(), narrow.end());
}

[[nodiscard]] const wchar_t* StateName(Neuron::ConnectionState _state) noexcept
{
  switch (_state)
  {
  case Neuron::ConnectionState::Disconnected:
    return L"Disconnected";
  case Neuron::ConnectionState::Connecting:
    return L"Connecting";
  case Neuron::ConnectionState::Connected:
    return L"Connected";
  case Neuron::ConnectionState::Draining:
    return L"Draining";
  case Neuron::ConnectionState::Closed:
    return L"Closed";
  }
  return L"?";
}

// A real localhost connection: one library, one listener on an ephemeral port, and one client dialled
// at whatever port it got. Nothing is shared between tests -- MsQuic is opened and closed per test --
// so a test that goes wrong takes only itself with it.
//
// EVERY WAIT IN THIS FILE IS BOUNDED. Waiting for a state is bounded by QUIC_HANDSHAKE_TIMEOUT_MS
// and fails the test with the states it saw, rather than sitting until CI's timeout kills the run
// and says nothing. The one longer bound is teardown: ~Pair closes the connections, and closing one
// waits up to the idle timeout for MsQuic to finish with it (QuicTransport::Close). That is
// milliseconds in every passing run and ten seconds only if MsQuic has stopped answering.
class Pair
{
public:
  explicit Pair(std::uint32_t _clientCapacity = DEFAULT_CAPACITY, std::uint32_t _serverCapacity = DEFAULT_CAPACITY)
  {
    Neuron::QuicApi::Desc apiDesc;
    apiDesc.allowUnvalidatedPeer = true;
    Assert::IsTrue(m_api.Open(apiDesc), Widen(m_api.Reason()).c_str());

    Neuron::QuicListener::Desc listenerDesc;
    listenerDesc.transport.capacityDatagrams = _serverCapacity;
    Assert::IsTrue(m_listener.Start(m_api, 0, listenerDesc), Widen(m_listener.Reason()).c_str());
    Assert::IsTrue(m_listener.Port() != 0, L"a listener started on port 0 did not report the port it got");

    Neuron::QuicTransport::Desc clientDesc;
    clientDesc.capacityDatagrams = _clientCapacity;
    Assert::IsTrue(m_client.Connect(m_api, {"127.0.0.1", m_listener.Port()}, clientDesc), Widen(m_client.Reason()).c_str());
  }

  ~Pair()
  {
    // The order Design/QuicTransport.md 6 gives, and the order OutpostApp::Shutdown uses: the client
    // end, then the listener and everything it accepted, then the library.
    m_client.Close();
    m_listener.Stop();
    m_api.Close();
  }

  Pair(const Pair&) = delete;
  Pair& operator=(const Pair&) = delete;

  [[nodiscard]] Neuron::QuicTransport& Client() noexcept
  {
    return m_client;
  }

  [[nodiscard]] Neuron::QuicTransport* Server() noexcept
  {
    const std::span<Neuron::QuicTransport* const> accepted = m_listener.Accepted();
    return accepted.empty() ? nullptr : accepted[0];
  }

  [[nodiscard]] std::uint16_t Port() const noexcept
  {
    return m_listener.Port();
  }

  // How many connections the listener has seen come and go. Since ADR 0031 this is how a departure
  // is reported: the transport leaves Accepted() and this rises.
  [[nodiscard]] std::uint32_t RecycledCount() const noexcept
  {
    return m_listener.RecycledCount();
  }

  [[nodiscard]] Neuron::QuicApi& Api() noexcept
  {
    return m_api;
  }

  // One turn of what the composition root does every frame: the listener surfaces what it accepted,
  // and each end delivers what its workers left in the ring.
  void Pump()
  {
    m_listener.Poll();
    m_client.Poll();
    if (Neuron::QuicTransport* const server = Server())
      server->Poll();
  }

  template <class Predicate> [[nodiscard]] bool PumpUntil(Predicate _done, std::uint32_t _timeoutMs)
  {
    const std::int64_t start = m_clock.Now();
    for (;;)
    {
      Pump();
      if (_done())
        return true;
      if (m_clock.ElapsedMs(start, m_clock.Now()) >= static_cast<float>(_timeoutMs))
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Time passing with nothing polled, which is the only way to show that delivery happens in Poll
  // and not on the worker that put the datagram in the ring.
  void SpinWithoutPolling(std::uint32_t _forMs)
  {
    const std::int64_t start = m_clock.Now();
    while (m_clock.ElapsedMs(start, m_clock.Now()) < static_cast<float>(_forMs))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  void RequireConnected()
  {
    const bool connected = PumpUntil(
      [this]
      {
        const Neuron::QuicTransport* const server = Server();
        return server != nullptr && m_client.State() == Neuron::ConnectionState::Connected &&
               server->State() == Neuron::ConnectionState::Connected;
      },
      Neuron::QUIC_HANDSHAKE_TIMEOUT_MS);

    if (!connected)
    {
      const Neuron::QuicTransport* const server = Server();
      std::wstring message = L"the handshake did not complete: client ";
      message += StateName(m_client.State());
      message += L", server ";
      message += (server != nullptr) ? StateName(server->State()) : L"never accepted";
      Assert::Fail(message.c_str());
    }
  }

private:
  Neuron::FrameClock m_clock;
  Neuron::QuicApi m_api;
  Neuron::QuicListener m_listener;
  Neuron::QuicTransport m_client;
};

[[nodiscard]] bool SendByte(Neuron::QuicTransport& _from, std::uint8_t _value)
{
  return _from.Send(&_value, 1);
}

// A datagram every byte of which follows from its id, so that a fragment which arrives out of order
// still proves it arrived whole.
void FillPattern(Buffer& _outBytes, int _id)
{
  for (std::size_t at = 0; at < _outBytes.size(); ++at)
    _outBytes[at] = static_cast<std::uint8_t>((at * 7u + static_cast<std::size_t>(_id) * 31u) & 0xFFu);
  _outBytes[0] = static_cast<std::uint8_t>(_id);
}
} // namespace

TEST_CLASS(QuicTransportTests)
{
public:
  TEST_METHOD(AConnectionReachesConnectedOnBothEnds)
  {
    // The whole slice in one line: a real MsQuic handshake over 127.0.0.1, inside the budget boot is
    // willing to wait before it falls back to the loopback.
    Pair pair;
    pair.RequireConnected();

    Assert::IsTrue(pair.Client().State() == Neuron::ConnectionState::Connected, L"the client end did not reach Connected");
    Assert::IsTrue(pair.Server()->State() == Neuron::ConnectionState::Connected, L"the accepted end did not reach Connected");
  }

  TEST_METHOD(TheMaxSendLengthCoversTheDatagram)
  {
    // Connected is defined to require this, so a regression here shows up as a handshake that never
    // finishes rather than as a truncated snapshot. The number goes in the log so that the day
    // MsQuic's MTU floor moves, the run says so.
    Pair pair;
    pair.RequireConnected();

    const std::uint32_t clientLimit = pair.Client().MaxSendLength();
    const std::uint32_t serverLimit = pair.Server()->MaxSendLength();
    Logger::WriteMessage(
      (L"MaxSendLength: client " + std::to_wstring(clientLimit) + L", server " + std::to_wstring(serverLimit) + L"\n").c_str());

    Assert::IsTrue(clientLimit >= Neuron::MAX_DATAGRAM_BYTES, L"the client's peer will not take a full-sized datagram");
    Assert::IsTrue(serverLimit >= Neuron::MAX_DATAGRAM_BYTES, L"the server's peer will not take a full-sized datagram");
  }

  TEST_METHOD(AFullSizedDatagramSurvivesAndALargerOneIsRefused)
  {
    // LoopbackTransportTests' test, over a real wire. Refused rather than truncated: a wire that
    // silently shortens a datagram turns a format bug into a corrupt-payload bug.
    Pair pair;
    pair.RequireConnected();

    std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES + 1> sent{};
    for (std::size_t at = 0; at < sent.size(); ++at)
      sent[at] = static_cast<std::uint8_t>(at & 0xFFu);

    Assert::IsTrue(pair.Client().Send(sent.data(), Neuron::MAX_DATAGRAM_BYTES), L"a datagram of exactly the maximum size was refused");
    Assert::IsFalse(pair.Client().Send(sent.data(), Neuron::MAX_DATAGRAM_BYTES + 1), L"an oversized datagram was accepted");

    Buffer got{};
    Neuron::QuicTransport* const server = pair.Server();
    Assert::IsTrue(pair.PumpUntil([&] { return server->Receive(got.data(), static_cast<std::uint32_t>(got.size())) != 0; },
                                  Neuron::QUIC_HANDSHAKE_TIMEOUT_MS),
                   L"a full-sized datagram never arrived");

    for (std::size_t at = 0; at < got.size(); ++at)
      Assert::AreEqual(static_cast<int>(sent[at]), static_cast<int>(got[at]), L"a full-sized datagram changed in flight");

    Assert::AreEqual(0u, server->Receive(got.data(), static_cast<std::uint32_t>(got.size())), L"the oversized datagram was sent anyway");
  }

  TEST_METHOD(AFragmentSizedBurstArrivesIntact)
  {
    // One snapshot's fragments, back to back, which is what the wire sees ten times a second. QUIC
    // may reorder them and nothing here depends on it not doing so: each datagram's bytes follow
    // from its id, so arriving whole is decidable without arriving in order.
    Pair pair;
    pair.RequireConnected();

    for (int id = 0; id < SNAPSHOT_FRAGMENTS; ++id)
    {
      Buffer sent{};
      FillPattern(sent, id);
      Assert::IsTrue(pair.Client().Send(sent.data(), Neuron::MAX_DATAGRAM_BYTES), L"a fragment-sized send failed mid-burst");
    }

    std::array<bool, SNAPSHOT_FRAGMENTS> seen{};
    int arrived = 0;
    Neuron::QuicTransport* const server = pair.Server();
    const bool all = pair.PumpUntil(
      [&]
      {
        Buffer got{};
        while (server->Receive(got.data(), static_cast<std::uint32_t>(got.size())) != 0)
        {
          const int id = static_cast<int>(got[0]);
          Assert::IsTrue(id >= 0 && id < SNAPSHOT_FRAGMENTS, L"a datagram arrived carrying an id nothing sent");
          Assert::IsFalse(seen[static_cast<std::size_t>(id)], L"the same fragment arrived twice");

          Buffer expected{};
          FillPattern(expected, id);
          Assert::IsTrue(expected == got, L"a fragment changed in flight");

          seen[static_cast<std::size_t>(id)] = true;
          ++arrived;
        }
        return arrived == SNAPSHOT_FRAGMENTS;
      },
      Neuron::QUIC_HANDSHAKE_TIMEOUT_MS);

    Assert::IsTrue(all, (L"only " + std::to_wstring(arrived) + L" of 13 fragments arrived").c_str());
  }

  TEST_METHOD(BothDirectionsAreIndependent)
  {
    // One connection, two rings. An order travelling up must not disturb the snapshot travelling
    // down, which is the whole reason the two directions have separate arenas.
    Pair pair;
    pair.RequireConnected();

    Neuron::QuicTransport* const server = pair.Server();
    Assert::IsTrue(SendByte(*server, 1), L"the downstream send failed");
    Assert::IsTrue(SendByte(pair.Client(), 2), L"the upstream send failed");

    Buffer clientGot{};
    Buffer serverGot{};
    std::uint32_t clientSize = 0;
    std::uint32_t serverSize = 0;
    Assert::IsTrue(pair.PumpUntil(
                     [&]
                     {
                       if (clientSize == 0)
                         clientSize = pair.Client().Receive(clientGot.data(), static_cast<std::uint32_t>(clientGot.size()));
                       if (serverSize == 0)
                         serverSize = server->Receive(serverGot.data(), static_cast<std::uint32_t>(serverGot.size()));
                       return clientSize != 0 && serverSize != 0;
                     },
                     Neuron::QUIC_HANDSHAKE_TIMEOUT_MS),
                   L"one of the two directions delivered nothing");

    Assert::AreEqual(1, static_cast<int>(clientGot[0]), L"the client received the datagram it sent");
    Assert::AreEqual(2, static_cast<int>(serverGot[0]), L"the server received the datagram it sent");
  }

  TEST_METHOD(TheReliableLaneCarriesAMessageBothWays)
  {
    // The lane over a real connection. It needs one more round trip than the datagram lane -- the
    // stream has to be opened and accepted -- which is why ReliableReady is a question of its own
    // and a caller that must not lose its first message waits for it rather than for Connected.
    Pair pair;
    Assert::IsTrue(pair.PumpUntil([&pair]
                                  { return pair.Server() != nullptr && pair.Client().State() == Neuron::ConnectionState::Connected; },
                                  Neuron::QUIC_HANDSHAKE_TIMEOUT_MS),
                   L"the handshake did not complete");
    Assert::IsTrue(pair.PumpUntil([&pair] { return pair.Client().ReliableReady() && pair.Server()->ReliableReady(); },
                                  Neuron::QUIC_HANDSHAKE_TIMEOUT_MS),
                   L"the reliable lane never came up on both ends");

    const std::uint8_t up[3] = {1, 2, 3};
    const std::uint8_t down[2] = {9, 8};
    Assert::IsTrue(pair.Client().SendReliable(up, 3), L"the client's reliable send failed");
    Assert::IsTrue(pair.Server()->SendReliable(down, 2), L"the server's reliable send failed");

    ReliableBuffer message(Neuron::MAX_RELIABLE_BYTES, 0u);
    Assert::IsTrue(pair.PumpUntil([&] { return pair.Server()->ReceiveReliable(message.data(), Neuron::MAX_RELIABLE_BYTES) == 3; },
                                  Neuron::QUIC_HANDSHAKE_TIMEOUT_MS),
                   L"the server did not receive the client's message");
    Assert::AreEqual(static_cast<std::uint8_t>(1), message[0], L"the message arrived corrupted");

    Assert::IsTrue(pair.PumpUntil([&] { return pair.Client().ReceiveReliable(message.data(), Neuron::MAX_RELIABLE_BYTES) == 2; },
                                  Neuron::QUIC_HANDSHAKE_TIMEOUT_MS),
                   L"the client did not receive the server's message");
    Assert::AreEqual(static_cast<std::uint8_t>(9), message[0], L"the message arrived corrupted");
  }

  TEST_METHOD(TheReliableLaneKeepsOrderAcrossManyMessages)
  {
    // A stream is ordered and this lane is framed over one, so a burst arrives in send order and
    // whole. The burst is deliberately larger than one datagram would carry, since reassembling a
    // frame split across deliveries is the part of this that a unit test cannot see from outside.
    Pair pair;
    Assert::IsTrue(
      pair.PumpUntil([&pair] { return pair.Server() != nullptr && pair.Client().ReliableReady(); }, Neuron::QUIC_HANDSHAKE_TIMEOUT_MS),
      L"the reliable lane never came up");

    constexpr std::uint32_t COUNT = 24;
    constexpr std::uint32_t SIZE = 700; // several of these exceed one datagram when framed together
    ReliableBuffer payload(SIZE, 0u);
    for (std::uint32_t at = 0; at < COUNT; ++at)
    {
      payload[0] = static_cast<std::uint8_t>(at);
      Assert::IsTrue(pair.Client().SendReliable(payload.data(), SIZE), L"a reliable send failed mid-burst");
    }

    ReliableBuffer message(Neuron::MAX_RELIABLE_BYTES, 0u);
    for (std::uint32_t at = 0; at < COUNT; ++at)
    {
      Assert::IsTrue(pair.PumpUntil([&] { return pair.Server()->ReceiveReliable(message.data(), Neuron::MAX_RELIABLE_BYTES) == SIZE; },
                                    Neuron::QUIC_HANDSHAKE_TIMEOUT_MS),
                     L"a message in the burst never arrived");
      Assert::AreEqual(static_cast<std::uint8_t>(at), message[0], L"the reliable lane delivered out of order");
    }
  }

  TEST_METHOD(AnOversizedReliableMessageIsRefused)
  {
    // Refused rather than truncated, which is what Send already promises on the datagram lane. A
    // wire that silently shortens a message turns a format bug into a corrupt-payload hunt.
    Pair pair;
    Assert::IsTrue(pair.PumpUntil([&pair] { return pair.Client().ReliableReady(); }, Neuron::QUIC_HANDSHAKE_TIMEOUT_MS),
                   L"the lane never came up");

    const ReliableBuffer tooBig(Neuron::MAX_RELIABLE_BYTES + 1, 0xCDu);
    Assert::IsFalse(pair.Client().SendReliable(tooBig.data(), Neuron::MAX_RELIABLE_BYTES + 1),
                    L"an oversized reliable message was accepted");

    const ReliableBuffer full(Neuron::MAX_RELIABLE_BYTES, 0xABu);
    Assert::IsTrue(pair.Client().SendReliable(full.data(), Neuron::MAX_RELIABLE_BYTES), L"a full-sized reliable message was refused");
  }

  TEST_METHOD(AFullRingDropsTheNewestAndCountsIt)
  {
    // Transport.h calls a lost datagram normal rather than an error. What must not happen is losing
    // what is already in the ring, and what must not happen silently is losing anything at all --
    // hence the count. The receiver is polled but never read from, because the ring fills against
    // the READ cursor: Poll moves the ready mark, Receive is what frees a slot.
    constexpr std::uint32_t RING = 8;
    Pair pair(DEFAULT_CAPACITY, RING);
    pair.RequireConnected();

    for (std::uint32_t sent = 0; sent < RING + 1; ++sent)
      Assert::IsTrue(SendByte(pair.Client(), static_cast<std::uint8_t>(sent)), L"a send failed with the sender's ring nearly empty");

    Neuron::QuicTransport* const server = pair.Server();
    Assert::IsTrue(pair.PumpUntil([&] { return server->DroppedCount() == 1; }, Neuron::QUIC_HANDSHAKE_TIMEOUT_MS),
                   L"a ring of eight did not drop the ninth datagram exactly once");

    // One more Poll before reading, and it is load-bearing rather than defensive. PumpUntil polls
    // and then tests, so the eighth datagram can land between the two -- leaving the drop counted
    // and the ready cursor one short, and the loop below accusing correct code of losing a datagram.
    server->Poll();

    Buffer got{};
    for (std::uint32_t taken = 0; taken < RING; ++taken)
      Assert::AreEqual(1u, server->Receive(got.data(), static_cast<std::uint32_t>(got.size())),
                       L"a datagram already in the ring was lost to the overflow");

    Assert::AreEqual(0u, server->Receive(got.data(), static_cast<std::uint32_t>(got.size())), L"the ring held more than it had room for");
    Assert::AreEqual(1u, server->DroppedCount(), L"the drop count moved after the ring drained");
  }

  TEST_METHOD(NothingIsDeliveredOutsidePoll)
  {
    // The threading rule as a test (AGENTS.md 5, Transport.h:46). MsQuic's worker has the datagram
    // within a millisecond on localhost; the owning thread must not see it until it asks.
    constexpr std::uint32_t QUIET_MS = 50;
    Pair pair;
    pair.RequireConnected();

    Assert::IsTrue(SendByte(pair.Client(), 7), L"the send failed");
    pair.SpinWithoutPolling(QUIET_MS);

    Neuron::QuicTransport* const server = pair.Server();
    Buffer got{};
    Assert::AreEqual(0u, server->Receive(got.data(), static_cast<std::uint32_t>(got.size())),
                     L"a datagram was delivered by a worker thread rather than by Poll");

    server->Poll();
    Assert::AreEqual(1u, server->Receive(got.data(), static_cast<std::uint32_t>(got.size())), L"one Poll did not deliver what had arrived");
    Assert::AreEqual(7, static_cast<int>(got[0]), L"the payload changed in flight");
  }

  TEST_METHOD(AClosedPeerIsReportedRatherThanGoingSilent)
  {
    // A peer that goes away has to be visible rather than silent, or the composition root has
    // nothing to report. What makes it visible changed with ADR 0031: the listener recycles the
    // accepted end the moment its connection closes, so the report is the transport leaving
    // Accepted() and RecycledCount rising -- not a state read off a pointer the pool has taken back.
    // Reading the state there would race the recycle and, once recycled, would say Disconnected.
    Pair pair;
    pair.RequireConnected();

    Neuron::QuicTransport* const server = pair.Server();
    Assert::IsNotNull(server, L"the pair reported connected without an accepted end");

    pair.Client().Close();

    // Closed is still a state the client half reaches and reports, which is the half of the original
    // guarantee that does not belong to the listener.
    Assert::IsTrue(pair.Client().State() == Neuron::ConnectionState::Closed, L"a closed end did not report Closed");

    Assert::IsTrue(pair.PumpUntil([&] { return pair.RecycledCount() == 1; }, Neuron::QUIC_IDLE_TIMEOUT_MS),
                   L"the listener never reported that the peer had gone");
    Assert::IsNull(pair.Server(), L"a departed connection was still listed as accepted");
    Assert::IsFalse(SendByte(*server, 1), L"a departed end accepted a datagram");
  }

  TEST_METHOD(ASlotIsRecycledWhenItsClientLeaves)
  {
    // The defect this retires: the listener used to count accepts and never give a slot back, so
    // backlog was a budget for the life of the process rather than a concurrency limit. A server
    // that had seen backlog logins refused everybody until restart, silently (ADR 0031, review E3).
    Neuron::QuicApi api;
    Neuron::QuicApi::Desc apiDesc;
    apiDesc.allowUnvalidatedPeer = true;
    Assert::IsTrue(api.Open(apiDesc), Widen(api.Reason()).c_str());

    // Its own, because Pair's clock belongs to Pair and these two tests drive a bare listener.
    Neuron::FrameClock clock;
    Neuron::QuicListener listener;
    Neuron::QuicListener::Desc listenerDesc;
    listenerDesc.backlog = 1; // one at a time, and this test connects three times through it
    Assert::IsTrue(listener.Start(api, 0, listenerDesc), Widen(listener.Reason()).c_str());

    for (int round = 0; round < 3; ++round)
    {
      Neuron::QuicTransport client;
      Assert::IsTrue(client.Connect(api, {"127.0.0.1", listener.Port()}, {}), Widen(client.Reason()).c_str());

      const std::int64_t start = clock.Now();
      bool up = false;
      while (clock.ElapsedMs(start, clock.Now()) < static_cast<float>(Neuron::QUIC_HANDSHAKE_TIMEOUT_MS))
      {
        listener.Poll();
        client.Poll();
        if (!listener.Accepted().empty())
          listener.Accepted()[0]->Poll();
        if (!listener.Accepted().empty() && client.State() == Neuron::ConnectionState::Connected)
        {
          up = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      Assert::IsTrue(up, L"a connection did not come up on a recycled slot");

      // The client leaves. The listener has to notice and take its slot back, or the next round
      // finds the backlog exhausted.
      client.Close();
      const std::int64_t left = clock.Now();
      while (!listener.Accepted().empty() && clock.ElapsedMs(left, clock.Now()) < static_cast<float>(Neuron::QUIC_HANDSHAKE_TIMEOUT_MS))
      {
        listener.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      Assert::IsTrue(listener.Accepted().empty(), L"a closed connection was still listed as accepted");
    }

    Assert::AreEqual(3u, listener.RecycledCount(), L"the slot was not recycled once per client");
    listener.Stop();
    api.Close();
  }

  TEST_METHOD(AFullBacklogRefusesAndRecovers)
  {
    // The other half: while every slot is carrying a connection the listener refuses, and it starts
    // accepting again the moment one is given back. Refusing is correct; refusing for ever is not.
    Neuron::QuicApi api;
    Neuron::QuicApi::Desc apiDesc;
    apiDesc.allowUnvalidatedPeer = true;
    Assert::IsTrue(api.Open(apiDesc), Widen(api.Reason()).c_str());

    // Its own, because Pair's clock belongs to Pair and these two tests drive a bare listener.
    Neuron::FrameClock clock;
    Neuron::QuicListener listener;
    Neuron::QuicListener::Desc listenerDesc;
    listenerDesc.backlog = 1;
    Assert::IsTrue(listener.Start(api, 0, listenerDesc), Widen(listener.Reason()).c_str());

    Neuron::QuicTransport first;
    Assert::IsTrue(first.Connect(api, {"127.0.0.1", listener.Port()}, {}), Widen(first.Reason()).c_str());
    const std::int64_t start = clock.Now();
    while (listener.Accepted().empty() && clock.ElapsedMs(start, clock.Now()) < static_cast<float>(Neuron::QUIC_HANDSHAKE_TIMEOUT_MS))
    {
      listener.Poll();
      first.Poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Assert::IsFalse(listener.Accepted().empty(), L"the first connection was never accepted");
    Assert::AreEqual(0u, listener.RecycledCount(), L"nothing had left yet");

    // A second dial while the one slot is busy. Connect itself succeeds -- it only starts a
    // handshake -- and the listener's refusal is what stops it reaching Connected.
    {
      Neuron::QuicTransport second;
      Assert::IsTrue(second.Connect(api, {"127.0.0.1", listener.Port()}, {}), Widen(second.Reason()).c_str());
      const std::int64_t busy = clock.Now();
      while (clock.ElapsedMs(busy, clock.Now()) < 250.0f)
      {
        listener.Poll();
        second.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      Assert::AreEqual(static_cast<std::size_t>(1), listener.Accepted().size(), L"a full backlog accepted a second connection");
      second.Close();
    }

    first.Close();
    const std::int64_t left = clock.Now();
    while (!listener.Accepted().empty() && clock.ElapsedMs(left, clock.Now()) < static_cast<float>(Neuron::QUIC_HANDSHAKE_TIMEOUT_MS))
    {
      listener.Poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Assert::AreEqual(1u, listener.RecycledCount(), L"the slot was not given back after its client left");

    listener.Stop();
    api.Close();
  }

  TEST_METHOD(ARefusedListenerReportsWhy)
  {
    // The fallback in Design/QuicTransport.md 6 depends on this being a diagnostic. A listener that
    // asserted on a taken port would take the game down over a number nobody chose deliberately.
    Neuron::QuicApi api;
    Neuron::QuicApi::Desc apiDesc;
    apiDesc.allowUnvalidatedPeer = true;
    Assert::IsTrue(api.Open(apiDesc), Widen(api.Reason()).c_str());

    Neuron::QuicListener first;
    Assert::IsTrue(first.Start(api, 0, {}), Widen(first.Reason()).c_str());

    Neuron::QuicListener second;
    Assert::IsFalse(second.Start(api, first.Port(), {}), L"two listeners bound the same port");
    Assert::IsTrue(second.Reason()[0] != '\0', L"a refused listener gave no reason");

    second.Stop();
    first.Stop();
    api.Close();
  }

  TEST_METHOD(ACredentialIsAcquiredWithoutAStore)
  {
    // No install step, no elevated prompt, no certificate written anywhere -- because every one of
    // those is a step a fresh clone or a CI runner would fail on (ADR 0023). The second Acquire has
    // to find the key the first one persisted, or every boot leaves another key behind.
    Neuron::DevCertificate first;
    Assert::IsTrue(first.Acquire(), Widen(first.Reason()).c_str());
    Assert::IsNotNull(first.Context(), L"Acquire reported success without a certificate");
    Logger::WriteMessage(first.KeyWasCreated() ? L"the development key was generated by this run\n"
                                               : L"the development key was already on this machine\n");

    Neuron::DevCertificate second;
    Assert::IsTrue(second.Acquire(), Widen(second.Reason()).c_str());
    Assert::IsFalse(second.KeyWasCreated(), L"the second Acquire generated a key instead of reusing the persisted one");
  }
};
} // namespace NeuronCoreTests
