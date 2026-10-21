// Continue without a dropped player (docs/ONLINE_DROP_VOTE.md, protocol 11).
// Included by netplay_runtime.cpp inside its anonymous namespace, after the
// input history, sendInput and the pause code. See DropVote for the design.
//
// Always-on log lines start with "[netplay] drop: ". Nothing here changes the
// game except dropApplyFrame(), which runs on the synchronized timeline (the same
// frame and the same inputs on every copy).

constexpr std::uint64_t kDropSilentMs = 5000;    // the banner and vote from 5 s without a packet (when the old toast came)
constexpr std::uint64_t kDropReportFreshMs = 3000;
constexpr std::uint64_t kDropSendEveryMs = 200;
constexpr std::uint64_t kDropHoldMs = 1000;      // hold START 1 s to vote (as hold START to pause)
constexpr std::uint64_t kDropCtrlGoneMs = 3000;  // a controller missing this long counts as disconnected
constexpr std::uint32_t kDropRelayFrames = 16;
constexpr std::uint8_t kCmdCtrlGone = 0x80;
constexpr std::uint8_t kCmdDirect = 0x10;
constexpr std::uint8_t kCmdVoteMask = 0x0f;
constexpr std::uint8_t kComputerDiff = 2; // a replacement computer player plays at Hard (user, 2026-10-19)

bool dropAvailable(const Runtime &runtime)
{
    return runtime.enabled && runtime.fullGame && !runtime.rollbackRequested && runtime.playerCount > 1;
}

std::uint8_t seatBit(unsigned seat) { return static_cast<std::uint8_t>(1u << seat); }

// The seats of the computer led by `lead` (its block).
std::uint8_t computerMask(const Runtime &runtime, std::uint8_t lead)
{
    const std::uint8_t count = isLocalSeat(runtime, lead) ? runtime.localSeatCount : runtime.leadSeats[lead];
    std::uint8_t mask = 0;
    for (unsigned k = 0; k < count && lead + k < rollback::kMaxPlayers; ++k) mask |= seatBit(lead + k);
    return mask;
}

std::uint8_t localMask(const Runtime &runtime)
{
    return computerMask(runtime, runtime.localPlayer);
}

std::uint8_t sessionMask(const Runtime &runtime)
{
    return static_cast<std::uint8_t>((1u << runtime.playerCount) - 1u);
}

int popcount8(std::uint8_t v)
{
    int n = 0;
    for (; v; v &= static_cast<std::uint8_t>(v - 1)) ++n;
    return n;
}

// "Player 5" / "Players 5-8" for a block of consecutive seats (1-based for people).
std::string playersText(std::uint8_t mask)
{
    std::string text;
    for (unsigned seat = 0; seat < rollback::kMaxPlayers;) {
        if (!(mask & seatBit(seat))) { ++seat; continue; }
        unsigned last = seat;
        while (last + 1 < rollback::kMaxPlayers && (mask & seatBit(last + 1))) ++last;
        if (!text.empty()) text += ", ";
        text += last == seat ? "Player " + std::to_string(seat + 1)
                             : "Players " + std::to_string(seat + 1) + "-" + std::to_string(last + 1);
        seat = last + 1;
    }
    return text.empty() ? std::string("nobody") : text;
}

// "Valentin's PC (Players 5-8)", or "Players 5-8" without a name from the lobby.
std::string computerText(const Runtime &runtime, std::uint8_t lead)
{
    const std::string players = playersText(computerMask(runtime, lead));
    const std::string &name = runtime.computerName[lead];
    const std::string &kind = runtime.computerKind[lead];
    if (name.empty()) return kind.empty() ? players : "the " + kind + " (" + players + ")";
    return name + "'s " + (kind.empty() ? std::string("computer") : kind) + " (" + players + ")";
}

std::string computersText(const Runtime &runtime, std::uint8_t mask)
{
    std::string text;
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if (!(mask & seatBit(seat)) || runtime.seatLead[seat] != seat) continue;
        if (!text.empty()) text += " and ";
        text += computerText(runtime, seat);
    }
    return text.empty() ? playersText(mask) : text;
}

// A computer the others agreed to continue without, or are about to.
bool dropIgnores(const Runtime &runtime, std::uint8_t lead)
{
    return runtime.drop.dropFrom[lead] != UINT32_MAX || (runtime.drop.frozen & seatBit(lead));
}

// Still playing: this computer, or another one not dropped.
bool liveLead(const Runtime &runtime, std::uint8_t seat)
{
    return (seat == runtime.localPlayer) || isRemoteLead(runtime, seat);
}

// Who counts the votes for a ballot about `target`: the lowest computer still
// playing that the ballot is not about. Everyone agrees on it for the same target.
std::uint8_t coordinatorFor(const Runtime &runtime, std::uint8_t target)
{
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat)
        if (liveLead(runtime, seat) && !(target & seatBit(seat))) return seat;
    return rollback::kMaxPlayers;
}

// The last frame this copy holds `lead`'s input for, in a row from the current frame.
std::uint32_t dropHigh(const Runtime &runtime, std::uint8_t lead)
{
    std::uint32_t high = runtime.frame - 1; // frames before the current one were applied with it
    const std::uint32_t cap = runtime.drop.dropFrom[lead] == UINT32_MAX ? UINT32_MAX : runtime.drop.dropFrom[lead] - 1;
    while (high + 1 - runtime.frame < kHistorySize - 1 && high + 1 <= cap && findInput(runtime.peerHistory[lead], high + 1)) ++high;
    return high;
}

struct DropTally {
    int votes = 0;
    int voters = 0;
    int needed = 0;
};

// A majority of the people still playing (all of them when there are two).
int dropNeeded(int voters) { return voters / 2 + 1; }

