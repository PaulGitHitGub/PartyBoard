#ifndef PARTYBOARD_PORT_LAN_ADDRESS_WIN_HPP
#define PARTYBOARD_PORT_LAN_ADDRESS_WIN_HPP

// The Windows PC's IPv4 addresses on its home network, for the phone
// controller's link (src/port/lan_address_win.cpp). Built into both
// partyboard.exe (the session supervisor writes the link file) and the game
// (Settings > Input > Phone Controller shows the link and QR code).
#if defined(_WIN32)

#include <string>
#include <vector>

namespace partyboard {

struct LanAddress {
    std::string ip;      // "192.168.1.20"
    std::string adapter; // the adapter's name in Windows, e.g. "Wi-Fi"
    bool wifi = false;
    bool gateway = false; // has a default gateway: a real home network
};

// Best first: Wi-Fi with a gateway, then Ethernet with a gateway, then the
// rest. Only adapters that are up; never loopback, link-local (169.254.x),
// VPN/tunnel adapters or Tailscale (100.64.0.0/10).
std::vector<LanAddress> win_lan_addresses();

} // namespace partyboard

#endif // _WIN32
#endif
