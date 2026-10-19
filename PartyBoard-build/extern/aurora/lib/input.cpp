#include "input.hpp"
#include "device.hpp"
#include "internal.hpp"

#include "magic_enum.hpp"

#include <SDL3/SDL_haptic.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>

#include <absl/container/flat_hash_map.h>
#include <algorithm>
#include <array>
#include <string>
#include <utility>

using namespace std::string_view_literals;

namespace aurora::input {
Module Log("aurora::input");
absl::flat_hash_map<Uint32, GameController> g_GameControllers;

namespace {
constexpr uint32_t kPortPreferencesMagic = SBIG('CPRT');
constexpr uint32_t kPortPreferencesVersion = 3;
// The version 3 layout holds ports 1-4. Ports 5-8 (PartyBoard's 8-player party)
// follow the rumble values in an extension block that older builds never read,
// so the file stays readable both ways.
constexpr uint32_t kPortPreferencesLegacyPorts = 4;
constexpr uint32_t kPortPreferencesExtraMagic = SBIG('CPR8');
constexpr uint32_t kMaxPersistedStringLength = 256;
constexpr uint16_t kDefaultDeviceRumbleIntensityLow = 0x8000;
constexpr uint16_t kDefaultDeviceRumbleIntensityHigh = 0x8000;

enum class PortPreferenceState : uint8_t {
  Unset = 0,
  None = 1,
  Controller = 2,
};

struct ControllerIdentity {
  std::string guid;
  std::string serial;
};

struct PortPreference {
  PortPreferenceState state = PortPreferenceState::Unset;
  ControllerIdentity identity;
};

struct DevicePreference {
  uint16_t rumbleIntensityLow = kDefaultDeviceRumbleIntensityLow;
  uint16_t rumbleIntensityHigh = kDefaultDeviceRumbleIntensityHigh;
};

std::array<PortPreference, PAD_MAX_CONTROLLERS> g_portPreferences;
DevicePreference g_devicePreference;
bool g_portPreferencesLoaded = false;

std::string port_preferences_path() {
  if (g_config.userPath == nullptr) {
    return {};
  }

  std::string path{g_config.userPath};
  if (!path.empty() && path.back() != '/' && path.back() != '\\') {
    path += '/';
  }
  path += "controller_ports.dat";
  return path;
}

std::string normalize_serial(const char* serial) {
  std::string normalized;
  if (serial == nullptr) {
    return normalized;
  }

  for (const char* c = serial; *c != '\0'; ++c) {
    if (*c == ':' || *c == '-') {
      continue;
    }
    normalized += *c >= 'A' && *c <= 'Z' ? static_cast<char>(*c - 'A' + 'a') : *c;
  }
  return normalized;
}

bool serial_matches(const std::string& saved, const std::string& current) {
  return !saved.empty() && !current.empty() && normalize_serial(saved.c_str()) == normalize_serial(current.c_str());
}

ControllerIdentity controller_identity(const GameController& controller) {
  ControllerIdentity identity;
  char guid[33] = {};
  SDL_GUIDToString(SDL_GetGamepadGUIDForID(SDL_GetGamepadID(controller.m_controller)), guid, sizeof(guid));
  identity.guid = guid;
  identity.serial = normalize_serial(SDL_GetGamepadSerial(controller.m_controller));
  return identity;
}

bool read_exact(SDL_IOStream* file, void* dst, size_t size) {
  auto* bytes = static_cast<uint8_t*>(dst);
  size_t total = 0;
  while (total < size) {
    const size_t read = SDL_ReadIO(file, bytes + total, size - total);
    if (read == 0) {
      return false;
    }
    total += read;
  }
  return true;
}

bool write_exact(SDL_IOStream* file, const void* src, size_t size) {
  auto* bytes = static_cast<const uint8_t*>(src);
  size_t total = 0;
  while (total < size) {
    const size_t written = SDL_WriteIO(file, bytes + total, size - total);
    if (written == 0) {
      return false;
    }
    total += written;
  }
  return true;
}

template <typename T>
bool read_value(SDL_IOStream* file, T& value) {
  return read_exact(file, &value, sizeof(value));
}

template <typename T>
bool write_value(SDL_IOStream* file, const T& value) {
  return write_exact(file, &value, sizeof(value));
}

bool read_string(SDL_IOStream* file, std::string& value) {
  uint32_t size = 0;
  if (!read_value(file, size) || size > kMaxPersistedStringLength) {
    return false;
  }

  value.resize(size);
  return size == 0 || read_exact(file, value.data(), size);
}

bool write_string(SDL_IOStream* file, const std::string& value) {
  const uint32_t size = static_cast<uint32_t>(std::min<size_t>(value.size(), kMaxPersistedStringLength));
  return write_value(file, size) && (size == 0 || write_exact(file, value.data(), size));
}

bool read_identity(SDL_IOStream* file, ControllerIdentity& identity) {
  return read_string(file, identity.guid) && read_string(file, identity.serial);
}

bool write_identity(SDL_IOStream* file, const ControllerIdentity& identity) {
  return write_string(file, identity.guid) && write_string(file, identity.serial);
}

bool read_port_preferences_file(std::array<PortPreference, PAD_MAX_CONTROLLERS>& preferences) {
  const auto path = port_preferences_path();
  if (path.empty()) {
    return true;
  }

  SDL_IOStream* file = SDL_IOFromFile(path.c_str(), "rb");
  if (file == nullptr) {
    return true;
  }

  uint32_t magic = 0;
  uint32_t version = 0;
  bool ok = read_value(file, magic) && read_value(file, version) && magic == kPortPreferencesMagic &&
            (version == 2 || version == kPortPreferencesVersion);

  auto read_preference = [&](PortPreference& preference) {
    uint8_t state = 0;
    ControllerIdentity identity;
    const bool read = read_value(file, state) && state <= static_cast<uint8_t>(PortPreferenceState::Controller) &&
                      read_identity(file, identity);
    if (read) {
      preference.state = static_cast<PortPreferenceState>(state);
      preference.identity = std::move(identity);
    }
    return read;
  };
  for (uint32_t port = 0; port < kPortPreferencesLegacyPorts; ++port) {
    ok = ok && read_preference(preferences[port]);
  }
  if (ok && version >= 3) {
    ok = read_value(file, g_devicePreference.rumbleIntensityLow) &&
         read_value(file, g_devicePreference.rumbleIntensityHigh);
  }
  // Optional block for ports 5-8; absent in files from 4-port builds.
  uint32_t extraMagic = 0;
  uint32_t extraCount = 0;
  if (ok && version >= 3 && read_value(file, extraMagic) && extraMagic == kPortPreferencesExtraMagic &&
      read_value(file, extraCount)) {
    // A damaged extension only loses ports 5-8, never the first four.
    extraCount = std::min<uint32_t>(extraCount, PAD_MAX_CONTROLLERS - kPortPreferencesLegacyPorts);
    for (uint32_t port = kPortPreferencesLegacyPorts; port < kPortPreferencesLegacyPorts + extraCount; ++port) {
      if (!read_preference(preferences[port])) {
        Log.warn("Controller port preference file '{}': ports 5-8 unreadable, ignoring them", path);
        for (uint32_t rest = kPortPreferencesLegacyPorts; rest < PAD_MAX_CONTROLLERS; ++rest) {
          preferences[rest] = {};
        }
        break;
      }
    }
  }

  SDL_CloseIO(file);
  if (!ok) {
    Log.warn("Ignoring invalid controller port preference file '{}'", path);
  }
  return ok;
}

void ensure_port_preferences_loaded() {
  if (g_portPreferencesLoaded) {
    return;
  }

  std::array<PortPreference, PAD_MAX_CONTROLLERS> preferences;
  if (read_port_preferences_file(preferences)) {
    g_portPreferences = std::move(preferences);
  }
  g_portPreferencesLoaded = true;
}

void save_port_preferences() {
  const auto path = port_preferences_path();
  if (path.empty()) {
    return;
  }

  if (!SDL_CreateDirectory(g_config.userPath)) {
    Log.warn("Failed to create controller port preference directory '{}': {}", g_config.userPath, SDL_GetError());
    return;
  }

  SDL_IOStream* file = SDL_IOFromFile(path.c_str(), "wb");
  if (file == nullptr) {
    Log.warn("Failed to open controller port preference file '{}': {}", path, SDL_GetError());
    return;
  }

  bool ok = write_value(file, kPortPreferencesMagic) && write_value(file, kPortPreferencesVersion);
  auto write_preference = [&](const PortPreference& preference) {
    const auto state = static_cast<uint8_t>(preference.state);
    ok = ok && write_value(file, state) && write_identity(file, preference.identity);
  };
  for (uint32_t port = 0; port < kPortPreferencesLegacyPorts; ++port) {
    write_preference(g_portPreferences[port]);
  }
  ok = ok && write_value(file, g_devicePreference.rumbleIntensityLow) &&
       write_value(file, g_devicePreference.rumbleIntensityHigh);
  constexpr uint32_t extraCount = PAD_MAX_CONTROLLERS - kPortPreferencesLegacyPorts;
  ok = ok && write_value(file, kPortPreferencesExtraMagic) && write_value(file, extraCount);
  for (uint32_t port = kPortPreferencesLegacyPorts; port < PAD_MAX_CONTROLLERS; ++port) {
    write_preference(g_portPreferences[port]);
  }

  if (!SDL_FlushIO(file)) {
    ok = false;
  }
  if (!SDL_CloseIO(file)) {
    ok = false;
  }
  if (!ok) {
    Log.warn("Failed to write controller port preference file '{}': {}", path, SDL_GetError());
  }
}

enum class IdentityMatch {
  None,
  Fallback,
  Exact,
};

IdentityMatch identity_match(const ControllerIdentity& saved, const ControllerIdentity& current) {
  if (saved.guid.empty()) {
    return IdentityMatch::None;
  }
  if (saved.guid == current.guid) {
    return saved.serial.empty() || serial_matches(saved.serial, current.serial) ? IdentityMatch::Exact
                                                                                : IdentityMatch::None;
  }
  if (!serial_matches(saved.serial, current.serial)) {
    return IdentityMatch::None;
  }

  // Attempt to match against VID/PID as a fallback if the GUID changes
  uint16_t savedVendor = 0;
  uint16_t savedProduct = 0;
  uint16_t currentVendor = 0;
  uint16_t currentProduct = 0;
  SDL_GetJoystickGUIDInfo(SDL_StringToGUID(saved.guid.c_str()), &savedVendor, &savedProduct, nullptr, nullptr);
  SDL_GetJoystickGUIDInfo(SDL_StringToGUID(current.guid.c_str()), &currentVendor, &currentProduct, nullptr, nullptr);
  return savedVendor != 0 && savedVendor == currentVendor && savedProduct != 0 && savedProduct == currentProduct
             ? IdentityMatch::Fallback
             : IdentityMatch::None;
}

bool is_instance_claimed(const std::array<Uint32, PAD_MAX_CONTROLLERS>& claimedControllers, size_t claimedCount,
                         Uint32 instance) {
  return std::find(claimedControllers.begin(), claimedControllers.begin() + claimedCount, instance) !=
         claimedControllers.begin() + claimedCount;
}

void apply_port_preferences() noexcept {
  ensure_port_preferences_loaded();
  if (!std::any_of(g_portPreferences.begin(), g_portPreferences.end(),
                   [](const auto& preference) { return preference.state != PortPreferenceState::Unset; })) {
    return;
  }

  for (auto& [instance, controller] : g_GameControllers) {
    const int32_t player = SDL_GetGamepadPlayerIndex(controller.m_controller);
    if (player >= 0 && player < PAD_MAX_CONTROLLERS && g_portPreferences[player].state != PortPreferenceState::Unset) {
      // Keep SDL's default player assignment from taking explicitly configured ports
      SDL_SetGamepadPlayerIndex(controller.m_controller, -1);
    }
  }

  std::array<Uint32, PAD_MAX_CONTROLLERS> claimedControllers{};
  size_t claimedCount = 0;
  for (uint32_t port = 0; port < g_portPreferences.size(); ++port) {
    const auto& preference = g_portPreferences[port];
    if (preference.state != PortPreferenceState::Controller) {
      continue;
    }

    Uint32 fallbackInstance = 0;
    GameController* fallbackController = nullptr;
    for (auto& [instance, controller] : g_GameControllers) {
      if (is_instance_claimed(claimedControllers, claimedCount, instance)) {
        continue;
      }

      switch (identity_match(preference.identity, controller_identity(controller))) {
      case IdentityMatch::Exact:
        SDL_SetGamepadPlayerIndex(controller.m_controller, static_cast<int32_t>(port));
        claimedControllers[claimedCount++] = instance;
        fallbackController = nullptr;
        break;
      case IdentityMatch::Fallback:
        // Prefer any later exact match before claiming a fallback candidate
        if (fallbackController == nullptr) {
          fallbackInstance = instance;
          fallbackController = &controller;
        }
        continue;
      case IdentityMatch::None:
        continue;
      }
      break;
    }

    if (fallbackController != nullptr) {
      SDL_SetGamepadPlayerIndex(fallbackController->m_controller, static_cast<int32_t>(port));
      claimedControllers[claimedCount++] = fallbackInstance;
    }
  }
}
} // namespace

GameController* get_controller_for_player(uint32_t player) noexcept {
  for (auto& [which, controller] : g_GameControllers) {
    if (player_index(which) == player) {
      return &controller;
    }
  }

#if 0
  /* If we don't have a controller assigned to this port use the first unassigned controller */
  if (!g_GameControllers.empty()) {
    int32_t availIndex = -1;
    GameController* ct = nullptr;
    for (auto& controller : g_GameControllers) {
      if (player_index(controller.first) == -1) {
        availIndex = controller.first;
        ct = &controller.second;
        break;
      }
    }
    if (availIndex != -1) {
      set_player_index(availIndex, player);
      return ct;
    }
  }
#endif
  return nullptr;
}

Sint32 get_instance_for_player(uint32_t player) noexcept {
  for (const auto& [which, controller] : g_GameControllers) {
    if (player_index(which) == player) {
      return which;
    }
  }

  return {};
}

// ---- Diagnostics for the session log: how each controller is connected,
// its battery, and which player it drives. Bluetooth dropouts and flat
// batteries are the usual reasons a wireless controller misbehaves.
namespace {
const char* connection_name(SDL_Gamepad* ctrl) noexcept {
  switch (SDL_GetJoystickConnectionState(SDL_GetGamepadJoystick(ctrl))) {
  case SDL_JOYSTICK_CONNECTION_WIRED:
    return "USB";
  case SDL_JOYSTICK_CONNECTION_WIRELESS:
    return "wireless";
  default:
    return "unknown connection";
  }
}

std::string battery_text(SDL_PowerState state, int percent) {
  const std::string pct = percent >= 0 ? " " + std::to_string(percent) + "%" : "";
  switch (state) {
  case SDL_POWERSTATE_ON_BATTERY:
    return "battery" + (pct.empty() ? std::string(" level unknown") : pct);
  case SDL_POWERSTATE_CHARGING:
    return "charging" + pct;
  case SDL_POWERSTATE_CHARGED:
    return "fully charged";
  case SDL_POWERSTATE_NO_BATTERY:
    return "no battery";
  default:
    return "battery unknown";
  }
}

const char* gamepad_name(SDL_Gamepad* ctrl) noexcept {
  const char* name = SDL_GetGamepadName(ctrl);
  return name != nullptr ? name : "unknown";
}

// Logs which controller drives each player, whenever that changes.
void log_player_slots() {
  static std::string last;
  std::string text;
  for (int32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
    std::string who = "none";
    for (auto& [instance, controller] : g_GameControllers) {
      if (SDL_GetGamepadPlayerIndex(controller.m_controller) == port) {
        who = std::string("'") + gamepad_name(controller.m_controller) + "' (" +
              connection_name(controller.m_controller) + ")";
        break;
      }
    }
    text += (port == 0 ? "" : ", ") + std::string("P") + std::to_string(port + 1) + " " + who;
  }
  if (text != last) {
    Log.info("Controller players: {}", text);
    last = text;
  }
}
} // namespace

void log_battery(Uint32 instance, SDL_PowerState state, int percent) noexcept {
  auto it = g_GameControllers.find(instance);
  if (it == g_GameControllers.end()) {
    return;
  }
  // Report state changes, every 10% step, and a warning when it runs low;
  // controllers send small changes often.
  struct Seen {
    SDL_PowerState state;
    int bucket;
  };
  static absl::flat_hash_map<Uint32, Seen> seen;
  const int bucket = percent >= 0 ? percent / 10 : -1;
  auto [entry, inserted] = seen.try_emplace(instance, Seen{state, bucket});
  if (!inserted && entry->second.state == state && entry->second.bucket == bucket) {
    return;
  }
  entry->second = {state, bucket};
  const std::string text = battery_text(state, percent);
  if (state == SDL_POWERSTATE_ON_BATTERY && percent >= 0 && percent <= 15) {
    Log.warn("Controller '{}' battery low: {}", gamepad_name(it->second.m_controller), text);
  } else {
    Log.info("Controller '{}': {}", gamepad_name(it->second.m_controller), text);
  }
}

SDL_JoystickID add_controller(SDL_JoystickID which) noexcept {
  auto* ctrl = SDL_OpenGamepad(which);
  if (ctrl != nullptr) {
    GameController controller;
    controller.m_controller = ctrl;
    controller.m_index = which;
    controller.m_vid = SDL_GetGamepadVendor(ctrl);
    controller.m_pid = SDL_GetGamepadProduct(ctrl);
    if (controller.m_vid == 0x05ac /* USB_VENDOR_APPLE */ && controller.m_pid == 3) {
      // Ignore Apple TV remote
      SDL_CloseGamepad(ctrl);
      return -1;
    }
    controller.m_isGameCube = controller.m_vid == 0x057E && controller.m_pid == 0x0337;
    if (controller.m_isGameCube ||
        (SDL_GetGamepadType(ctrl) == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && controller.m_pid == 0x2073)) {
      controller.m_deadZones.emulateTriggers = false;
    }
    const auto props = SDL_GetGamepadProperties(ctrl);
    controller.m_hasRumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, true);
    controller.m_hasRgbLed = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
    SDL_JoystickID instance = SDL_GetJoystickID(SDL_GetGamepadJoystick(ctrl));
    int percent = -1;
    const SDL_PowerState power = SDL_GetGamepadPowerInfo(ctrl, &percent);
    Log.info("Added controller '{}' (instance {}, vid {:04x}, pid {:04x}, type {}, {}, {})", gamepad_name(ctrl),
             instance, controller.m_vid, controller.m_pid, static_cast<int>(SDL_GetGamepadType(ctrl)),
             connection_name(ctrl), battery_text(power, percent));
    g_GameControllers[instance] = controller;
    apply_port_preferences();
    log_player_slots();
    return instance;
  }