// The vote to continue without `target` (the silent seats), as this copy hears it.
DropTally dropWaitTally(const Runtime &runtime, std::uint8_t target)
{
    const std::uint64_t now = monotonicMs();
    const DropVote &drop = runtime.drop;
    const std::uint8_t voters = static_cast<std::uint8_t>(sessionMask(runtime) & ~target & ~drop.droppedSeats & ~drop.computerSeats);
    std::uint8_t yes = drop.silent == target ? drop.localVotes : 0;
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if (!isRemoteLead(runtime, seat) || (target & seatBit(seat))) continue;
        const DropReport &report = drop.reports[seat];
        if (!report.valid || now - report.heardMs > kDropReportFreshMs || report.silent != target) continue;
        yes |= static_cast<std::uint8_t>(report.votes & computerMask(runtime, seat));
    }
    DropTally tally;
    tally.voters = popcount8(voters);
    tally.votes = popcount8(static_cast<std::uint8_t>(yes & voters));
    tally.needed = dropNeeded(tally.voters);
    return tally;
}

// ---- Control packets ------------------------------------------------------------
void put32At(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint32_t get32At(const std::uint8_t *at)
{
    return (static_cast<std::uint32_t>(at[0]) << 24) | (static_cast<std::uint32_t>(at[1]) << 16)
        | (static_cast<std::uint32_t>(at[2]) << 8) | at[3];
}

// Payload (version 1): silent, votes, frozen, pass, commit count n, relay count m,
// 2 zero bytes, sender frame (u32); n x {lead, 3 zero, from frame u32}; m x {lead,
// seats, frames, 0, high u32, first u32, frames x {context u32, seats x (8 input + command)}}.
std::vector<std::uint8_t> dropPayload(Runtime &runtime, bool withRelay)
{
    DropVote &drop = runtime.drop;
    std::vector<std::uint8_t> bytes;
    bytes.push_back(1);
    bytes.push_back(drop.silent);
    bytes.push_back(drop.localVotes);
    bytes.push_back(drop.frozen);
    bytes.push_back(drop.pass);
    std::vector<std::uint8_t> commits, relayLeads;
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if (runtime.seatLead[seat] != seat || isLocalSeat(runtime, seat)) continue;
        if (drop.dropFrom[seat] != UINT32_MAX) commits.push_back(seat);
        const bool needed = (drop.frozen & seatBit(seat)) || drop.dropFrom[seat] != UINT32_MAX;
        if (withRelay && needed) relayLeads.push_back(seat);
    }
    bytes.push_back(static_cast<std::uint8_t>(commits.size()));
    bytes.push_back(static_cast<std::uint8_t>(relayLeads.size()));
    bytes.push_back(0);
    put32At(bytes, runtime.frame);
    for (std::uint8_t lead : commits) {
        bytes.push_back(lead);
        bytes.push_back(0); bytes.push_back(0); bytes.push_back(0);
        put32At(bytes, drop.dropFrom[lead]);
    }
    for (std::uint8_t lead : relayLeads) {
        const std::uint8_t seats = runtime.leadSeats[lead];
        const std::uint32_t high = dropHigh(runtime, lead);
        // The newest frames up to `high` this copy still holds (others are a few frames apart at most).
        const std::size_t perFrame = 4 + 9u * seats;
        const std::size_t room = kNetplayMaxControlSize > bytes.size() + 12 * relayLeads.size()
            ? (kNetplayMaxControlSize - bytes.size() - 12 * relayLeads.size()) / std::max<std::size_t>(1, relayLeads.size()) : 0;
        std::uint32_t frames = static_cast<std::uint32_t>(std::min<std::size_t>(kDropRelayFrames, room / perFrame));
        std::uint32_t first = high + 1 - std::min<std::uint32_t>(frames, high + 1);
        while (first <= high && !findInput(runtime.peerHistory[lead], first)) ++first;
        frames = first <= high ? high - first + 1 : 0;
        for (std::uint32_t f = first; f < first + frames; ++f)
            if (!findInput(runtime.peerHistory[lead], f)) { frames = f - first; break; }
        bytes.push_back(lead);
        bytes.push_back(seats);
        bytes.push_back(static_cast<std::uint8_t>(frames));
        bytes.push_back(0);
        put32At(bytes, high);
        put32At(bytes, first);
        for (std::uint32_t f = first; f < first + frames; ++f) {
            const InputSlot *slot = findInput(runtime.peerHistory[lead], f);
            put32At(bytes, slot->captureContext);
            for (std::uint8_t k = 0; k < seats; ++k) {
                const PartyBoardRollbackInput &in = slotInput(*slot, k);
                bytes.push_back(static_cast<std::uint8_t>(in.buttons >> 8));
                bytes.push_back(static_cast<std::uint8_t>(in.buttons));
                bytes.push_back(static_cast<std::uint8_t>(in.stickX));
                bytes.push_back(static_cast<std::uint8_t>(in.stickY));
                bytes.push_back(static_cast<std::uint8_t>(in.substickX));
                bytes.push_back(static_cast<std::uint8_t>(in.substickY));
                bytes.push_back(in.triggerLeft);
                bytes.push_back(in.triggerRight);
                bytes.push_back(slot->command[k]);
            }
        }
    }
    return bytes;
}

void sendDropControl(Runtime &runtime, bool force)
{
    DropVote &drop = runtime.drop;
    const std::uint64_t now = monotonicMs();
    if (!force && now - drop.lastSendMs < kDropSendEveryMs) return;
    drop.lastSendMs = now;
    InputPacket packet {};
    packet.type = PacketType::Control;
    packet.sessionId = kSessionId;
    packet.player = runtime.localPlayer;
    packet.sequence = ++drop.sequence;
    // Relay inputs only once this copy acted on a passed vote (then what it holds is final).
    packet.control = dropPayload(runtime, drop.frozen != 0 || drop.droppedSeats != 0);
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if (!isSessionRemoteLead(runtime, seat) || !runtime.transport.hasPeer(seat)) continue;
        // A dropped computer hears from us only while it is sending again (so it learns why).
        if (runtime.drop.dropFrom[seat] != UINT32_MAX && !(drop.comebackLogged & seatBit(seat))) continue;
        runtime.transport.sendInputTo(seat, packet);
    }
}

void dropAdopt(Runtime &runtime, std::uint8_t lead, std::uint32_t from, const char *how);

