#include "port/netplay_transport.hpp"
#include "port/rollback.hpp"

#include "dolphin/os.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace partyboard::netplay {

// The transport seats exactly as many players as the simulation does. If these
// ever drift, a packet is accepted for a seat the rollback session has no slot
// for, and the failure appears somewhere far from here.
static_assert(kMaxNetplayPeers == rollback::kMaxPlayers, "netplay peers and rollback seats must agree");
namespace {

constexpr std::size_t kPacketSize = kNetplayPacketSize;
constexpr std::array<std::uint8_t, 4> kMagic { 'P', 'B', 'R', 'B' };


void put16(std::uint8_t *destination, std::uint16_t value)
{
    destination[0] = static_cast<std::uint8_t>(value >> 8);
    destination[1] = static_cast<std::uint8_t>(value);
}

void put32(std::uint8_t *destination, std::uint32_t value)
{
    destination[0] = static_cast<std::uint8_t>(value >> 24);
    destination[1] = static_cast<std::uint8_t>(value >> 16);
    destination[2] = static_cast<std::uint8_t>(value >> 8);
    destination[3] = static_cast<std::uint8_t>(value);
}

std::uint16_t get16(const std::uint8_t *source)
{
    return static_cast<std::uint16_t>((source[0] << 8) | source[1]);
}

std::uint32_t get32(const std::uint8_t *source)
{
    return (static_cast<std::uint32_t>(source[0]) << 24)
        | (static_cast<std::uint32_t>(source[1]) << 16)
        | (static_cast<std::uint32_t>(source[2]) << 8)
        | static_cast<std::uint32_t>(source[3]);
}

std::uint32_t packetHash(const std::uint8_t *bytes, std::size_t size)
{
    std::uint32_t hash = 2166136261u;
    for (std::size_t index = 0; index < size; ++index) {
        hash = (hash ^ bytes[index]) * 16777619u;
    }
    return hash;
}

std::size_t packetSizeFor(std::size_t seats)
{
    return kPacketSize + (seats - 1) * kNetplayExtraSeatSize;
}
constexpr std::size_t kMaxPacketSize = kPacketSize + (kMaxNetplayPeers - 1) * kNetplayExtraSeatSize;

void putInput(std::uint8_t *destination, const PartyBoardRollbackInput &input)
{
    put16(destination, input.buttons);
    destination[2] = static_cast<std::uint8_t>(input.stickX);
    destination[3] = static_cast<std::uint8_t>(input.stickY);
    destination[4] = static_cast<std::uint8_t>(input.substickX);
    destination[5] = static_cast<std::uint8_t>(input.substickY);
    destination[6] = input.triggerLeft;
    destination[7] = input.triggerRight;
}

PartyBoardRollbackInput getInput(const std::uint8_t *source)
{
    PartyBoardRollbackInput input {};
    input.buttons = get16(source);
    input.stickX = static_cast<s8>(source[2]);
    input.stickY = static_cast<s8>(source[3]);
    input.substickX = static_cast<s8>(source[4]);
    input.substickY = static_cast<s8>(source[5]);
    input.triggerLeft = source[6];
    input.triggerRight = source[7];
    return input;
}

// Protocol 11 Control packet: bytes 0-7 as any packet (magic, version, type 4,
// author seat), 8-11 session, 12-15 sequence, 16-17 payload length, the payload,
// then the FNV checksum.
constexpr std::size_t kControlHeadSize = 18;
std::vector<std::uint8_t> encodeControl(const InputPacket &packet)
{
    const std::size_t length = (std::min)(packet.control.size(), kNetplayMaxControlSize);
    std::vector<std::uint8_t> bytes(kControlHeadSize + length + 4);
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
    put16(bytes.data() + 4, kProtocolVersion);
    bytes[6] = static_cast<std::uint8_t>(PacketType::Control);
    bytes[7] = packet.player;
    put32(bytes.data() + 8, packet.sessionId);
    put32(bytes.data() + 12, packet.sequence);
    put16(bytes.data() + 16, static_cast<std::uint16_t>(length));
    std::copy_n(packet.control.begin(), length, bytes.begin() + kControlHeadSize);
    const std::size_t body = bytes.size() - 4;
    put32(bytes.data() + body, packetHash(bytes.data(), body));
    return bytes;
}

bool decodeControl(const std::uint8_t *bytes, std::size_t size, InputPacket &packet)
{
    if (size < kControlHeadSize + 4) return false;
    const std::size_t length = get16(bytes + 16);
    if (length > kNetplayMaxControlSize || size != kControlHeadSize + length + 4
        || get32(bytes + size - 4) != packetHash(bytes, size - 4) || bytes[7] >= kMaxNetplayPeers) {
        return false;
    }
    packet = InputPacket {};
    packet.type = PacketType::Control;
    packet.player = bytes[7];
    packet.sessionId = get32(bytes + 8);
    packet.sequence = get32(bytes + 12);
    packet.control.assign(bytes + kControlHeadSize, bytes + kControlHeadSize + length);
    return true;
}

// Protocol 10 layout: bytes 0-147 as in protocol 9 (the lead seat's input and
// pause word included), byte 148 the seat count, 149 the lead seat's command
// (protocol 11), 150-151 zero, then 16 bytes per further seat (8 input + 4 pause
// + 1 command + 3 zero), then the FNV checksum of everything before it.
std::vector<std::uint8_t> encode(const InputPacket &packet)
{
    if (packet.type == PacketType::Control) return encodeControl(packet);
    const std::size_t seats = std::clamp<std::size_t>(packet.seats, 1, kMaxNetplayPeers);
    std::vector<std::uint8_t> bytes(packetSizeFor(seats));
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
    put16(bytes.data() + 4, kProtocolVersion);
    bytes[6] = static_cast<std::uint8_t>(packet.type);
    bytes[7] = packet.player;
    put32(bytes.data() + 8, packet.sessionId);
    put32(bytes.data() + 12, packet.sequence);
    put32(bytes.data() + 16, packet.frame);
    putInput(bytes.data() + 20, packet.input);
    put32(bytes.data() + 28, packet.configSignature);
    put32(bytes.data() + 32, packet.frandSeed);
    put32(bytes.data() + 36, packet.rand8Seed);
    put32(bytes.data() + 40, packet.hashAckNext);
    put32(bytes.data() + 44, packet.captureContext);
    put32(bytes.data() + 48, packet.state.version);
    put32(bytes.data() + 52, packet.state.frame);
    put32(bytes.data() + 56, packet.state.context);
    put32(bytes.data() + 60, packet.state.frand);
    put32(bytes.data() + 64, packet.state.rand8);
    put32(bytes.data() + 68, packet.state.counter);
    put32(bytes.data() + 72, static_cast<std::uint32_t>(packet.state.hash >> 32));
    put32(bytes.data() + 76, static_cast<std::uint32_t>(packet.state.hash));
    put32(bytes.data() + 80, packet.pause);
    for (std::size_t index = 0; index < kSubsystemCount; ++index)
        put32(bytes.data() + 84 + index * 4, packet.state.parts[index]);
    bytes[148] = static_cast<std::uint8_t>(seats);
    bytes[149] = packet.command[0];
    for (std::size_t extra = 1; extra < seats; ++extra) {
        std::uint8_t *at = bytes.data() + 152 + (extra - 1) * kNetplayExtraSeatSize;
        putInput(at, packet.extraInput[extra - 1]);
        put32(at + 8, packet.extraPause[extra - 1]);
        at[12] = packet.command[extra];
    }
    const std::size_t body = bytes.size() - 4;
    put32(bytes.data() + body, packetHash(bytes.data(), body));
    return bytes;
}

bool decode(const std::uint8_t *bytes, std::size_t size, InputPacket &packet)
{
    if (size >= 8 && std::equal(kMagic.begin(), kMagic.end(), bytes) && get16(bytes + 4) == kProtocolVersion
        && bytes[6] == static_cast<std::uint8_t>(PacketType::Control)) {
        return decodeControl(bytes, size, packet);
    }
    if (size < kPacketSize || size > kMaxPacketSize || !std::equal(kMagic.begin(), kMagic.end(), bytes)
        || get16(bytes + 4) != kProtocolVersion || bytes[6] < 1 || bytes[6] > 3
        || get32(bytes + 48) != kStateHashVersion) {
        return false;
    }
    const std::size_t seats = bytes[148];
    if (seats < 1 || seats > kMaxNetplayPeers || size != packetSizeFor(seats)
        || bytes[150] != 0 || bytes[151] != 0
        || get32(bytes + size - 4) != packetHash(bytes, size - 4)) {
        return false;
    }
    packet.player = bytes[7];
    packet.sessionId = get32(bytes + 8);
    packet.sequence = get32(bytes + 12);
    packet.frame = get32(bytes + 16);
    packet.input = getInput(bytes + 20);
    packet.configSignature = get32(bytes + 28);
    packet.frandSeed = get32(bytes + 32);
    packet.rand8Seed = get32(bytes + 36);
    packet.type = static_cast<PacketType>(bytes[6]);
    packet.hashAckNext = get32(bytes + 40);
    packet.pause = get32(bytes + 80);
    packet.state.version = get32(bytes + 48);
    packet.state.frame = get32(bytes + 52);
    packet.state.context = get32(bytes + 56);
    packet.state.frand = get32(bytes + 60);
    packet.state.rand8 = get32(bytes + 64);
    packet.state.counter = get32(bytes + 68);
    packet.state.hash = (static_cast<std::uint64_t>(get32(bytes + 72)) << 32) | get32(bytes + 76);
    for (std::size_t index = 0; index < kSubsystemCount; ++index)
        packet.state.parts[index] = get32(bytes + 84 + index * 4);
    packet.captureContext = get32(bytes + 44);
    packet.seats = static_cast<std::uint8_t>(seats);
    packet.extraInput = {};
    packet.extraPause = {};
    packet.command = {};
    packet.control.clear();
    packet.command[0] = bytes[149];
    for (std::size_t extra = 1; extra < seats; ++extra) {
        const std::uint8_t *at = bytes + 152 + (extra - 1) * kNetplayExtraSeatSize;
        if (at[13] != 0 || at[14] != 0 || at[15] != 0) return false;
        packet.extraInput[extra - 1] = getInput(at);
        packet.extraPause[extra - 1] = get32(at + 8);
        packet.command[extra] = at[12];
    }
    // The author byte indexes seat arrays, so it is bounded here and nowhere else.
    // It said "< 2" -- the single line that made a third player impossible at the
    // wire level, whatever the rest of the stack could do. The seats it speaks
    // for must fit after it too.
    return packet.player < kMaxNetplayPeers && packet.player + seats <= kMaxNetplayPeers
        && (packet.type != PacketType::State || packet.state.frame != kNoHashFrame);
}

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void closeSocket(Socket socket) { closesocket(socket); }
int lastSocketError() { return WSAGetLastError(); }
bool wouldBlock(int error) { return error == WSAEWOULDBLOCK; }
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void closeSocket(Socket socket) { ::close(socket); }
int lastSocketError() { return errno; }
bool wouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
#endif

}