  return -1;
}

void remove_controller(Uint32 instance) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    Log.info("Removed controller '{}' (instance {})",
             SDL_GetGamepadName(it->second.m_controller) != nullptr ? SDL_GetGamepadName(it->second.m_controller)
                                                                    : "unknown",
             instance);
    SDL_CloseGamepad(it->second.m_controller);
    g_GameControllers.erase(it);
    apply_port_preferences();
    log_player_slots();
  }
}

bool is_gamecube(Uint32 instance) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    return it->second.m_isGameCube;
  }
  return false;
}

int32_t player_index(Uint32 instance) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    return SDL_GetGamepadPlayerIndex(it->second.m_controller);
  }
  return -1;
}

void set_player_index(Uint32 instance, Sint32 index) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    SDL_SetGamepadPlayerIndex(it->second.m_controller, index);
    log_player_slots();
  }
}

std::string controller_name(Uint32 instance) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    const auto* name = SDL_GetGamepadName(it->second.m_controller);
    if (name != nullptr) {
      return {name};
    }
  }
  return {};
}

bool controller_has_rumble(Uint32 instance) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    return it->second.m_hasRumble;
  }
  return false;
}

void controller_rumble(uint32_t instance, uint16_t low_freq_intensity, uint16_t high_freq_intensity,
                       uint16_t duration_ms) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    SDL_RumbleGamepad(it->second.m_controller, low_freq_intensity, high_freq_intensity, duration_ms);
  }
}

