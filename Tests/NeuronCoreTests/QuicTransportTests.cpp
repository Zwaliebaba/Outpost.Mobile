#include "pch.h"

// Not reached through the umbrella: NeuronCore.h deliberately does not carry the credential, so that
// wincrypt and ncrypt stay out of every translation unit that only wants a Transport
// (Design/Archive/QuicTransport-slice-1.md 2.5). The test that decides the credential works has to
// name it, so it names it here.
#include "DevCertificate.h"

#include <chrono>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NeuronCoreTests
{
namespace
{
// The size a caller must always offer, since it is the most a datagram can be.
using Buffer = std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES>;

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
// EVERY WAIT IN THIS FILE IS BOUNDED. A handshake that never completes has to fail the test in
// seconds with the states it saw, not sit until CI's timeout kills the run and says nothing.
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

  TEST_METHOD(AClosedPeerDrainsThenCloses)
  {
    // Two of the five ConnectionStates the loopback never used. A peer that goes away has to become
    // visible as a state rather than as a silence, or the composition root has nothing to report.
    Pair pair;
    pair.RequireConnected();

    Neuron::QuicTransport* const server = pair.Server();
    pair.Client().Close();

    Assert::IsTrue(pair.PumpUntil([&] { return server->State() == Neuron::ConnectionState::Closed; }, Neuron::QUIC_IDLE_TIMEOUT_MS),
                   L"the accepted end never noticed its peer had gone");
    Assert::IsFalse(SendByte(*server, 1), L"a closed end accepted a datagram");
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