// Relayed inputs of a computer this copy no longer hears from: fill the frames it lacks.
void dropTakeRelay(Runtime &runtime, std::uint8_t lead, std::uint8_t seats, std::uint32_t first, std::uint32_t frames,
    const std::uint8_t *at, std::uint8_t from)
{
    if (!dropIgnores(runtime, lead) || seats != runtime.leadSeats[lead]) return;
    unsigned filled = 0;
    for (std::uint32_t f = first; f < first + frames; ++f, at += 4 + 9u * seats) {
        if (f < runtime.frame || f - runtime.frame >= kHistorySize - 1 || f >= runtime.drop.dropFrom[lead]) continue;
        InputSlot relayed {};
        relayed.frame = f;
        relayed.valid = true;
        relayed.captureContext = get32At(at);
        relayed.seats = seats;
        for (std::uint8_t k = 0; k < seats; ++k) {
            const std::uint8_t *in = at + 4 + 9u * k;
            PartyBoardRollbackInput input {};
            input.buttons = static_cast<std::uint16_t>((in[0] << 8) | in[1]);
            input.stickX = static_cast<s8>(in[2]);
            input.stickY = static_cast<s8>(in[3]);
            input.substickX = static_cast<s8>(in[4]);
            input.substickY = static_cast<s8>(in[5]);
            input.triggerLeft = in[6];
            input.triggerRight = in[7];
            if (k == 0) relayed.input = input; else relayed.extra[k - 1] = input;
            relayed.command[k] = in[8];
        }
        InputSlot &slot = runtime.peerHistory[lead][f % kHistorySize];
        if (slot.valid && slot.frame == f) {
            bool same = slot.captureContext == relayed.captureContext && slot.command == relayed.command
                && rollback::inputsEqual(slot.input, relayed.input);
            for (std::uint8_t k = 1; k < seats; ++k) same = same && rollback::inputsEqual(slot.extra[k - 1], relayed.extra[k - 1]);
            if (!same && !runtime.drop.contradictionLogged) {
                runtime.drop.contradictionLogged = true;
                std::fprintf(stdout, "[netplay] drop: WARNING: seat %u relayed a different input of %s for network frame %u than this copy holds; kept ours.\n",
                    static_cast<unsigned>(from + 1), playersText(computerMask(runtime, lead)).c_str(), f);
            }
            continue;
        }
        slot = relayed;
        ++filled;
    }
    if (filled)
        std::fprintf(stdout, "[netplay] drop: took %u frame(s) of %s's input relayed by seat %u (network frames %u-%u; this copy is at %u).\n",
            filled, playersText(computerMask(runtime, lead)).c_str(), static_cast<unsigned>(from + 1), first, first + frames - 1, runtime.frame);
}

void receiveDropControlFor(Runtime &runtime, const InputPacket &packet)
{
    DropVote &drop = runtime.drop;
    const std::uint8_t from = packet.player;
    if (!dropAvailable(runtime) || !isRemoteLead(runtime, from)) return;
    const std::vector<std::uint8_t> &bytes = packet.control;
    if (bytes.size() < 12 || bytes[0] != 1) return;
    const std::uint64_t now = monotonicMs();
    // A Control packet is a sign of life like any other.
    for (std::uint8_t k = 0; k < runtime.leadSeats[from]; ++k) runtime.peerHeardMs[from + k] = now;
    DropReport report {};
    report.valid = true;
    report.heardMs = now;
    report.silent = bytes[1];
    report.votes = bytes[2];
    report.frozen = bytes[3];
    report.pass = bytes[4];
    const std::size_t commits = bytes[5], relays = bytes[6];
    report.frame = get32At(bytes.data() + 8);
    std::size_t at = 12;
    std::vector<std::pair<std::uint8_t, std::uint32_t>> commitList;
    for (std::size_t i = 0; i < commits; ++i, at += 8) {
        if (at + 8 > bytes.size()) return;
        commitList.emplace_back(bytes[at], get32At(bytes.data() + at + 4));
    }
    struct Relay { std::uint8_t lead, seats; std::uint32_t frames, high, first; std::size_t offset; };
    std::vector<Relay> relayList;
    for (std::size_t i = 0; i < relays; ++i) {
        if (at + 12 > bytes.size()) return;
        Relay r { bytes[at], bytes[at + 1], bytes[at + 2], get32At(bytes.data() + at + 4), get32At(bytes.data() + at + 8), at + 12 };
        if (r.lead >= rollback::kMaxPlayers || r.seats == 0 || r.seats > rollback::kMaxPlayers) return;
        at += 12 + r.frames * (4 + 9u * r.seats);
        if (at > bytes.size()) return;
        relayList.push_back(r);
        report.high[r.lead] = r.high;
        report.highKnown[r.lead] = true;
    }
    DropReport &previous = drop.reports[from];
    if (previous.valid && (previous.votes != report.votes || previous.silent != report.silent) && report.votes != 0) {
        std::fprintf(stdout, "[netplay] drop: vote: %s voted to continue without %s (network frame %u there).\n",
            playersText(report.votes).c_str(), playersText(report.silent).c_str(), report.frame);
    }
    previous = report;
    // An agreement reaches every copy through every copy.
    for (const auto &[lead, fromFrame] : commitList) {
        if (lead >= runtime.playerCount || runtime.seatLead[lead] != lead) continue;
        if (isLocalSeat(runtime, lead)) {
            std::fprintf(stdout, "[netplay] drop: the others continued without this computer (%s) from network frame %u "
                "(told by seat %u); it can't rejoin this game.\n", playersText(localMask(runtime)).c_str(), fromFrame,
                static_cast<unsigned>(from + 1));
            std::fflush(stdout);
            failSession("The others continued without this computer while it was disconnected. It can't rejoin this game.");
            return;
        }
        if (drop.dropFrom[lead] == UINT32_MAX) dropAdopt(runtime, lead, fromFrame, "told by another computer");
    }
    // The coordinator of a vote declared it passed: stop listening to those seats.
    if (report.pass != 0 && !(report.pass & localMask(runtime)) && coordinatorFor(runtime, report.pass) == from
        && (drop.frozen & report.pass) == drop.frozen && drop.frozen != report.pass) {
        drop.frozen = report.pass;
        runtime.progress.commit(now); // the switch gets a fresh 2 minutes
        std::fprintf(stdout, "[netplay] drop: vote passed (announced by seat %u): this copy stops listening to %s at network frame %u "
            "and reports the last frame it holds their input for (%s).\n", static_cast<unsigned>(from + 1),
            playersText(report.pass).c_str(), runtime.frame, [&] {
                std::string text;
                for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat)
                    if ((report.pass & seatBit(seat)) && runtime.seatLead[seat] == seat)
                        text += (text.empty() ? "" : ", ") + playersText(computerMask(runtime, seat)) + ": " + std::to_string(dropHigh(runtime, seat));
                return text;
            }().c_str());
        sendDropControl(runtime, true);
    }
    // After the pass: what it holds of the computers this copy no longer hears from.
    for (const Relay &r : relayList)
        dropTakeRelay(runtime, r.lead, r.seats, r.first, r.frames, bytes.data() + r.offset, from);
}

