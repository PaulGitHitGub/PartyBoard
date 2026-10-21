// Android app updates from GitHub (port/app_update.hpp). The manifest is fetched with
// port/http.hpp; the APK itself is downloaded by the browser, the way the first install was.
// Mac and Windows release builds read the public latest.json the same way and only open the
// release page in the browser; they never download anything themselves.

#include "port/app_update.hpp"

#include "port/http.hpp"

#include <aurora/lib/logging.hpp>
#include <fmt/format.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#ifdef __ANDROID__
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_system.h>
#include <jni.h>
#else
#include "port/netplay_runtime.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_misc.h>
#endif

// This build's release version, set by CMake (-DPARTYBOARD_RELEASE_VERSION=1.1) for the builds
// given to players. Builds without it never check.
#ifndef PARTYBOARD_RELEASE_VERSION
#define PARTYBOARD_RELEASE_VERSION ""
#endif

namespace partyboard::update {
namespace {

    std::mutex gMutex;
    Status gStatus;

#ifdef __ANDROID__
    aurora::Module UpdateLog("partyboard::update");

    // A check owns a worker thread.
    bool gWorking = false;

#if defined(__aarch64__)
    constexpr std::string_view kAbi = "arm64-v8a";
#elif defined(__x86_64__)
    constexpr std::string_view kAbi = "x86_64";
#else
    constexpr std::string_view kAbi = "";
#endif

    // PartyBoardActivity.getInstalledVersionCode(). The class comes from the activity object
    // rather than FindClass, which cannot see the app's classes from a thread the VM did not start.
    std::int64_t installed_version_code()
    {
        auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
        if (env == nullptr || env->PushLocalFrame(8) != 0) {
            if (env != nullptr) {
                env->ExceptionClear();
            }
            return 0;
        }
        std::int64_t code = 0;
        if (auto *activity = static_cast<jobject>(SDL_GetAndroidActivity())) {
            jclass activityClass = env->GetObjectClass(activity);
            const jmethodID method =
                activityClass != nullptr ? env->GetMethodID(activityClass, "getInstalledVersionCode", "()J") : nullptr;
            if (method != nullptr && !env->ExceptionCheck()) {
                code = static_cast<std::int64_t>(env->CallLongMethod(activity, method));
            }
        }
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            code = 0;
        }
        env->PopLocalFrame(nullptr);
        return code;
    }

    void fail(std::string error, std::string detail = {})
    {
        UpdateLog.warn("{} {}", error, detail);
        std::lock_guard lock(gMutex);
        // An automatic check says nothing when it fails: most often there is no network.
        const bool silent = gStatus.quiet && gStatus.state == State::Checking;
        gStatus.state = silent ? State::Idle : State::Failed;
        gStatus.error = std::move(error);
        gStatus.detail = std::move(detail);
        gWorking = false;
    }

    void run_check()
    {
        const auto response = http::get(std::string(kManifestUrl), "PartyBoard-Updater/1");
        if (response.status != 200) {
            if (response.status == 0) {
                fail("Could not reach GitHub.", response.error);
            }
            else {
                fail("GitHub did not send the update manifest.", fmt::format("HTTP {}", response.status));
            }
            return;
        }
        auto manifest = parse_manifest(response.body);
        if (!manifest) {
            fail("The update manifest on GitHub is invalid.");
            return;
        }
        const std::int64_t installed = installed_version_code();
        const bool newer = offers_update(*manifest, installed, kAbi);
        UpdateLog.info("Installed build {}, GitHub has build {} for {}{}", installed, manifest->versionCode,
            manifest->abi, newer ? ": update available" : "");

        std::lock_guard lock(gMutex);
        gStatus.state = newer ? State::Available : State::UpToDate;
        gStatus.manifest = std::move(*manifest);
        gStatus.installedVersionCode = installed;
        gStatus.error.clear();
        gStatus.detail.clear();
        gWorking = false;
    }
#else
    // A check owns a worker thread.
    bool gWorking = false;

    constexpr std::string_view kReleaseVersion = PARTYBOARD_RELEASE_VERSION;

    // Always-on game.log lines (stdout, like the lobby's).
    template <typename... Args>
    void log_line(fmt::format_string<Args...> format, Args &&...args)
    {
        const auto line = fmt::format(format, std::forward<Args>(args)...);
        std::fprintf(stdout, "[update] %s\n", line.c_str());
        std::fflush(stdout);
    }

    // The manifest to read. Test builds (HD beta) take PARTYBOARD_UPDATE_MANIFEST_URL, which may
    // also be a file:// path, to fake a newer release.
    std::string manifest_url()
    {
#if defined(PARTYBOARD_HD_BETA)
        if (const char *url = std::getenv("PARTYBOARD_UPDATE_MANIFEST_URL"); url != nullptr && url[0] != '\0') {
            return url;
        }
#endif
        return std::string(kReleaseManifestUrl);
    }

    http::Response fetch(const std::string &url)
    {
        constexpr std::string_view kFile = "file://";
        if (!url.starts_with(kFile)) {
            return http::get(url, "PartyBoard-Updater/1");
        }
        http::Response response;
        std::ifstream file(url.substr(kFile.size()), std::ios::binary);
        if (!file) {
            response.status = 404;
            return response;
        }
        response.body.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        response.status = 200;
        return response;
    }

