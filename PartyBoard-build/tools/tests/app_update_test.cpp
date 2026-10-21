// Checks the update manifest parser (src/port/app_update_manifest.cpp) against manifests shaped
// like the ones platforms/android/scripts/publish-update.sh writes.

#include "port/app_update.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace partyboard::update;

namespace {

int failures = 0;

void expect(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

const std::string kHash = "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789ABCDEF";
const std::string kUrl =
    "https://github.com/zeranemesis/Marioparty4/releases/download/partyboard-android-latest/PartyBoard-android-arm64-v8a-128.apk";

std::string manifest_with(const std::string &url, const std::string &hash, const std::string &code = "128")
{
    return R"j({"versionCode": )j" + code + R"j(, "version": "0.1.0 (build 128)", "abi": "arm64-v8a",
        "commit": "d8bea614d2079d91582f755fcc87fb831d9eb535", "downloadUrl": ")j" + url + R"j(",
        "sha256": ")j" + hash + R"j(", "notes": "- Android: updates from GitHub\n- PAL: accents"})j";
}

} // namespace

int main()
{
    const auto parsed = parse_manifest(manifest_with(kUrl, kHash));
    expect(parsed.has_value(), "a published manifest parses");
    if (parsed) {
        expect(parsed->versionCode == 128, "versionCode");
        expect(parsed->version == "0.1.0 (build 128)", "version");
        expect(parsed->abi == "arm64-v8a", "abi");
        expect(parsed->downloadUrl == kUrl, "downloadUrl");
        expect(parsed->sha256 == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "sha256 is lowercased");
        expect(parsed->notes == "- Android: updates from GitHub\n- PAL: accents", "notes keep their lines");

        expect(offers_update(*parsed, 127, "arm64-v8a"), "a newer build is offered");
        expect(!offers_update(*parsed, 128, "arm64-v8a"), "the installed build is not offered again");
        expect(!offers_update(*parsed, 200, "arm64-v8a"), "an older build is never offered");
        expect(!offers_update(*parsed, 1, "x86_64"), "another ABI's APK is not offered");
        expect(!offers_update(*parsed, 1, ""), "an unknown ABI gets nothing");
    }

    expect(!parse_manifest("").has_value(), "empty text");
    expect(!parse_manifest("[1, 2]").has_value(), "not an object");
    expect(!parse_manifest("{\"versionCode\": 3").has_value(), "truncated JSON");
    expect(!parse_manifest(manifest_with(kUrl, kHash, "0")).has_value(), "versionCode 0");
    expect(!parse_manifest(manifest_with(kUrl, kHash, "\"128\"")).has_value(), "versionCode as a string");
    expect(!parse_manifest(manifest_with(kUrl, kHash.substr(1))).has_value(), "short hash");
    expect(!parse_manifest(manifest_with(kUrl, "g" + kHash.substr(1))).has_value(), "hash with a non-hex digit");
    expect(!parse_manifest(manifest_with("https://example.com/PartyBoard.apk", kHash)).has_value(), "download off GitHub");
    expect(!parse_manifest(manifest_with("http://github.com/zeranemesis/Marioparty4/releases/download/x/a.apk", kHash))
                .has_value(),
        "plain HTTP download");
    expect(!parse_manifest(manifest_with("https://github.com/someone/else/releases/download/x/a.apk", kHash)).has_value(),
        "another repository");
    expect(!parse_manifest(manifest_with("https://github.com/zeranemesis/Marioparty4/releases/download/../../x", kHash))
                .has_value(),
        "parent directory");
    expect(!parse_manifest(manifest_with("https://github.com/zeranemesis/Marioparty4/releases/download/x/a.apk?y=1", kHash))
                .has_value(),
        "query string");

    const auto minimal = parse_manifest(R"({"versionCode": 5, "version": "5", "abi": "x86_64", "downloadUrl": ")" + kUrl
        + R"(", "sha256": ")" + kHash + R"("})");
    expect(minimal.has_value() && minimal->notes.empty() && minimal->commit.empty(), "commit and notes are optional");

    // Mac and Windows releases: latest.json in PaulGitHitGub/PartyBoard-updates.
    const auto release = parse_release_manifest(R"({"version":"1.1","name":"PartyBoard Beta 1.1","released":"2026-10-19",
        "page":"https://github.com/PaulGitHitGub/PartyBoard/releases/latest","notes":"one short line"})");
    expect(release && release->version == "1.1" && release->name == "PartyBoard Beta 1.1"
            && release->page == "https://github.com/PaulGitHitGub/PartyBoard/releases/latest" && release->notes == "one short line",
        "a release manifest parses");
    const auto bare = parse_release_manifest(R"({"version":"2","page":"https://github.com/PaulGitHitGub/PartyBoard/releases/latest"})");
    expect(bare && bare->name == "PartyBoard 2" && bare->notes.empty(), "name and notes are optional");
    expect(!parse_release_manifest(R"({"version":"1.1","page":"https://example.com/PartyBoard/releases/latest"})"),
        "a page outside the release repository");
    expect(!parse_release_manifest(R"({"version":"1.1","page":"https://github.com/PaulGitHitGub/PartyBoard-evil/x"})"),
        "a page in a look-alike repository");
    expect(!parse_release_manifest(R"({"version":"1.1 beta","page":"https://github.com/PaulGitHitGub/PartyBoard/"})"),
        "a version with text");
    expect(!parse_release_manifest(R"({"version":1.1,"page":"https://github.com/PaulGitHitGub/PartyBoard/"})"),
        "a version that is a number");
    expect(!parse_release_manifest("<html>404</html>"), "not JSON");
    expect(!parse_release_manifest(""), "empty");

    expect(compare_versions("1.1", "1.0") == 1, "1.1 > 1.0");
    expect(compare_versions("1.1", "1.1.0") == 0, "1.1 == 1.1.0");
    expect(compare_versions("1.9", "1.10") == -1, "1.9 < 1.10");
    expect(compare_versions("2", "1.99") == 1, "2 > 1.99");
    expect(!compare_versions("1..1", "1.0").has_value(), "empty part");
    expect(!compare_versions("", "1.0").has_value(), "empty version");
    expect(!compare_versions("1.0", "v1.0").has_value(), "leading v");

    if (failures == 0) {
        std::puts("app_update_test: all checks passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
