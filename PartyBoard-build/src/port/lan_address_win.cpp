// The Windows PC's addresses on its home network: what local_ipv4() in
// src/port/ui/phone_controller.cpp does with getifaddrs() on the Mac.
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>

#include "port/lan_address_win.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace partyboard {

namespace {

std::string utf8(const wchar_t *text)
{
    if (text == nullptr || text[0] == L'\0') {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

} // namespace

std::vector<LanAddress> win_lan_addresses()
{
    std::vector<unsigned char> buffer;
    ULONG size = 16 * 1024;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    // The list can grow between the size query and the call; retry a little.
    for (int attempt = 0; attempt < 4 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
    }
    std::vector<LanAddress> found;
    if (result != NO_ERROR) {
        return found;
    }
    for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()); adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        // Wi-Fi and Ethernet only: Tailscale and other VPNs are virtual or
        // tunnel adapters, and phones on the Wi-Fi can't reach those.
        const bool wifi = adapter->IfType == IF_TYPE_IEEE80211;
        if (!wifi && adapter->IfType != IF_TYPE_ETHERNET_CSMACD) {
            continue;
        }
        for (auto *unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            const sockaddr *address = unicast->Address.lpSockaddr;
            if (address == nullptr || address->sa_family != AF_INET) {
                continue;
            }
            const auto *bytes =
                reinterpret_cast<const unsigned char *>(&reinterpret_cast<const sockaddr_in *>(address)->sin_addr);
            if (bytes[0] == 127 || bytes[0] == 0 || (bytes[0] == 169 && bytes[1] == 254) ||
                (bytes[0] == 100 && (bytes[1] & 0xC0) == 64)) {
                continue; // loopback, unset, link-local, Tailscale / carrier NAT
            }
            char text[16];
            std::snprintf(text, sizeof(text), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
            LanAddress entry;
            entry.ip = text;
            entry.adapter = utf8(adapter->FriendlyName);
            entry.wifi = wifi;
            // Hyper-V, WSL and VirtualBox host-only adapters look like
            // Ethernet but have no gateway.
            entry.gateway = adapter->FirstGatewayAddress != nullptr;
            found.push_back(entry);
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const LanAddress &a, const LanAddress &b) {
        const int rankA = (a.gateway ? 2 : 0) + (a.wifi ? 1 : 0);
        const int rankB = (b.gateway ? 2 : 0) + (b.wifi ? 1 : 0);
        return rankA > rankB;
    });
    return found;
}

} // namespace partyboard

#endif // _WIN32