    void finish(State state, std::string error = {}, std::string detail = {})
    {
        std::lock_guard lock(gMutex);
        // An automatic check says nothing when it fails: most often there is no network.
        if (state == State::Failed && gStatus.quiet) {
            state = State::Idle;
        }
        gStatus.state = state;
        gStatus.error = std::move(error);
        gStatus.detail = std::move(detail);
        gWorking = false;
    }

    void run_check()
    {
        const std::string url = manifest_url();
        const std::string mine(kReleaseVersion);
        const auto response = fetch(url);
        if (response.status != 200) {
            const std::string why = response.status == 0
                ? fmt::format("error no response ({})", response.error.empty() ? "no network" : response.error)
                : fmt::format("error HTTP {}", response.status);
            log_line("checked {}: latest ?, this build {}, {}", url, mine, why);
            finish(State::Failed, response.status == 0 ? "Could not check for updates." : "Could not find the update information.",
                response.status == 0 ? std::string() : fmt::format("HTTP {}", response.status));
            return;
        }
        auto manifest = parse_release_manifest(response.body);
        if (!manifest) {
            log_line("checked {}: latest ?, this build {}, error bad manifest ({} bytes)", url, mine, response.body.size());
            finish(State::Failed, "The update information is invalid.");
            return;
        }
        const auto order = compare_versions(manifest->version, mine);
        const char *verdict = !order ? "error this build's version is not comparable"
            : *order > 0             ? "newer"
            : *order == 0            ? "same"
                                     : "older";
        log_line("checked {}: latest {}, this build {}, {}{}", url, manifest->version, mine, verdict,
            order && *order > 0
                ? fmt::format(" (\"{}\", released {}, page {}, notes \"{}\")", manifest->name, manifest->released,
                      manifest->page, manifest->notes)
                : std::string());
        {
            std::lock_guard lock(gMutex);
            gStatus.release = std::move(*manifest);
            gStatus.installedVersion = mine;
        }
        finish(order && *order > 0 ? State::Available : State::UpToDate);
    }
#endif

} // namespace

bool supported() noexcept
{
#ifdef __ANDROID__
    return !std::string_view(kAbi).empty();
#else
    return !kReleaseVersion.empty();
#endif
}

std::string_view release_version() noexcept
{
    return PARTYBOARD_RELEASE_VERSION;
}

std::string release_name()
{
    const std::string_view version = release_version();
    return version.empty() ? std::string() : "PartyBoard Beta " + std::string(version);
}

void check([[maybe_unused]] bool quiet)
{
#ifdef __ANDROID__
    if (!supported()) {
        return;
    }
    {
        std::lock_guard lock(gMutex);
        if (gWorking) {
            return;
        }
        gWorking = true;
        gStatus.state = State::Checking;
        gStatus.quiet = quiet;
        gStatus.error.clear();
        gStatus.detail.clear();
    }
    std::thread(run_check).detach();
#else
    if (!supported()) {
        return;
    }
    // Never during an online game: the check touches no game state, but it is skipped there so
    // nothing extra happens while computers play together.
    if (PartyBoard_NetplayEnabled()) {
        log_line("skipped: online game");
        return;
    }
    {
        std::lock_guard lock(gMutex);
        if (gWorking) {
            return;
        }
        gWorking = true;
        gStatus.state = State::Checking;
        gStatus.quiet = quiet;
        gStatus.error.clear();
        gStatus.detail.clear();
    }
    log_line("checking for a newer release ({}, this build {})", quiet ? "at startup" : "Check Now", kReleaseVersion);
    std::thread(run_check).detach();
#endif
}

void download()
{
#ifdef __ANDROID__
    std::string url;
    {
        std::lock_guard lock(gMutex);
        if (gWorking || gStatus.manifest.versionCode <= gStatus.installedVersionCode) {
            return;
        }
        url = gStatus.manifest.downloadUrl;
    }
    // The browser downloads the APK and Android installs it over this one, keeping the saves:
    // no install permission in the app, nothing for Play Protect to hold against it.
    if (!SDL_OpenURL(url.c_str())) {
        fail("Could not open the download.", SDL_GetError());
        return;
    }
    UpdateLog.info("Opened {} in the browser", url);
    std::lock_guard lock(gMutex);
    gStatus.state = State::Downloading;
    gStatus.error.clear();
    gStatus.detail.clear();
#else
    std::string url;
    {
        std::lock_guard lock(gMutex);
        if (gWorking || gStatus.release.page.empty()) {
            return;
        }
        url = gStatus.release.page;
    }
    if (!SDL_OpenURL(url.c_str())) {
        std::string why = SDL_GetError();
        log_line("could not open {} in the browser: {}", url, why);
        std::lock_guard lock(gMutex);
        gStatus.state = State::Failed;
        gStatus.error = "Could not open the download page.";
        gStatus.detail = std::move(why);
        return;
    }
    log_line("opened {} in the browser", url);
    std::lock_guard lock(gMutex);
    gStatus.state = State::Downloading;
    gStatus.error.clear();
    gStatus.detail.clear();
#endif
}

Status status()
{
    std::lock_guard lock(gMutex);
    return gStatus;
}

} // namespace partyboard::update