struct UdpTransport::Impl {
    struct Peer {
        sockaddr_in address {};
        bool valid = false;
        // Ping bookkeeping (see peerRoundTripMs).
        std::chrono::steady_clock::time_point lastPingSent {};
        std::chrono::steady_clock::time_point lastPong {};
        double rttMs = -1.0;
    };
    Socket socket = kInvalidSocket;
    std::array<Peer, kMaxNetplayPeers> peers {};
    std::uint8_t lastSender = kMaxNetplayPeers;
    std::size_t expectedPeers = 1;
    bool discoverPeer = false;
#if defined(_WIN32)
    bool winsockStarted = false;
#endif
    std::uint16_t port = 0;
    std::string error;
    // Protocol versions already reported as refused (one line per version).
    std::array<std::uint16_t, 4> refusedVersions {};

    // Test aid, off unless PARTYBOARD_NETPLAY_TEST_IMPAIR=<delay ms>:<jitter ms>:<loss %>
    // is set: received datagrams are held for delay + random(0..jitter) ms (so they
    // can arrive out of order) or dropped, inside this copy only. Exercises the
    // lockstep/rollback paths with a bad network without touching any system
    // network setting. Its random numbers never touch the game's.
    struct Held {
        std::chrono::steady_clock::time_point due;
        std::array<std::uint8_t, 1500> bytes {};
        int size = 0;
        sockaddr_in sender {};
    };
    bool impair = false;
    int impairDelayMs = 0, impairJitterMs = 0, impairLossPercent = 0;
    std::deque<Held> held;
    std::mt19937 impairRandom {12345u};
    unsigned long long impairDropped = 0, impairPassed = 0;
};

