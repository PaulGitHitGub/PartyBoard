// Log line for the cross-play determinism guards in include/port/determinism.h.
//
// A guard fires when the game converts NaN or an out-of-range float to an integer,
// or divides an integer by zero: places where an Apple chip and an x86 PC would
// otherwise compute different results (or the PC would crash). The guard already
// returned the Apple-chip result; this only records that it happened, so a desync
// report can be matched against it. Rate-limited: a few lines per place in the
// code, and a cap for the whole session, because such a place can be hit every
// frame (and again during rollback resimulation).

#include "port/determinism.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {
constexpr int kLinesPerSite = 3;
constexpr int kLinesTotal = 200;
std::mutex gMutex;
std::unordered_map<std::string, int> gSeen;
int gTotal = 0;

// "…/PartyBoard-build/src/REL/m419Dll/main.c" -> "src/REL/m419Dll/main.c"; keeps
// home-folder names out of the log.
const char* ShortPath(const char* file)
{
    const char* best = file;
    for (const char* p = file; *p; ++p) {
        if ((p[0] == 's' && p[1] == 'r' && p[2] == 'c' && (p[3] == '/' || p[3] == '\\')) &&
            (p == file || p[-1] == '/' || p[-1] == '\\')) {
            best = p;
        }
    }
    return best;
}
}  // namespace

extern "C" void PartyBoard_DeterminismNote(const char* file, int line, const char* what, double value)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (gTotal > kLinesTotal) {
        return;
    }
    const char* shortFile = ShortPath(file ? file : "?");
    std::string key = std::string(shortFile) + ":" + std::to_string(line);
    int& count = gSeen[key];
    ++count;
    if (count > kLinesPerSite) {
        return;
    }
    ++gTotal;
    if (gTotal > kLinesTotal) {
        std::fprintf(stderr, "[determinism] more guard notes this session are not logged\n");
    } else {
        std::fprintf(stderr, "[determinism] %s:%d %s (value %.9g)%s\n", shortFile, line, what, value,
                     count == kLinesPerSite ? "; not logged again from here" : "");
    }
    std::fflush(stderr);
}