void receiveDropControl(const InputPacket &packet)
{
    receiveDropControlFor(gRuntime, packet);
}

// The agreed switch: from network frame `from` on, `lead`'s seats are computer players.
void dropAdopt(Runtime &runtime, std::uint8_t lead, std::uint32_t from, const char *how)
{
    DropVote &drop = runtime.drop;
    const std::uint8_t mask = computerMask(runtime, lead);
    if (runtime.frame > from) {
        std::fprintf(stdout, "[netplay] drop: ERROR: the agreed frame %u for %s is behind this copy (network frame %u); "
            "this copy can't follow the others.\n", from, playersText(mask).c_str(), runtime.frame);
        std::fflush(stdout);
        failSession("Couldn't continue without the dropped player at the agreed moment.");
        return;
    }
    drop.dropFrom[lead] = from;
    drop.droppedSeats |= mask;
    drop.frozen &= static_cast<std::uint8_t>(~mask);
    drop.pass &= static_cast<std::uint8_t>(~mask);
    if (drop.silent & mask)
        std::fprintf(stdout, "[netplay] drop: countdown ended at network frame %u: the vote passed for %s.\n",
            runtime.frame, playersText(mask).c_str());
    drop.silent &= static_cast<std::uint8_t>(~mask);
    if (drop.silent == 0) drop.localVotes = 0;
    drop.activeUntilMs = monotonicMs() + 30000;
    for (std::uint8_t k = 0; k < runtime.leadSeats[lead]; ++k) {
        runtime.peerPaused[lead + k] = false;
        runtime.pauseLogged[lead + k] = false;
    }
    runtime.silentLogged[lead] = false;
    runtime.progress.commit(monotonicMs());
    std::fprintf(stdout, "[netplay] drop: agreed (%s): %s become computer players from network frame %u "
        "(this copy is at %u, holds their input to %u); the game no longer waits for that computer.\n",
        how, playersText(mask).c_str(), from, runtime.frame, dropHigh(runtime, lead));
    std::fflush(stdout);
    sendDropControl(runtime, true);
}

// The coordinator: declare the vote passed, then fix the frame once every other
// remaining computer has reported what it holds.
void dropCoordinate(Runtime &runtime)
{
    DropVote &drop = runtime.drop;
    const std::uint64_t now = monotonicMs();
    if (drop.pass == 0) {
        if (drop.silent == 0 || coordinatorFor(runtime, drop.silent) != runtime.localPlayer) return;
        const DropTally tally = dropWaitTally(runtime, drop.silent);
        if (tally.votes < tally.needed) return;
        drop.pass = drop.silent;
        drop.frozen = drop.silent;
        runtime.progress.commit(now);
        std::fprintf(stdout, "[netplay] drop: vote passed: %d of %d voted (%d needed) to continue without %s; this copy "
            "(seat %u) coordinates the switch at network frame %u.\n", tally.votes, tally.voters, tally.needed,
            computersText(runtime, drop.pass).c_str(), static_cast<unsigned>(runtime.localPlayer + 1), runtime.frame);
        sendDropControl(runtime, true);
        return;
    }
    // Every other computer still playing must have acted on the pass and said what it holds.
    std::array<std::uint32_t, rollback::kMaxPlayers> from {};
    from.fill(0);
    std::string waiting;
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if ((drop.pass & seatBit(seat)) && runtime.seatLead[seat] == seat) from[seat] = dropHigh(runtime, seat) + 1;
    }
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if (!isRemoteLead(runtime, seat) || (drop.pass & seatBit(seat))) continue;
        const DropReport &report = drop.reports[seat];
        if (!report.valid || now - report.heardMs > kDropReportFreshMs || report.frozen != drop.pass) {
            waiting += (waiting.empty() ? "" : ", ") + seatBlockText(seat, runtime.leadSeats[seat]);
            continue;
        }
        for (std::uint8_t lead = 0; lead < runtime.playerCount; ++lead) {
            if (!(drop.pass & seatBit(lead)) || runtime.seatLead[lead] != lead) continue;
            const std::uint32_t high = report.highKnown[lead] ? report.high[lead] : report.frame - 1;
            from[lead] = std::max(from[lead], high + 1);
        }
    }
    if (!waiting.empty()) {
        static std::uint64_t nextLog = 0;
        if (now >= nextLog) {
            nextLog = now + 5000;
            std::fprintf(stdout, "[netplay] drop: switch: waiting for %s to confirm the passed vote.\n", waiting.c_str());
        }
        return;
    }
    const std::uint8_t pass = drop.pass;
    for (std::uint8_t lead = 0; lead < runtime.playerCount; ++lead) {
        if (!(pass & seatBit(lead)) || runtime.seatLead[lead] != lead || drop.dropFrom[lead] != UINT32_MAX) continue;
        dropAdopt(runtime, lead, from[lead], "this copy coordinated");
    }
    drop.pass = 0;
}

void noteDroppedComeback(Runtime &runtime, std::uint8_t lead)
{
    DropVote &drop = runtime.drop;
    if (drop.dropFrom[lead] == UINT32_MAX || (drop.comebackLogged & seatBit(lead))) return;
    drop.comebackLogged |= seatBit(lead);
    std::fprintf(stdout, "[netplay] drop: %s (replaced by computer players from network frame %u) is sending again at "
        "network frame %u; it can't rejoin mid-game, its packets are ignored and it is told so.\n",
        computerText(runtime, lead).c_str(), drop.dropFrom[lead], runtime.frame);
    std::fflush(stdout);
}