UdpTransport::UdpTransport()
    : mImpl(std::make_unique<Impl>())
{
}

UdpTransport::~UdpTransport() { close(); }
UdpTransport::UdpTransport(UdpTransport &&) noexcept = default;
UdpTransport &UdpTransport::operator=(UdpTransport &&other) noexcept
{
    if (this != &other) {
        close();
        mImpl = std::move(other.mImpl);
    }
    return *this;
}

bool UdpTransport::open(std::uint16_t localPort, bool loopbackOnly)
{
    close();
    if (!mImpl) mImpl = std::make_unique<Impl>();
    mImpl->error.clear();
#if defined(_WIN32)
    WSADATA data {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        mImpl->error = "WSAStartup failed";
        return false;
    }
    mImpl->winsockStarted = true;
#endif
    mImpl->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (mImpl->socket == kInvalidSocket) {
        mImpl->error = "socket creation failed: " + std::to_string(lastSocketError());
        close();
        return false;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    // The Tailscale lobby (port/online/tailnet_lobby.cpp) names this player's
    // Tailscale address, so the game port is not open on every network the
    // computer is on -- only on the private one the session uses.
    if (!loopbackOnly) {
        if (const char *bind = std::getenv("PARTYBOARD_NETPLAY_BIND"); bind != nullptr && *bind != '\0') {
            in_addr chosen {};
            if (inet_pton(AF_INET, bind, &chosen) == 1) {
                address.sin_addr = chosen;
            }
        }
    }
    address.sin_port = htons(localPort);
    if (::bind(mImpl->socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        mImpl->error = "UDP bind failed: " + std::to_string(lastSocketError());
        close();
        return false;
    }

#if defined(_WIN32)
    u_long nonBlocking = 1;
    if (ioctlsocket(mImpl->socket, FIONBIO, &nonBlocking) != 0) {
#else
    if (fcntl(mImpl->socket, F_SETFL, fcntl(mImpl->socket, F_GETFL, 0) | O_NONBLOCK) != 0) {
#endif
        mImpl->error = "failed to make UDP socket non-blocking";
        close();
        return false;
    }

    sockaddr_in bound {};
#if defined(_WIN32)
    int boundSize = sizeof(bound);
#else
    socklen_t boundSize = sizeof(bound);
#endif
    if (getsockname(mImpl->socket, reinterpret_cast<sockaddr *>(&bound), &boundSize) != 0) {
        mImpl->error = "getsockname failed";
        close();
        return false;
    }
    mImpl->port = ntohs(bound.sin_port);
    if (const char *impair = std::getenv("PARTYBOARD_NETPLAY_TEST_IMPAIR"); impair != nullptr && *impair != '\0') {
        int delay = 0, jitter = 0, loss = 0;
        if (std::sscanf(impair, "%d:%d:%d", &delay, &jitter, &loss) >= 1 && delay >= 0 && jitter >= 0 && loss >= 0 && loss < 100) {
            mImpl->impair = true;
            mImpl->impairDelayMs = delay;
            mImpl->impairJitterMs = jitter;
            mImpl->impairLossPercent = loss;
            mImpl->impairRandom.seed(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ mImpl->port);
            std::fprintf(stdout, "Netplay: TEST network impairment on received packets: delay %d ms + up to %d ms jitter, %d%% lost.\n",
                delay, jitter, loss);
            std::fflush(stdout);
        } else {
            std::fprintf(stdout, "Netplay: TEST network impairment ignored, expected <delay ms>:<jitter ms>:<loss %%>: %s\n", impair);
        }
    }
    return true;
}

bool UdpTransport::addPeer(std::uint8_t player, const std::string &ipv4Address, std::uint16_t port)
{
    if (player >= kMaxNetplayPeers) {
        mImpl->error = "peer seat out of range";
        return false;
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, ipv4Address.c_str(), &address.sin_addr) != 1) {
        mImpl->error = "invalid IPv4 peer address";
        return false;
    }
    mImpl->peers[player].address = address;
    mImpl->peers[player].valid = true;
    return true;
}

bool UdpTransport::setPeer(const std::string &ipv4Address, std::uint16_t port)
{
    for (std::size_t seat = 0; seat < mImpl->peers.size(); ++seat) {
        if (!mImpl->peers[seat].valid) {
            return addPeer(static_cast<std::uint8_t>(seat), ipv4Address, port);
        }
    }
    mImpl->error = "no free peer seat";
    return false;
}

void UdpTransport::enablePeerDiscovery(bool enabled)
{
    mImpl->discoverPeer = enabled;
}

void UdpTransport::expectPeers(std::size_t count)
{
    mImpl->expectedPeers = std::clamp<std::size_t>(count, 1, kMaxNetplayPeers);
}

bool UdpTransport::sendInputTo(std::uint8_t player, const InputPacket &packet)
{
    if (mImpl->socket == kInvalidSocket || player >= kMaxNetplayPeers || !mImpl->peers[player].valid) {
        mImpl->error = "UDP transport is not connected";
        return false;
    }
    const auto bytes = encode(packet);
    const int sent = sendto(mImpl->socket, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
        reinterpret_cast<const sockaddr *>(&mImpl->peers[player].address), sizeof(sockaddr_in));
    // The ping rides beside the game packets of every link, about once a second. It used to
    // be sent only from sendInput(), which the session (it sends to each peer with its own
    // acknowledgements) never calls: no peer ever answered, and the ping box showed "--".
    sendPingIfDue(player);
    if (sent != static_cast<int>(bytes.size())) {
        mImpl->error = "UDP send failed: " + std::to_string(lastSocketError());
        return false;
    }
    return true;
}

// ---- Ping: 16-byte datagrams beside the game packets, for the online ping
// display. "PBPG", kind (1 ping, 2 pong), three zero bytes, then the pinger's
// own steady-clock timestamp in nanoseconds, echoed back unchanged by the pong
// so no clock agreement is needed. decode() rejects them by magic and size, so
// an older build simply ignores them.
namespace {
constexpr std::size_t kPingSize = 16;
constexpr std::array<std::uint8_t, 4> kPingMagic { 'P', 'B', 'P', 'G' };
constexpr auto kPingInterval = std::chrono::seconds(1);
constexpr auto kPingStale = std::chrono::seconds(3);

std::int64_t steady_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::array<std::uint8_t, kPingSize> ping_bytes(std::uint8_t kind, std::int64_t stamp)
{
    std::array<std::uint8_t, kPingSize> bytes {};
    std::copy(kPingMagic.begin(), kPingMagic.end(), bytes.begin());
    bytes[4] = kind;
    for (int i = 0; i < 8; ++i) {
        bytes[8 + i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(stamp) >> (56 - 8 * i));
    }
    return bytes;
}

std::int64_t ping_stamp(const std::uint8_t *bytes)
{
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | bytes[8 + i];
    }
    return static_cast<std::int64_t>(value);
}
} // namespace

void UdpTransport::sendPingIfDue(std::uint8_t player)
{
    auto &peer = mImpl->peers[player];
    const auto now = std::chrono::steady_clock::now();
    if (now - peer.lastPingSent < kPingInterval) return;
    peer.lastPingSent = now;
    const auto ping = ping_bytes(1, steady_ns());
    sendto(mImpl->socket, reinterpret_cast<const char *>(ping.data()), static_cast<int>(ping.size()), 0,
        reinterpret_cast<const sockaddr *>(&peer.address), sizeof(sockaddr_in));
}

bool UdpTransport::sendInput(const InputPacket &packet)
{
    if (mImpl->socket == kInvalidSocket) {
        mImpl->error = "UDP transport is not connected";
        return false;
    }
    // One unreachable guest must not stop the frame reaching the other two, so a
    // send counts as done when it reached anybody. The caller learns nothing about
    // which link failed; that is the session layer's job, from missing inputs.
    bool any = false;
    bool anyPeer = false;
    for (std::size_t seat = 0; seat < mImpl->peers.size(); ++seat) {
        if (!mImpl->peers[seat].valid) continue;
        anyPeer = true;
        if (sendInputTo(static_cast<std::uint8_t>(seat), packet)) any = true;   // (pings too)
    }
    if (!anyPeer) {
        mImpl->error = "UDP transport is not connected";
        return false;
    }
    return any;
}

bool UdpTransport::receiveInput(InputPacket &packet)
{
    if (!mImpl || mImpl->socket == kInvalidSocket) return false;
    // Pings are answered or measured here and never returned; keep reading so
    // a ping does not end the caller's drain of the socket for this frame.
    for (;;) {
    std::array<std::uint8_t, 1500> bytes {};
    sockaddr_in sender {};
#if defined(_WIN32)
    int senderSize = sizeof(sender);
#else
    socklen_t senderSize = sizeof(sender);
#endif
    int received = 0;
    if (!mImpl->impair) {
        received = recvfrom(mImpl->socket, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
            reinterpret_cast<sockaddr *>(&sender), &senderSize);
    } else {
        // Drain the socket into the hold queue, then hand out the first datagram that is due.
        const auto now = std::chrono::steady_clock::now();
        for (;;) {
            Impl::Held item;
#if defined(_WIN32)
            int itemSize = sizeof(item.sender);
#else
            socklen_t itemSize = sizeof(item.sender);
#endif
            item.size = recvfrom(mImpl->socket, reinterpret_cast<char *>(item.bytes.data()), static_cast<int>(item.bytes.size()), 0,
                reinterpret_cast<sockaddr *>(&item.sender), &itemSize);
            if (item.size < 0) break;
            if (static_cast<int>(mImpl->impairRandom() % 100u) < mImpl->impairLossPercent) {
                ++mImpl->impairDropped;
                continue;
            }
            const int extra = mImpl->impairJitterMs > 0 ? static_cast<int>(mImpl->impairRandom() % static_cast<unsigned>(mImpl->impairJitterMs + 1)) : 0;
            item.due = now + std::chrono::milliseconds(mImpl->impairDelayMs + extra);
            mImpl->held.push_back(item);
        }
        auto due = std::min_element(mImpl->held.begin(), mImpl->held.end(), [](const Impl::Held &a, const Impl::Held &b) { return a.due < b.due; });
        if (due == mImpl->held.end() || due->due > now) {
            return false;
        }
        received = due->size;
        std::copy(due->bytes.begin(), due->bytes.begin() + std::min<std::size_t>(due->bytes.size(), bytes.size()), bytes.begin());
        sender = due->sender;
        mImpl->held.erase(due);
        if ((++mImpl->impairPassed % 20000u) == 0) {
            std::fprintf(stdout, "Netplay: TEST network impairment: %llu packets passed, %llu dropped.\n", mImpl->impairPassed, mImpl->impairDropped);
            std::fflush(stdout);
        }
    }
    if (received < 0) {
        const int error = lastSocketError();
        if (wouldBlock(error)) {
            return false;
        }
        mImpl->error = "UDP receive failed: " + std::to_string(error);
        return false;
    }
    // Which registered seat this came from. A packet from nobody we know is
    // dropped unless discovery is on, exactly as before -- the difference is that
    // "nobody we know" is now a search rather than a single comparison.
    std::uint8_t from = kMaxNetplayPeers;
    for (std::size_t seat = 0; seat < mImpl->peers.size(); ++seat) {
        const auto &peer = mImpl->peers[seat];
        if (peer.valid && peer.address.sin_addr.s_addr == sender.sin_addr.s_addr
            && peer.address.sin_port == sender.sin_port) {
            from = static_cast<std::uint8_t>(seat);
            break;
        }
    }
    const bool known = from < kMaxNetplayPeers;
    if (received == static_cast<int>(kPingSize) && std::equal(kPingMagic.begin(), kPingMagic.end(), bytes.data())) {
        if (known && bytes[4] == 1) {
            const auto pong = ping_bytes(2, ping_stamp(bytes.data()));
            sendto(mImpl->socket, reinterpret_cast<const char *>(pong.data()), static_cast<int>(pong.size()), 0,
                reinterpret_cast<const sockaddr *>(&sender), sizeof(sockaddr_in));
        } else if (known && bytes[4] == 2) {
            const double sample = static_cast<double>(steady_ns() - ping_stamp(bytes.data())) / 1e6;
            if (sample >= 0.0 && sample < 10000.0) {
                auto &peer = mImpl->peers[from];
                peer.rttMs = peer.rttMs < 0.0 ? sample : peer.rttMs * 0.7 + sample * 0.3;
                peer.lastPong = std::chrono::steady_clock::now();
            }
        }
        continue;
    }
    if (!known && !mImpl->discoverPeer) {
        return false;
    }
    // Any size: protocol 9 packets were 152 bytes, protocol 10 ones 156 or more.
    if (received >= 8 && std::equal(kMagic.begin(), kMagic.end(), bytes.data())
        && get16(bytes.data() + 4) != kProtocolVersion) {
        // A PartyBoard copy of another version. Refused, and said once per version:
        // silently dropping it looked exactly like a lost connection.
        const std::uint16_t version = get16(bytes.data() + 4);
        auto &seen = mImpl->refusedVersions;
        if (std::find(seen.begin(), seen.end(), version) == seen.end()) {
            const auto slot = std::find(seen.begin(), seen.end(), std::uint16_t {0});
            if (slot != seen.end()) *slot = version;
            char address[INET_ADDRSTRLEN] {};
            inet_ntop(AF_INET, &sender.sin_addr, address, sizeof(address));
            std::fprintf(stdout,
                "[netplay] refused: %s:%u (seat byte %u) speaks netplay protocol %u, this copy speaks %u; "
                "every player needs the same PartyBoard version.\n",
                address, static_cast<unsigned>(ntohs(sender.sin_port)), static_cast<unsigned>(bytes[7]),
                static_cast<unsigned>(version), static_cast<unsigned>(kProtocolVersion));
            std::fflush(stdout);
        }
        return false;
    }
    if (!decode(bytes.data(), static_cast<std::size_t>(received), packet)) {
        return false;
    }
    if (!known) {
        // Discovery closes as soon as the expected peers are seated. Without this a
        // host would keep accepting strangers for the rest of the session; with two
        // players that was prevented by there being exactly one peer to compare to.
        if (peerCount() >= mImpl->expectedPeers) {
            return false;
        }
        // Discovery seats a new peer by the author byte it announced, which decode
        // has already bounded. A host binds one port and learns each guest from its
        // first packet; this is the same trust as the two-player version had, and
        // the same caveat -- the authenticated channel that carries these is the
        // thing that makes it safe, not this function.
        if (mImpl->peers[packet.player].valid) {
            return false;
        }
        mImpl->peers[packet.player].address = sender;
        mImpl->peers[packet.player].valid = true;
        from = packet.player;
    }
    mImpl->lastSender = from;
    return true;
    }
}

int UdpTransport::peerRoundTripMs(std::uint8_t player) const
{
    if (!mImpl || player >= kMaxNetplayPeers) return -1;
    const auto &peer = mImpl->peers[player];
    if (!peer.valid || peer.rttMs < 0.0 || std::chrono::steady_clock::now() - peer.lastPong > kPingStale) return -1;
    return static_cast<int>(peer.rttMs + 0.5);
}

void UdpTransport::close()
{
    if (!mImpl) return;
    if (mImpl->socket != kInvalidSocket) {
        closeSocket(mImpl->socket);
        mImpl->socket = kInvalidSocket;
    }
    mImpl->peers = {};
    mImpl->lastSender = kMaxNetplayPeers;
    mImpl->expectedPeers = 1;
    mImpl->discoverPeer = false;
    mImpl->port = 0;
#if defined(_WIN32)
    if (mImpl->winsockStarted) {
        WSACleanup();
        mImpl->winsockStarted = false;
    }
#endif
}

std::uint16_t UdpTransport::localPort() const { return mImpl->port; }
bool UdpTransport::hasPeer() const { return peerCount() != 0; }
bool UdpTransport::hasPeer(std::uint8_t player) const
{
    return player < kMaxNetplayPeers && mImpl->peers[player].valid;
}
std::size_t UdpTransport::peerCount() const
{
    std::size_t count = 0;
    for (const auto &peer : mImpl->peers) if (peer.valid) ++count;
    return count;
}
std::uint8_t UdpTransport::lastSenderPlayer() const { return mImpl->lastSender; }
const std::string &UdpTransport::lastError() const { return mImpl->error; }

// Every failed check names its line, so a red loopback self-test says which
// step went wrong (a socket, the codec, discovery, the intruder, a delivery)
// instead of a bare FAIL.
bool transportCheckFailed(int line)
{
    OSReport("Netplay UDP loopback self-test: check at netplay_transport.cpp:%d failed\n", line);
    return false;
}

// A real four-player star over loopback: three guests, one host, every guest
// discovered from its own first packet and seated by the author byte it
// announced. This is the shape the lobby will drive, and it is checked here
// because everything above it is easier to debug once this is known good.
//
// It also pins the two properties the two-player code got for free and that a
// peer table has to earn back: a stranger is refused once the expected guests
// are seated, and the host can tell which link a packet arrived on rather than
// trusting the author byte, which is the only way a relay avoids echoing a
// packet back to its sender.
bool runStarSelfTest()
{
    UdpTransport host;
    if (!host.open()) return transportCheckFailed(__LINE__);
    host.enablePeerDiscovery(true);
    host.expectPeers(3);

    std::array<UdpTransport, 3> guests;
    for (std::size_t index = 0; index < guests.size(); ++index) {
        if (!guests[index].open() || !guests[index].setPeer("127.0.0.1", host.localPort())) return transportCheckFailed(__LINE__);
    }

    // Each guest announces itself once; the host must end up with three distinct
    // seats, in the seats the guests claimed rather than in arrival order.
    for (std::size_t index = 0; index < guests.size(); ++index) {
        InputPacket hello {};
        hello.sessionId = 0x4D503452u;
        hello.frame = 1;
        hello.player = static_cast<std::uint8_t>(index + 1);
        hello.type = PacketType::Input;
        hello.state.frame = kNoHashFrame;
        hello.state.version = kStateHashVersion;
        if (!guests[index].sendInput(hello)) return transportCheckFailed(__LINE__);
    }

    std::array<bool, kMaxNetplayPeers> seen {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    std::size_t received = 0;
    while (received < guests.size() && std::chrono::steady_clock::now() < deadline) {
        InputPacket packet {};
        if (host.receiveInput(packet)) {
            if (packet.player == 0 || packet.player > guests.size()) return transportCheckFailed(__LINE__);
            // Sender seat and announced author agree here because the guests are
            // honest; what matters is that the host can name the link at all.
            if (host.lastSenderPlayer() != packet.player) return transportCheckFailed(__LINE__);
            if (seen[packet.player]) return transportCheckFailed(__LINE__);
            seen[packet.player] = true;
            ++received;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (received != guests.size() || host.peerCount() != guests.size()) return transportCheckFailed(__LINE__);
    for (std::uint8_t seat = 1; seat <= guests.size(); ++seat) {
        if (!host.hasPeer(seat)) return transportCheckFailed(__LINE__);
    }
    if (host.hasPeer(0)) return transportCheckFailed(__LINE__);

    // A fourth socket arriving after the expected guests are seated is refused,
    // exactly as the third socket was refused when there were only two players.
    UdpTransport intruder;
    InputPacket forged {};
    forged.sessionId = 0x4D503452u;
    forged.frame = 2;
    forged.player = 0;
    forged.type = PacketType::Input;
    forged.state.frame = kNoHashFrame;
    forged.state.version = kStateHashVersion;
    if (!intruder.open() || !intruder.setPeer("127.0.0.1", host.localPort())
        || !intruder.sendInput(forged)) return transportCheckFailed(__LINE__);
    const auto rejectDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    while (std::chrono::steady_clock::now() < rejectDeadline) {
        InputPacket packet {};
        if (host.receiveInput(packet)) return transportCheckFailed(__LINE__);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (host.peerCount() != guests.size()) return transportCheckFailed(__LINE__);

    // One broadcast from the host reaches all three guests.
    InputPacket broadcast {};
    broadcast.sessionId = 0x4D503452u;
    broadcast.frame = 3;
    broadcast.player = 0;
    broadcast.sequence = 42;
    broadcast.type = PacketType::Input;
    broadcast.state.frame = kNoHashFrame;
    broadcast.state.version = kStateHashVersion;
    if (!host.sendInput(broadcast)) return transportCheckFailed(__LINE__);
    for (std::size_t index = 0; index < guests.size(); ++index) {
        bool got = false;
        const auto guestDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (!got && std::chrono::steady_clock::now() < guestDeadline) {
            InputPacket packet {};
            if (guests[index].receiveInput(packet)) {
                if (packet.sequence != broadcast.sequence || packet.player != 0) return transportCheckFailed(__LINE__);
                got = true;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (!got) return transportCheckFailed(__LINE__);
    }

    // And a targeted send reaches only the guest it names.
    InputPacket direct = broadcast;
    direct.sequence = 43;
    if (!host.sendInputTo(2, direct)) return transportCheckFailed(__LINE__);
    bool reachedTwo = false;
    const auto directDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < directDeadline) {
        InputPacket packet {};
        if (guests[1].receiveInput(packet) && packet.sequence == direct.sequence) { reachedTwo = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!reachedTwo) return transportCheckFailed(__LINE__);
    for (const std::size_t other : {std::size_t{0}, std::size_t{2}}) {
        InputPacket packet {};
        if (guests[other].receiveInput(packet)) return transportCheckFailed(__LINE__);
    }
    return true;
}

bool runTransportSelfTest()
{
    InputPacket codecSample {};
    codecSample.captureContext = 0xFEDCBA98u;
    auto bytes = encode(codecSample);
    InputPacket decoded {};
    if (!decode(bytes.data(), bytes.size(), decoded)
        || decoded.captureContext != codecSample.captureContext) return transportCheckFailed(__LINE__);
    bytes[44] ^= 1; // Context metadata is covered by the packet checksum.
    if (decode(bytes.data(), bytes.size(), decoded)) return transportCheckFailed(__LINE__);
    bytes = encode(codecSample);
    put16(bytes.data() + 4, 3); // Old timeline protocol, otherwise valid checksum.
    put32(bytes.data() + bytes.size() - 4, packetHash(bytes.data(), bytes.size() - 4));
    if (decode(bytes.data(), bytes.size(), decoded)) return transportCheckFailed(__LINE__);

    for (const auto type : {PacketType::Input, PacketType::Retransmit, PacketType::State}) {
        codecSample.type = type;
        codecSample.hashAckNext = 8;
        codecSample.state = {7, 3, 0x11223344, 0x55667788, 6, kStateHashVersion, 0x0123456789abcdefull};
        // Distinct per-subsystem hashes must survive the round trip intact.
        for (std::size_t index = 0; index < kSubsystemCount; ++index)
            codecSample.state.parts[index] = 0xA5000000u + static_cast<std::uint32_t>(index) * 0x01010101u;
        bytes = encode(codecSample);
        if (!decode(bytes.data(), bytes.size(), decoded) || decoded.type != type
            || decoded.state != codecSample.state || decoded.hashAckNext != 8
            || decoded.state.parts != codecSample.state.parts) return transportCheckFailed(__LINE__);
    }
    // A single altered subsystem hash must be both checksum-detected and, once
    // re-signed, visible as a difference rather than silently equal.
    bytes = encode(codecSample);
    bytes[84 + 4 * static_cast<std::size_t>(Subsystem::Board)] ^= 0x40;
    if (decode(bytes.data(), bytes.size(), decoded)) return transportCheckFailed(__LINE__);
    put32(bytes.data() + bytes.size() - 4, packetHash(bytes.data(), bytes.size() - 4));
    if (!decode(bytes.data(), bytes.size(), decoded)
        || decoded.state == codecSample.state
        || decoded.state.firstDifferentPart(codecSample.state)
            != static_cast<std::size_t>(Subsystem::Board)) return transportCheckFailed(__LINE__);
    // Protocol version, type, player and state-hash version. (Not byte 80: since
    // protocol 8 it is the pause word, where every value is valid.)
    for (const unsigned offset : {5u, 6u, 7u, 48u}) {
        bytes = encode(codecSample);
        bytes[offset] = 99; // Valid checksum must not authorize an unknown schema/type/player.
        put32(bytes.data() + bytes.size() - 4, packetHash(bytes.data(), bytes.size() - 4));
        if (decode(bytes.data(), bytes.size(), decoded)) return transportCheckFailed(__LINE__);
    }
    bytes = encode(codecSample);
    if (decode(bytes.data(), bytes.size() - 1, decoded)) return transportCheckFailed(__LINE__);

    // Protocol 10: a packet for a computer with four seats carries all four,
    // each with its own pause word, and only in that size.
    {
        InputPacket multi = codecSample;
        multi.player = 4;
        multi.seats = 4;
        multi.input = {0x0100, 1, 2, 3, 4, 5, 6};
        multi.pause = 0x80000003u;
        for (std::size_t k = 0; k < 3; ++k) {
            multi.extraInput[k] = {static_cast<std::uint16_t>(0x0200u + k), static_cast<s8>(-10 - static_cast<int>(k)),
                static_cast<s8>(20 + k), 0, 0, static_cast<std::uint8_t>(k), 0};
            multi.extraPause[k] = static_cast<std::uint32_t>(k == 1 ? 0x80000001u : k);
        }
        bytes = encode(multi);
        if (bytes.size() != kPacketSize + 3 * kNetplayExtraSeatSize) return transportCheckFailed(__LINE__);
        InputPacket back {};
        if (!decode(bytes.data(), bytes.size(), back) || back.seats != 4 || back.player != 4
            || !rollback::inputsEqual(back.input, multi.input) || back.pause != multi.pause) return transportCheckFailed(__LINE__);
        for (std::size_t k = 0; k < 3; ++k)
            if (!rollback::inputsEqual(back.extraInput[k], multi.extraInput[k]) || back.extraPause[k] != multi.extraPause[k])
                return transportCheckFailed(__LINE__);
        if (back.command != std::array<std::uint8_t, kMaxNetplayPeers> {}) return transportCheckFailed(__LINE__);
        // Protocol 11: the command bytes of every seat travel with their input.
        {
            InputPacket commands = multi;
            commands.command = {0x85, 0x00, 0x13, 0x80};
            const auto withCommands = encode(commands);
            InputPacket read {};
            if (withCommands.size() != bytes.size() || !decode(withCommands.data(), withCommands.size(), read)
                || read.command[0] != 0x85 || read.command[1] != 0 || read.command[2] != 0x13 || read.command[3] != 0x80)
                return transportCheckFailed(__LINE__);
        }
        // A seat count that does not match the size, or seats past seat 8, never decode.
        auto wrong = bytes;
        wrong[148] = 3;
        put32(wrong.data() + wrong.size() - 4, packetHash(wrong.data(), wrong.size() - 4));
        if (decode(wrong.data(), wrong.size(), back)) return transportCheckFailed(__LINE__);
        multi.player = 6;
        bytes = encode(multi);
        if (decode(bytes.data(), bytes.size(), back)) return transportCheckFailed(__LINE__);
        // One seat: the same 156 bytes as any one-person computer.
        multi.seats = 1;
        if (encode(multi).size() != kPacketSize) return transportCheckFailed(__LINE__);
    }
    // Protocol 11 Control packets: the payload comes back byte for byte; a bad
    // checksum or length never decodes.
    {
        InputPacket control {};
        control.type = PacketType::Control;
        control.sessionId = codecSample.sessionId;
        control.sequence = 991;
        control.player = 4;
        for (unsigned i = 0; i < 300; ++i) control.control.push_back(static_cast<std::uint8_t>(i * 7));
        auto wire = encode(control);
        InputPacket back {};
        if (!decode(wire.data(), wire.size(), back) || back.type != PacketType::Control || back.player != 4
            || back.sequence != 991 || back.control != control.control) return transportCheckFailed(__LINE__);
        wire[40] ^= 1;
        if (decode(wire.data(), wire.size(), back)) return transportCheckFailed(__LINE__);
        wire[40] ^= 1;
        if (decode(wire.data(), wire.size() - 1, back)) return transportCheckFailed(__LINE__);
    }
    codecSample.state.frame = kNoHashFrame;
    bytes = encode(codecSample);
    if (decode(bytes.data(), bytes.size(), decoded)) return transportCheckFailed(__LINE__);

    if (!runStarSelfTest()) return transportCheckFailed(__LINE__);

    UdpTransport host;
    UdpTransport client;
    if (!host.open() || !client.open()
        || !client.setPeer("127.0.0.1", host.localPort())) {
        return transportCheckFailed(__LINE__);
    }
    host.enablePeerDiscovery(true);

    InputPacket expected;
    expected.sessionId = 0x4D503452u;
    expected.sequence = 73;
    expected.frame = 1204;
    expected.player = 1;
    expected.input = { 0x4120, -37, 82, -11, 29, 201, 144 };
    expected.configSignature = 0x4E500002u;
    expected.frandSeed = 0x13579BDFu;
    expected.rand8Seed = 0x2468ACE0u;
    expected.type = PacketType::Retransmit;
    expected.hashAckNext = 97;
    expected.state = {96, 7, 0x10203040u, 0x50607080u, 95, kStateHashVersion, 0x89abcdef01234567ull};
    expected.captureContext = 0x12345678u;
    if (!client.sendInput(expected)) {
        return transportCheckFailed(__LINE__);
    }

    InputPacket actual;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < deadline) {
        if (host.receiveInput(actual)) {
            const bool requestMatches = host.hasPeer()
                && actual.sessionId == expected.sessionId && actual.sequence == expected.sequence
                && actual.frame == expected.frame && actual.player == expected.player
                && actual.configSignature == expected.configSignature
                && actual.frandSeed == expected.frandSeed
                && actual.rand8Seed == expected.rand8Seed
                && actual.type == expected.type && actual.state == expected.state
                && actual.hashAckNext == expected.hashAckNext
                && actual.captureContext == expected.captureContext
                && rollback::inputsEqual(actual.input, expected.input);
            if (!requestMatches) {
                return transportCheckFailed(__LINE__);
            }
            // A third socket cannot inject input into an established pair.
            UdpTransport intruder;
            if (!intruder.open() || !intruder.setPeer("127.0.0.1", host.localPort())
                || !intruder.sendInput(expected)) return transportCheckFailed(__LINE__);
            const auto rejectDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
            while (std::chrono::steady_clock::now() < rejectDeadline) {
                if (host.receiveInput(actual)) return transportCheckFailed(__LINE__);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            InputPacket response = expected;
            response.sequence++;
            response.player = 0;
            if (!host.sendInput(response)) {
                return transportCheckFailed(__LINE__);
            }
            const auto responseDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            while (std::chrono::steady_clock::now() < responseDeadline) {
                if (client.receiveInput(actual)) {
                    return actual.sequence == response.sequence && actual.player == response.player
                        && rollback::inputsEqual(actual.input, response.input);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return transportCheckFailed(__LINE__);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return transportCheckFailed(__LINE__);
}

}

extern "C" bool PartyBoard_NetTransportRunSelfTest(void)
{
    const bool passed = partyboard::netplay::runTransportSelfTest();
    OSReport("Netplay UDP loopback self-test: %s\n", passed ? "PASS" : "FAIL");
    return passed;
}