void get_device_rumble_intensity(uint16_t* low_freq_intensity, uint16_t* high_freq_intensity) noexcept {
  ensure_port_preferences_loaded();
  *low_freq_intensity = g_devicePreference.rumbleIntensityLow;
  *high_freq_intensity = g_devicePreference.rumbleIntensityHigh;
}

void set_device_rumble_intensity(const uint16_t low_freq_intensity, const uint16_t high_freq_intensity) noexcept {
  ensure_port_preferences_loaded();
  g_devicePreference.rumbleIntensityLow = low_freq_intensity;
  g_devicePreference.rumbleIntensityHigh = high_freq_intensity;
  save_port_preferences();
}

uint32_t controller_count() noexcept { return g_GameControllers.size(); }

void persist_controller_for_player(uint32_t player, const GameController* controller) noexcept {
  if (player >= PAD_MAX_CONTROLLERS) {
    return;
  }

  ensure_port_preferences_loaded();
  if (controller != nullptr) {
    g_portPreferences[player].state = PortPreferenceState::Controller;
    g_portPreferences[player].identity = controller_identity(*controller);
  } else {
    g_portPreferences[player].state = PortPreferenceState::None;
    g_portPreferences[player].identity = {};
  }
  save_port_preferences();
}

void initialize() noexcept {
  /* Make sure we initialize everything input related now, this will automatically add all of the connected controllers
   * as expected */
  ASSERT(SDL_Init(SDL_INIT_HAPTIC | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_SENSOR),
         "Failed to initialize SDL subsystems: {}", SDL_GetError());
}

struct MouseScrollStatus {
  float scrollX;
  float scrollY;
};

static MouseScrollStatus g_MouseStatus;

void set_mouse_scroll(const float scrollX, const float scrollY) noexcept {
  g_MouseStatus.scrollX = scrollX;
  g_MouseStatus.scrollY = scrollY;
}

void get_mouse_scroll(float* scrollX, float* scrollY) noexcept {
  *scrollX = g_MouseStatus.scrollX;
  *scrollY = g_MouseStatus.scrollY;
}

void shutdown() noexcept {
  // Upon shutdown we want to ensure all controllers are in a default state, so force all rumble supporting controllers
  // to shut off their rumble motors.
  for (const auto& controller : g_GameControllers) {
    if (!controller.second.m_hasRumble) {
      continue;
    }
    controller_rumble(controller.first, 0, 0, 0);
  }
  device::shutdown();
}
} // namespace aurora::input