void testHoldDuration(int &value, const char *name)
{
    if (const char *text = std::getenv(name); text && *text) {
        value = std::atoi(text);
        std::fprintf(stdout, "[NET TEST] %s=%d s\n", name, value);
    }
}

// Every tick attempt (also while the game waits): who is silent, the votes, the
// coordinator's work, and the Control packets.
void serviceDrop(Runtime &runtime)
{
    if (!dropAvailable(runtime)) return;
    DropVote &drop = runtime.drop;
    const std::uint64_t now = monotonicMs();
    static bool testRead = false;
    if (!testRead) {
        testRead = true;
        testHoldDuration(drop.testVoteAfterS, "PARTYBOARD_NETPLAY_TEST_DROPVOTE");
        testHoldDuration(drop.testCtrlVoteAfterS, "PARTYBOARD_NETPLAY_TEST_CTRLVOTE");
        testHoldDuration(drop.testMakeComAfterS, "PARTYBOARD_NETPLAY_TEST_MAKECOM");
    }
    std::uint8_t silent = 0;
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if (!isRemoteLead(runtime, seat)) continue;
        if ((drop.frozen & seatBit(seat)) || (runtime.peerHeardMs[seat] != 0 && now - runtime.peerHeardMs[seat] > kDropSilentMs))
            silent |= computerMask(runtime, seat);
    }
    if (silent != drop.silent) {
        const std::uint8_t before = drop.silent;
        if (before != 0) {
            const std::uint8_t back = static_cast<std::uint8_t>(before & ~silent);
            std::fprintf(stdout, "[netplay] drop: countdown ended at network frame %u: %s%s; votes so far cleared.\n",
                runtime.frame, back ? playersText(back).c_str() : playersText(before).c_str(),
                back ? " came back" : " are waited for together with others now");
        }
        drop.silent = silent;
        drop.localVotes = 0;
        drop.holdFired = {};
        drop.tallyLogged.clear();
        if (silent != 0) {
            drop.waitStartMs = now;
            const std::uint64_t left = runtime.progress.remaining(now);
            std::fprintf(stdout, "[netplay] drop: countdown started at network frame %u: waiting for %s; the game stops in "
                "%llu:%02llu unless they come back or a majority of the others votes to continue without them.\n",
                runtime.frame, computersText(runtime, silent).c_str(),
                static_cast<unsigned long long>(left / 60000), static_cast<unsigned long long>((left / 1000) % 60));
        }
        sendDropControl(runtime, true);
    }
    // Hold START 1 s to vote, per seat of this computer (or the menu, or a test).
    if (drop.silent != 0 && drop.pass == 0) {
        for (std::uint8_t k = 0; k < runtime.localSeatCount; ++k) {
            LocalSeat &ls = runtime.local[k];
            const std::uint8_t bit = seatBit(ls.seat);
            if ((drop.localVotes & bit) || (drop.computerSeats & bit)) continue;
            const bool down = (capturePad(ls.pad).buttons & PAD_BUTTON_START) != 0;
            const bool test = drop.testVoteAfterS >= 0 && now - drop.waitStartMs >= static_cast<std::uint64_t>(drop.testVoteAfterS) * 1000u;
            if (down && drop.holdSinceMs[k] == 0) drop.holdSinceMs[k] = now;
            if (!down) drop.holdSinceMs[k] = 0;
            if (test || (down && now - drop.holdSinceMs[k] >= kDropHoldMs)) {
                drop.localVotes |= bit;
                ls.suppressStart = true; // that START never reaches the game
                drop.holdSinceMs[k] = 0;
                std::fprintf(stdout, "[netplay] drop: vote: player %u (this computer) voted to continue without %s (%s) at network frame %u.\n",
                    static_cast<unsigned>(ls.seat + 1), playersText(drop.silent).c_str(), test ? "test script" : "held START",
                    runtime.frame);
                sendDropControl(runtime, true);
            }
        }
    }
    if (drop.silent != 0) {
        const DropTally tally = dropWaitTally(runtime, drop.silent);
        const std::string text = std::to_string(tally.votes) + " of " + std::to_string(tally.voters) + " voted, "
            + std::to_string(tally.needed) + " needed";
        if (text != drop.tallyLogged) {
            drop.tallyLogged = text;
            std::fprintf(stdout, "[netplay] drop: vote to continue without %s: %s (network frame %u).\n",
                playersText(drop.silent).c_str(), text.c_str(), runtime.frame);
        }
    }
    dropCoordinate(runtime);
    // Also while a replaced computer is sending again: it must learn it was replaced.
    const bool active = drop.silent != 0 || drop.frozen != 0 || drop.pass != 0 || now < drop.activeUntilMs
        || (drop.comebackLogged & drop.droppedSeats) != 0;
    if (active) sendDropControl(runtime, false);
}

// ---- The seat's command byte (in the input: every copy reads it on its frame) ----

// This computer's controllers, once a second (logSeatControllers): a controller
// that fed a seat and is gone for 3 s is reported as disconnected.
void dropWatchController(Runtime &runtime, std::uint8_t k, bool any)
{
    DropVote &drop = runtime.drop;
    const std::uint64_t now = monotonicMs();
    const LocalSeat &ls = runtime.local[k];
    if (any) {
        drop.ctrlSeen[k] = true;
        drop.ctrlNothingSinceMs[k] = 0;
        if (drop.ctrlGone[k]) {
            drop.ctrlGone[k] = false;
            std::fprintf(stdout, "[netplay] drop: controller: player %u's controller (this computer's port %u) is back at network frame %u.\n",
                static_cast<unsigned>(ls.seat + 1), static_cast<unsigned>(ls.pad + 1), runtime.frame);
        }
        return;
    }
    if (!drop.ctrlSeen[k] || drop.ctrlGone[k]) return;
    if (drop.ctrlNothingSinceMs[k] == 0) drop.ctrlNothingSinceMs[k] = now;
    if (now - drop.ctrlNothingSinceMs[k] >= kDropCtrlGoneMs && !(drop.computerSeats & seatBit(ls.seat))) {
        drop.ctrlGone[k] = true;
        std::fprintf(stdout, "[netplay] drop: controller: player %u's controller (this computer's port %u) disconnected "
            "(nothing for 3 s) at network frame %u; told to every computer.\n",
            static_cast<unsigned>(ls.seat + 1), static_cast<unsigned>(ls.pad + 1), runtime.frame);
    }
}

