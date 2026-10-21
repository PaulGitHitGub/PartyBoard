#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Updates of the Android app straight from GitHub, like PartyBoardOnline.exe's UpdateService on
// Windows. Every build CI publishes (.github/workflows/build.yml, platforms/android/scripts/
// publish-update.sh) lands in the release "partyboard-android-latest" with a manifest,
// android-update.json, that names the build, its APK, the APK's SHA-256 and the changes since the
// previous published build. The app reads that manifest, and when it names a newer build it opens
// the APK's download in the browser; Android installs it over the app, keeping the saves, and
// only accepts it when it is signed with the installed app's key. The app asks for no install
// permission of its own: Play Protect blocks unknown apps that do.
//
// Mac and Windows (PartyBoard releases): a build made with PARTYBOARD_RELEASE_VERSION (e.g. "1.1")
// reads a small public file, latest.json in PaulGitHitGub/PartyBoard-updates, that names the newest
// release and its download page. When it is newer, the home screen says so and its button opens the
// page in the browser (the release repository is private: the player signs in there). Nothing is
// downloaded or installed by the game. Builds without a release version never check.
namespace partyboard::update {

inline constexpr std::string_view kReleaseManifestUrl =
    "https://raw.githubusercontent.com/PaulGitHitGub/PartyBoard-updates/main/latest.json";
// A release manifest can only send the player to the release repository.
inline constexpr std::string_view kReleasePagePrefix = "https://github.com/PaulGitHitGub/PartyBoard/";

struct ReleaseManifest {
    // Dotted numbers, e.g. "1.1": the only thing compared.
    std::string version;
    // What the home screen calls it, e.g. "PartyBoard Beta 1.1".
    std::string name;
    std::string released;
    std::string page;
    std::string notes;
};

// Nothing for bad JSON, a missing or malformed version, or a page outside kReleasePagePrefix.
// A missing name becomes "PartyBoard <version>"; notes and released are optional.
std::optional<ReleaseManifest> parse_release_manifest(std::string_view json);

// Compares dotted version numbers ("1.10" > "1.9", "1.1" == "1.1.0"): <0, 0 or >0.
// Nothing when either is not digits separated by single dots.
std::optional<int> compare_versions(std::string_view a, std::string_view b);

inline constexpr std::string_view kManifestUrl =
    "https://github.com/zeranemesis/Marioparty4/releases/download/partyboard-android-latest/android-update.json";
// A manifest can only send the app to this repository's own releases.
inline constexpr std::string_view kDownloadPrefix = "https://github.com/zeranemesis/Marioparty4/releases/download/";

struct Manifest {
    // Android's versionCode, the CI run number: the only thing compared.
    std::int64_t versionCode = 0;
    std::string version;
    // The Android ABI the APK is built for, e.g. "arm64-v8a".
    std::string abi;
    std::string commit;
    std::string downloadUrl;
    // 64 lowercase hex digits.
    std::string sha256;
    // One change per line.
    std::string notes;
};

// Nothing for a manifest with a missing or malformed field, or a download outside kDownloadPrefix.
std::optional<Manifest> parse_manifest(std::string_view json);

// Whether the manifest names a newer build of the app built for `abi`.
bool offers_update(const Manifest &manifest, std::int64_t installedVersionCode, std::string_view abi);

// --- Runtime. Android installs updates; Mac and Windows release builds only point to the download
// page. Elsewhere, and in builds without a release version, supported() is false and nothing runs. ---

enum class State : std::uint8_t {
    Idle,
    Checking,
    UpToDate,
    Available,
    // The download (Android) or the release page (Mac, Windows) was handed to the browser.
    Downloading,
    Failed,
};

struct Status {
    State state = State::Idle;
    // Valid from Available on.
    Manifest manifest;
    std::int64_t installedVersionCode = 0;
    // Mac and Windows: the newest release named by the public manifest, and this build's version.
    ReleaseManifest release;
    std::string installedVersion;
    // When Failed: a fixed English sentence, translated by the UI, and what went wrong, which is not.
    std::string error;
    std::string detail;
    // The last check was the automatic one: the UI keeps quiet unless it found an update.
    bool quiet = false;
};

bool supported() noexcept;
// Fetches the manifest on a worker thread, unless a check or an install is already running. A quiet
// check that fails (no network, most often) goes back to Idle: the game stays playable offline.
// Never during an online game: the check is skipped and logged.
void check(bool quiet);
// Opens the download (Android) or the release page (Mac, Windows) of the build the last check found
// in the browser. Call it from the UI thread.
void download();
Status status();
// This build's release version ("" when it has none).
std::string_view release_version() noexcept;
// What players call this build, e.g. "PartyBoard Beta 1.1" ("" without a release version).
std::string release_name();

} // namespace partyboard::update
