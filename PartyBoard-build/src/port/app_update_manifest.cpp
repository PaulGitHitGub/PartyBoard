// The update manifests (android-update.json, and latest.json for Mac and Windows releases): parsing
// and the newer-build tests. No platform code,
// so tools/tests/app_update_test.cpp builds it on its own.

#include "port/app_update.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <vector>

namespace partyboard::update {
namespace {

    using json = nlohmann::json;

    // Bounds a manifest cannot need; they keep a broken one from filling the menu.
    constexpr std::size_t kMaxFieldLength = 512;
    constexpr std::size_t kMaxNotesLength = 4000;

    std::optional<std::string> text_field(const json &document, const char *key, std::size_t maxLength)
    {
        const auto found = document.find(key);
        if (found == document.end() || !found->is_string()) {
            return std::nullopt;
        }
        auto value = found->get<std::string>();
        if (value.size() > maxLength) {
            return std::nullopt;
        }
        return value;
    }

    bool is_sha256(std::string &hash)
    {
        if (hash.size() != 64) {
            return false;
        }
        for (char &c : hash) {
            if (c >= 'A' && c <= 'F') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    // A file name right under a release tag: no query, fragment, parent directory or control character.
    bool is_release_download(std::string_view url)
    {
        if (!url.starts_with(kDownloadPrefix) || url.size() == kDownloadPrefix.size()) {
            return false;
        }
        const auto rest = url.substr(kDownloadPrefix.size());
        if (rest.find("..") != std::string_view::npos) {
            return false;
        }
        for (const unsigned char c : rest) {
            if (c <= 0x20 || c == 0x7f || c == '?' || c == '#' || c == '\\') {
                return false;
            }
        }
        return true;
    }

    // The release page: an https link under the release repository, no control characters, spaces,
    // backslashes or parent directories.
    bool is_release_page(std::string_view url)
    {
        if (!url.starts_with(kReleasePagePrefix) || url.find("..") != std::string_view::npos) {
            return false;
        }
        for (const unsigned char c : url) {
            if (c <= 0x20 || c == 0x7f || c == '\\' || c == '"' || c == '<' || c == '>') {
                return false;
            }
        }
        return true;
    }

    // Splits "1.10.2" into numbers; nothing unless it is digits separated by single dots.
    std::optional<std::vector<std::uint32_t>> version_parts(std::string_view text)
    {
        std::vector<std::uint32_t> parts;
        std::uint64_t value = 0;
        std::size_t digits = 0;
        for (std::size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '.') {
                if (digits == 0 || parts.size() >= 8) {
                    return std::nullopt;
                }
                parts.push_back(static_cast<std::uint32_t>(value));
                value = 0;
                digits = 0;
            }
            else if (text[i] >= '0' && text[i] <= '9') {
                value = value * 10 + static_cast<std::uint64_t>(text[i] - '0');
                if (++digits > 9) {
                    return std::nullopt;
                }
            }
            else {
                return std::nullopt;
            }
        }
        return parts;
    }

} // namespace

std::optional<int> compare_versions(std::string_view a, std::string_view b)
{
    const auto left = version_parts(a);
    const auto right = version_parts(b);
    if (!left || !right) {
        return std::nullopt;
    }
    const std::size_t count = std::max(left->size(), right->size());
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t l = i < left->size() ? (*left)[i] : 0;
        const std::uint32_t r = i < right->size() ? (*right)[i] : 0;
        if (l != r) {
            return l < r ? -1 : 1;
        }
    }
    return 0;
}

std::optional<ReleaseManifest> parse_release_manifest(std::string_view text)
{
    const json document = json::parse(text.begin(), text.end(), nullptr, false);
    if (!document.is_object()) {
        return std::nullopt;
    }
    auto version = text_field(document, "version", 32);
    auto page = text_field(document, "page", kMaxFieldLength);
    if (!version || !version_parts(*version) || !page || !is_release_page(*page)) {
        return std::nullopt;
    }
    ReleaseManifest manifest;
    manifest.version = std::move(*version);
    manifest.page = std::move(*page);
    // Shown on the home screen: one short line, no line breaks.
    auto one_line = [](std::string value) {
        for (char &c : value) {
            if (static_cast<unsigned char>(c) < 0x20) {
                c = ' ';
            }
        }
        return value;
    };
    manifest.name = one_line(text_field(document, "name", 64).value_or(""));
    if (manifest.name.empty()) {
        manifest.name = "PartyBoard " + manifest.version;
    }
    manifest.released = one_line(text_field(document, "released", 32).value_or(""));
    manifest.notes = one_line(text_field(document, "notes", 200).value_or(""));
    return manifest;
}

std::optional<Manifest> parse_manifest(std::string_view text)
{
    const json document = json::parse(text.begin(), text.end(), nullptr, false);
    if (!document.is_object()) {
        return std::nullopt;
    }

    Manifest manifest;
    const auto code = document.find("versionCode");
    if (code == document.end() || !code->is_number_integer()) {
        return std::nullopt;
    }
    manifest.versionCode = code->get<std::int64_t>();
    if (manifest.versionCode <= 0) {
        return std::nullopt;
    }

    auto version = text_field(document, "version", kMaxFieldLength);
    auto abi = text_field(document, "abi", kMaxFieldLength);
    auto url = text_field(document, "downloadUrl", kMaxFieldLength);
    auto hash = text_field(document, "sha256", kMaxFieldLength);
    if (!version || version->empty() || !abi || abi->empty() || !url || !is_release_download(*url) || !hash
        || !is_sha256(*hash))
    {
        return std::nullopt;
    }
    manifest.version = std::move(*version);
    manifest.abi = std::move(*abi);
    manifest.downloadUrl = std::move(*url);
    manifest.sha256 = std::move(*hash);
    // Informative only: a manifest without them still updates.
    manifest.commit = text_field(document, "commit", kMaxFieldLength).value_or("");
    if (auto notes = text_field(document, "notes", kMaxNotesLength)) {
        manifest.notes = std::move(*notes);
    }
    return manifest;
}

bool offers_update(const Manifest &manifest, std::int64_t installedVersionCode, std::string_view abi)
{
    return !abi.empty() && manifest.abi == abi && manifest.versionCode > installedVersionCode;
}

} // namespace partyboard::update