std::uint8_t dropLocalCommand(const Runtime &runtime, std::uint8_t k)
{
    const DropVote &drop = runtime.drop;
    std::uint8_t command = drop.ctrlGone[k] ? kCmdCtrlGone : 0;
    if (drop.voteFor[k] != 0) command |= static_cast<std::uint8_t>((drop.voteFor[k] & kCmdVoteMask) | (drop.direct[k] ? kCmdDirect : 0));
    return command;
}

const char *characterName(int character)
{
    static const char *const names[] = { "Mario", "Luigi", "Peach", "Yoshi", "Wario", "Donkey Kong", "Daisy", "Waluigi" };
    return character >= 0 && character < 8 ? names[character] : "?";
}

// The device port (= online seat) that drives game player i.
int playerDevice(int i)
{
    if (GWSystem.eight_players) {
        const s32 own = GWPlayerOwnPortGet(&GWPlayer[i]);
        if (own >= 0) return own;
    }
    return GWPlayerCfg[i].pad_idx;
}

// Game players driven by `mask`'s seats become computer players (board and
// minigame records both). Deterministic: game state and the agreed seats only.
int dropMakeComputers(std::uint8_t mask, bool quiet)
{
    int changed = 0;
    for (int i = 0; i < GWPlayerCount(); ++i) {
        const int device = playerDevice(i);
        if (device < 0 || device >= 8 || !(mask & seatBit(static_cast<unsigned>(device)))) continue;
        if (GWPlayer[i].com && GWPlayerCfg[i].iscom) continue;
        GWPlayer[i].com = 1;
        GWPlayer[i].diff = kComputerDiff;
        GWPlayerCfg[i].iscom = 1;
        GWPlayerCfg[i].diff = kComputerDiff;
        ++changed;
        if (!quiet)
            std::fprintf(stdout, "[netplay] drop: game player %d (%s, driven by online seat %d) is a computer player now (COM hard).\n",
                i + 1, characterName(GWPlayer[i].character), device + 1);
    }
    return changed;
}

// The person is back: game players driven by `mask`'s seats are people again.
int dropMakePeople(std::uint8_t mask)
{
    int changed = 0;
    for (int i = 0; i < GWPlayerCount(); ++i) {
        const int device = playerDevice(i);
        if (device < 0 || device >= 8 || !(mask & seatBit(static_cast<unsigned>(device)))) continue;
        if (!GWPlayer[i].com && !GWPlayerCfg[i].iscom) continue;
        GWPlayer[i].com = 0;
        GWPlayerCfg[i].iscom = 0;
        ++changed;
        std::fprintf(stdout, "[netplay] drop: game player %d (%s, driven by online seat %d) is a person again.\n",
            i + 1, characterName(GWPlayer[i].character), device + 1);
    }
    return changed;
}

bool contextPlaysParty(int context)
{
    using O = MenuProbeOverlay;
    if (contextIsMinigame(context)) return true;
    for (O o : { O::instDll, O::w01Dll, O::w02Dll, O::w03Dll, O::w04Dll, O::w05Dll, O::w06Dll })
        if (context == static_cast<int>(o)) return true;
    return false;
}

void dropSwitch(Runtime &runtime, std::uint8_t mask, std::uint32_t frame, const char *why, bool controller = false)
{
    runtime.drop.computerSeats |= mask;
    if (controller) runtime.drop.ctrlComputerSeats |= mask;
    std::fprintf(stdout, "[netplay] drop: switch at network frame %u: %s become computer players (%s); seats now played by computers: %s.\n",
        frame, playersText(mask).c_str(), why, playersText(runtime.drop.computerSeats).c_str());
    if (dropMakeComputers(mask, false) == 0)
        std::fprintf(stdout, "[netplay] drop: no game player uses %s right now (context %d); they become computer players when the party has them.\n",
            playersText(mask).c_str(), runtime.observedContext);
    std::fflush(stdout);
}

// On the synchronized timeline, after every seat's input for `frame` is known and
// before the game runs it: agreed drops reaching their frame, the command bytes'
// controller reports and votes, and computer players' pads held neutral.
void dropApplyFrame(Runtime &runtime, std::uint32_t frame, const InputSlot *localSlot,
    const std::array<const InputSlot *, rollback::kMaxPlayers> &peerSlot,
    std::array<PartyBoardRollbackInput, rollback::kMaxPlayers> &applied)
{
    if (!dropAvailable(runtime)) return;
    DropVote &drop = runtime.drop;
    for (std::uint8_t lead = 0; lead < runtime.playerCount; ++lead) {
        if (drop.dropFrom[lead] == UINT32_MAX || frame < drop.dropFrom[lead]) continue;
        const std::uint8_t mask = static_cast<std::uint8_t>(computerMask(runtime, lead) & ~drop.computerSeats);
        if (mask) dropSwitch(runtime, mask, frame, "their computer dropped and the others voted to continue");
    }
    std::array<std::uint8_t, rollback::kMaxPlayers> command {};
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if (isLocalSeat(runtime, seat)) {
            if (localSlot) command[seat] = localSlot->command[seat - runtime.localPlayer];
            continue;
        }
        const std::uint8_t lead = runtime.seatLead[seat];
        if (frame < drop.dropFrom[lead] && peerSlot[lead] != nullptr) command[seat] = peerSlot[lead]->command[seat - lead];
    }
    const std::uint8_t playing = static_cast<std::uint8_t>(sessionMask(runtime) & ~drop.computerSeats);
    std::uint8_t gone = 0;
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if ((playing & seatBit(seat)) && (command[seat] & kCmdCtrlGone)) gone |= seatBit(seat);
        if (command[seat] != drop.lastCommand[seat]) {
            const std::uint8_t was = drop.lastCommand[seat], now = command[seat];
            drop.lastCommand[seat] = now;
            if ((was ^ now) & kCmdCtrlGone)
                std::fprintf(stdout, "[netplay] drop: controller: player %u's controller is %s at network frame %u (every copy).\n",
                    static_cast<unsigned>(seat + 1), (now & kCmdCtrlGone) ? "disconnected" : "back", frame);
            if (((was ^ now) & (kCmdVoteMask | kCmdDirect)) && (now & kCmdVoteMask))
                std::fprintf(stdout, "[netplay] drop: vote: player %u %s player %u a computer player (network frame %u, every copy).\n",
                    static_cast<unsigned>(seat + 1), (now & kCmdDirect) ? "makes (own computer, F1 menu)" : "votes to make",
                    static_cast<unsigned>((now & kCmdVoteMask)), frame);
        }
    }
    drop.ctrlGoneSeats = gone;
    // A seat replaced because its controller was gone gets it back when the
    // controller returns (its computer says so in the seat's command byte).
    std::uint8_t back = 0;
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        const std::uint8_t lead = runtime.seatLead[seat];
        if ((drop.ctrlComputerSeats & seatBit(seat)) && !(command[seat] & kCmdCtrlGone)
            && (isLocalSeat(runtime, seat) ? localSlot != nullptr : (frame < drop.dropFrom[lead] && peerSlot[lead] != nullptr)))
            back |= seatBit(seat);
    }
    if (back) {
        drop.computerSeats &= static_cast<std::uint8_t>(~back);
        drop.ctrlComputerSeats &= static_cast<std::uint8_t>(~back);
        std::fprintf(stdout, "[netplay] drop: hand back at network frame %u: %s %s; seats now played by computers: %s.\n", frame,
            playersText(back).c_str(), popcount8(back) > 1 ? "have their controllers again and play on as people"
                                                             : "has their controller again and plays on as a person",
            drop.computerSeats ? playersText(drop.computerSeats).c_str() : "none");
        if (dropMakePeople(back) == 0)
            std::fprintf(stdout, "[netplay] drop: no game player uses %s right now (context %d).\n", playersText(back).c_str(),
                runtime.observedContext);
        std::fflush(stdout);
    }
    const std::uint8_t playingNow = static_cast<std::uint8_t>(sessionMask(runtime) & ~drop.computerSeats);
    std::uint8_t toSwitch = 0;
    std::string why;
    for (std::uint8_t target = 0; target < runtime.playerCount; ++target) {
        const std::uint8_t bit = seatBit(target);
        if (!(playingNow & bit)) { drop.tally[target] = 0; drop.tallyVoters[target] = 0; continue; }
        bool direct = false;
        std::uint8_t yes = 0;
        const std::uint8_t voters = static_cast<std::uint8_t>(playingNow & ~bit);
        for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
            if ((command[seat] & kCmdVoteMask) != target + 1) continue;
            if ((command[seat] & kCmdDirect) && runtime.seatLead[seat] == runtime.seatLead[target]) direct = true;
            else if (voters & seatBit(seat)) yes |= seatBit(seat);
        }
        const int votes = popcount8(yes), count = popcount8(voters), needed = dropNeeded(count);
        if ((gone & bit) && (yes != drop.tally[target] || voters != drop.tallyVoters[target]))
            std::fprintf(stdout, "[netplay] drop: vote to make player %u a computer player: %d of %d voted, %d needed (network frame %u).\n",
                static_cast<unsigned>(target + 1), votes, count, needed, frame);
        drop.tally[target] = yes;
        drop.tallyVoters[target] = voters;
        if (direct) {
            toSwitch |= bit;
            why = "chosen by their own computer in the F1 menu";
        } else if ((gone & bit) && votes >= needed) {
            toSwitch |= bit;
            why = "controller disconnected; vote passed " + std::to_string(votes) + " of " + std::to_string(count);
        }
    }
    if (toSwitch) dropSwitch(runtime, toSwitch, frame, why.c_str(), true);
    // Keep them computer players wherever the party is (a new board, the next minigame).
    if (drop.computerSeats && contextPlaysParty(runtime.observedContext)) {
        const int changed = dropMakeComputers(drop.computerSeats, true);
        if (changed)
            std::fprintf(stdout, "[netplay] drop: %d game player(s) driven by %s set to computer players again at network frame %u (context %d).\n",
                changed, playersText(drop.computerSeats).c_str(), frame, runtime.observedContext);
    }
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat)
        if (drop.computerSeats & seatBit(seat)) applied[seat] = {};
    // This computer's people stop voting for a seat that is settled or whose controller is back.
    for (std::uint8_t k = 0; k < runtime.localSeatCount; ++k) {
        const std::uint8_t target = drop.voteFor[k];
        if (target == 0) continue;
        const std::uint8_t bit = seatBit(target - 1u);
        if ((drop.computerSeats & bit) || (!drop.direct[k] && !(gone & bit) && (command[runtime.local[k].seat] & kCmdVoteMask) == target)) {
            drop.voteFor[k] = 0;
            drop.direct[k] = false;
        }
    }
}

// This computer's people vote to make `target` a computer player (every seat of
// this computer but the target), or, for a seat of this computer, its people
// choose it (`direct`: carried by this computer's first seat). False when nothing changed.
bool dropCastLocal(Runtime &runtime, std::uint8_t target, bool direct)
{
    DropVote &drop = runtime.drop;
    bool changed = false;
    // One person: this computer's first seat that can (the F1 choice, or a test aid).
    for (std::uint8_t k = 0; k < runtime.localSeatCount; ++k) {
        if (runtime.local[k].seat == target && !direct) continue;
        if (drop.computerSeats & seatBit(runtime.local[k].seat)) continue;
        if (drop.voteFor[k] == target + 1 && drop.direct[k] == direct) return false;
        drop.voteFor[k] = static_cast<std::uint8_t>(target + 1);
        drop.direct[k] = direct;
        changed = true;
        break;
    }
    return changed;
}

// Test aids: vote on a controller drop / make this computer's disconnected seat a
// computer player, N s after the banner appears (PARTYBOARD_NETPLAY_TEST_CTRLVOTE / _MAKECOM).
void dropServiceControllerTests(Runtime &runtime)
{
    DropVote &drop = runtime.drop;
    const std::uint64_t now = monotonicMs();
    const std::uint8_t gone = static_cast<std::uint8_t>(drop.ctrlGoneSeats & ~drop.computerSeats);
    if (gone == 0) { drop.ctrlBannerSinceMs = 0; return; }
    if (drop.ctrlBannerSinceMs == 0) drop.ctrlBannerSinceMs = now;
    std::uint8_t target = 0;
    while (!(gone & seatBit(target))) ++target;
    const bool mine = isLocalSeat(runtime, target);
    const std::uint64_t waited = now - drop.ctrlBannerSinceMs;
    const bool vote = !mine && drop.testCtrlVoteAfterS >= 0 && waited >= static_cast<std::uint64_t>(drop.testCtrlVoteAfterS) * 1000u;
    const bool make = mine && drop.testMakeComAfterS >= 0 && waited >= static_cast<std::uint64_t>(drop.testMakeComAfterS) * 1000u;
    if (!vote && !make) return;
    if (dropCastLocal(runtime, target, make))
        std::fprintf(stdout, "[NET TEST] drop: this computer %s player %u a computer player (test script).\n",
            make ? "makes" : "votes to make", static_cast<unsigned>(target + 1));
}

// Self-test (--netplay-self-test, "drop-vote"): the majority rule, the frames a
// dropped computer is still needed for, and two remaining computers agreeing on
// the switch frame from what each holds, one relaying the inputs the other lacks.
bool dropVoteSelfTest()
{
    if (dropNeeded(1) != 1 || dropNeeded(2) != 2 || dropNeeded(3) != 2 || dropNeeded(4) != 3 || dropNeeded(7) != 4) return false;
    // Seats 0 (A), 1 (B), 2 (C, silent). A holds C's input to frame 12, B to 10.
    auto aOwner = std::make_unique<Runtime>(), bOwner = std::make_unique<Runtime>();
    Runtime &a = *aOwner, &b = *bOwner;
    for (Runtime *r : { &a, &b }) {
        r->enabled = true;
        r->fullGame = true;
        r->playerCount = 3;
        r->frame = 9;
        r->localSeatCount = 1;
        // The other remaining computer's own input, frames 9-13.
        for (std::uint32_t f = 9; f <= 13; ++f) r->peerHistory[r == &a ? 1 : 0][f % kHistorySize] = InputSlot { f, {}, true };
        for (std::uint32_t f = 9; f <= (r == &a ? 12u : 10u); ++f) {
            InputSlot slot { f, {}, true };
            slot.input.buttons = static_cast<std::uint16_t>(0x100 + f);
            slot.command[0] = static_cast<std::uint8_t>(f);
            r->peerHistory[2][f % kHistorySize] = slot;
        }
    }
    a.localPlayer = 0;
    b.localPlayer = 1;
    std::array<const InputSlot *, rollback::kMaxPlayers> slots {};
    // Both still need C: nothing advances without it.
    if (!collectPeerInputs(a, 9, slots) || collectPeerInputs(a, 13, slots)) return false;
    if (dropHigh(a, 2) != 12 || dropHigh(b, 2) != 10) return false;
    if (coordinatorFor(a, 0x04) != 0 || coordinatorFor(b, 0x04) != 0 || coordinatorFor(a, 0x01) != 1) return false;
    // A coordinates: two voters, both voted.
    a.drop.silent = b.drop.silent = 0x04;
    a.drop.localVotes = 0x01;
    b.drop.localVotes = 0x02;
    InputPacket fromB {};
    fromB.type = PacketType::Control;
    fromB.player = 1;
    fromB.control = dropPayload(b, false);
    receiveDropControlFor(a, fromB);
    const DropTally tally = dropWaitTally(a, 0x04);
    if (tally.votes != 2 || tally.voters != 2 || tally.needed != 2) return false;
    dropCoordinate(a); // declares the vote passed and acts on it
    if (a.drop.pass != 0x04 || a.drop.frozen != 0x04) return false;
    // B hears the pass, acts on it, and reports with C's inputs; A relays its own to B.
    InputPacket fromA {};
    fromA.type = PacketType::Control;
    fromA.player = 0;
    fromA.control = dropPayload(a, true);
    receiveDropControlFor(b, fromA);
    if (b.drop.frozen != 0x04 || dropHigh(b, 2) != 12) return false; // frames 11-12 taken from A's relay
    const InputSlot *relayed = findInput(b.peerHistory[2], 12);
    if (!relayed || relayed->input.buttons != 0x10c || relayed->command[0] != 12) return false;
    fromB.control = dropPayload(b, true);
    receiveDropControlFor(a, fromB);
    dropCoordinate(a); // everyone reported: the first frame without C is 13 on both
    if (a.drop.dropFrom[2] != 13 || a.drop.pass != 0) return false;
    fromA.control = dropPayload(a, true);
    receiveDropControlFor(b, fromA);
    if (b.drop.dropFrom[2] != 13 || b.drop.droppedSeats != 0x04) return false;
    // From frame 13 on C is not waited for; before it, it still is.
    if (!collectPeerInputs(b, 12, slots) || slots[2] == nullptr || !collectPeerInputs(b, 13, slots) || slots[2] != nullptr) return false;
    return true;
}

// Hold START 1 s while another seat's controller is gone: this person's vote to
// make that seat a computer player, instead of a pause (one vote per person, the
// user's choice 2026-10-19). False when there is nothing to vote on: the hold pauses.
bool dropHoldVotes(Runtime &runtime, LocalSeat &ls)
{
    if (!dropAvailable(runtime)) return false;
    DropVote &drop = runtime.drop;
    const std::uint8_t k = static_cast<std::uint8_t>(ls.seat - runtime.localPlayer);
    const std::uint8_t gone = static_cast<std::uint8_t>(drop.ctrlGoneSeats & ~drop.computerSeats & ~seatBit(ls.seat));
    if (gone == 0 || (drop.computerSeats & seatBit(ls.seat))) return false;
    std::uint8_t target = 0;
    while (!(gone & seatBit(target))) ++target;
    if (drop.voteFor[k] == target + 1) return false; // already voted: the hold pauses as usual
    drop.voteFor[k] = static_cast<std::uint8_t>(target + 1);
    drop.direct[k] = false;
    std::fprintf(stdout, "[netplay] drop: vote: player %u (this computer) voted to make player %u a computer player (held START, "
        "no pause) at network frame %u.\n", static_cast<unsigned>(ls.seat + 1), static_cast<unsigned>(target + 1), runtime.frame);
    return true;
}
