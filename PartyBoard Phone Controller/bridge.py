#!/usr/bin/env python3
"""Phone-as-controller bridge for PartyBoard.

Serves a touch gamepad page to phones on the local network and forwards
their input to PartyBoard's scripted-input socket (src/port/test_input.cpp),
which PartyBoard opens on 127.0.0.1 when started with PARTYBOARD_TEST_INPUT.

Up to four phones, one per player. Each phone holds its player's pad with
"S <pad> <buttons> <x> <y>" lines (repeated while it is connected; the game
releases a pad that goes a second without one) and "R <pad>" when it leaves.
"#phones <mask>" tells the game which players have phones, for Settings.

A phone keeps its player across reconnects (lock screen, reload): the page
sends a client id it keeps in local storage, and the bridge remembers which
player that id last had.

Why phones drop is logged: every disconnect gets one line in bridge.log (and
a short one in game.log, sent to the game as "#note <text>") with the reason
(the phone's close code, no message for DROP_AFTER seconds, the page being
hidden, or a network error), how long it was connected and its last ping. The
bridge pings each phone every PING_EVERY seconds (WebSocket ping frames, which
browsers answer by themselves) and logs the round trip once a minute. The page
reports when it is hidden or visible again ("vis:"), its screen wake lock
("wake:"), and after a reconnect how its previous connection ended ("prev:").

Standard library only (macOS's built-in python3 is enough; on Windows,
partyboard.exe starts it with Python 3.7 or later from python.org, see
src/port/phone_bridge_win.cpp).
"""

import argparse
import asyncio
import base64
import hashlib
import hmac
import os
import struct
import sys
import time
from urllib.parse import parse_qs, urlsplit

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
HERE = os.path.dirname(os.path.abspath(__file__))
PADS = 4
# Guessing the access code: a device that gets it wrong this many times within
# the window is turned away for the lockout, so the code cannot be brute-forced
# from the same Wi-Fi.
MAX_WRONG_CODES = 5
WRONG_CODE_WINDOW = 600.0
LOCKOUT = 600.0
PHONE_TIMEOUT = 1.0      # seconds without a message before a phone's input is released
KEEPALIVE = 0.25         # how often held pads are re-sent (the game times out at 1 s)
# The page sends its state every 150 ms; a phone silent this long (asleep,
# locked, off the Wi-Fi) is disconnected, so the reason is logged and the
# player shows as free, instead of a dead connection lingering.
DROP_AFTER = 20.0
PING_EVERY = 5.0         # WebSocket ping to each phone, for its round-trip time
PING_LOG_EVERY = 60.0    # how often each phone's ping is written to bridge.log

NEUTRAL = (0, 0, 0)
BUTTON_NAMES = [(0x100, "A"), (0x200, "B"), (0x400, "X"), (0x800, "Y"), (0x1000, "START"),
                (0x10, "Z"), (0x20, "R"), (0x40, "L"),
                (0x8, "UP"), (0x4, "DOWN"), (0x1, "LEFT"), (0x2, "RIGHT")]


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def stamp():
    t = time.time()
    return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t % 1 * 1000):03d}"


class InputTrace:
    """Writes every phone button press/release and coarse stick moves, per
    player, with millisecond timestamps, to line a crash up with its input."""

    def __init__(self, path):
        self.f = open(path, "a", buffering=1) if path else None
        self.last = {}

    def event(self, text):
        if self.f:
            self.f.write(f"{stamp()} {text}\n")

    def state(self, pad, new):
        old = self.last.get(pad, NEUTRAL)
        if not self.f or new == old:
            return
        ob, ox, oy = old
        nb, nx, ny = new
        who = f"P{pad + 1}"
        for bit, name in BUTTON_NAMES:
            if (nb & bit) and not (ob & bit):
                self.event(f"{who} press   {name}")
            elif (ob & bit) and not (nb & bit):
                self.event(f"{who} release {name}")
        if self._stick_bucket(nx, ny) != self._stick_bucket(ox, oy):
            self.event(f"{who} stick   x={nx:+4d} y={ny:+4d}")
        self.last[pad] = new

    @staticmethod
    def _stick_bucket(x, y):
        # Log only when the stick changes direction or crosses the game's
        # menu threshold (|5|) / half / full tilt, not on every wiggle.
        def b(v):
            a = abs(v)
            return 0 if a < 5 else (1 if a < 64 else 2) * (1 if v > 0 else -1)
        return b(x), b(y)


class Phone:
    def __init__(self, writer, peer):
        self.writer = writer
        self.peer = peer
        self.client_id = None
        self.pad = None          # 0-3 once it has a player
        self.state = NEUTRAL
        self.last_msg = time.monotonic()
        self.released = True     # game told "R" since the last input
        # For the disconnect line.
        self.connected_at = time.monotonic()
        self.last_seen = self.connected_at   # any frame, including pongs
        self.end_reason = None   # set by the bridge when it ends the connection
        self.hidden_since = None # the page said it was hidden (screen off, app switch)
        self.wake = None         # the page's screen wake lock: held / unsupported / released / failed ...
        self.logged_pad = None   # the player in the log lines
        # Ping: id -> time sent; round trips since the last ping log line.
        self.ping_id = 0
        self.pings = {}
        self.rtt = None          # last round trip, ms
        self.rtts = []
        self.unanswered = 0
        self.ping_logged = self.connected_at

    def active(self):
        return time.monotonic() - self.last_msg < PHONE_TIMEOUT

    def who(self):
        pad = self.pad if self.pad is not None else self.logged_pad
        return f"Player {pad + 1}'s phone ({self.peer})" if pad is not None else f"Phone from {self.peer}"

    def short(self):
        pad = self.pad if self.pad is not None else self.logged_pad
        return f"P{pad + 1} phone" if pad is not None else f"phone {self.peer}"


class Bridge:
    def __init__(self, token, game_port, trace=None):
        self.token = token
        self.game_port = game_port
        self.trace = trace or InputTrace(None)
        self.phones = []             # connected phones, join order
        self.wrong_codes = {}        # device address -> times of recent wrong codes
        self.locked_until = {}       # device address -> time its lockout ends
        self.last_pad = {}           # client id -> player it last had
        self.game_writer = None
        self.controller_mask = 0     # players with a real controller (from the game)

    # ---- players ---------------------------------------------------------

    def used_pads(self, ignore=None):
        """Players taken by another phone or by a real controller."""
        used = {p.pad for p in self.phones if p.pad is not None and p is not ignore}
        used |= {pad for pad in range(PADS) if self.controller_mask >> pad & 1}
        return used

    async def set_controllers(self, mask):
        """The game says which players have a controller: a phone on one of
        them moves to the next free player (or stays, sharing, if none)."""
        if mask == self.controller_mask:
            return
        self.controller_mask = mask
        names = [str(pad + 1) for pad in range(PADS) if mask >> pad & 1] or ["none"]
        log(f"Controllers on players: {', '.join(names)}")
        moved = False
        for phone in list(self.phones):
            if phone.pad is not None and mask >> phone.pad & 1:
                new = self.pick_pad(phone)
                if new is not None:
                    old = phone.pad
                    self.release(phone, old)
                    phone.pad = new
                    moved = True
                    log(f"Phone from {phone.peer} moved from player {old + 1} to {new + 1} (a controller took player {old + 1}).")
                    self.trace.event(f"P{old + 1} phone moved to P{new + 1}: controller connected")
        if moved:
            await self.broadcast_status()

    def pick_pad(self, phone, wanted=None):
        """The player for a phone: the one asked for if free, else the one
        this phone had before if free, else the lowest free one."""
        used = self.used_pads(ignore=phone)
        for choice in (wanted, self.last_pad.get(phone.client_id)):
            if choice is not None and 0 <= choice < PADS and choice not in used:
                return choice
        free = [p for p in range(PADS) if p not in used]
        return free[0] if free else None

    def next_free_pad(self, phone):
        """For "tap to switch": the next free player after this phone's."""
        used = self.used_pads(ignore=phone)
        start = phone.pad if phone.pad is not None else -1
        for step in range(1, PADS + 1):
            p = (start + step) % PADS
            if p not in used:
                return p
        return phone.pad

    def phone_mask(self):
        mask = 0
        for p in self.phones:
            if p.pad is not None:
                mask |= 1 << p.pad
        return mask

    # ---- game side -------------------------------------------------------

    def send_game(self, line):
        if self.game_writer is not None:
            try:
                self.game_writer.write((line + "\n").encode())
            except (ConnectionError, OSError):
                pass

    def note(self, text):
        """A short line for game.log (the game prints it as "Phone controller: ...")."""
        self.send_game("#note " + text)

    def hold(self, phone):
        if phone.pad is None:
            return
        b, x, y = phone.state
        self.send_game(f"S {phone.pad} {b:x} {x} {y}")
        phone.released = False

    def release(self, phone, pad=None):
        pad = phone.pad if pad is None else pad
        if pad is not None:
            self.send_game(f"R {pad}")
        phone.released = True

    async def game_link(self):
        announced_wait = False
        while True:
            try:
                reader, writer = await asyncio.open_connection("127.0.0.1", self.game_port)
            except OSError:
                if not announced_wait:
                    log(f"Waiting for PartyBoard on 127.0.0.1:{self.game_port} ...")
                    announced_wait = True
                await asyncio.sleep(1)
                continue
            announced_wait = False
            log("Connected to PartyBoard.")
            self.game_writer = writer
            await self.broadcast_status()
            try:
                while True:
                    line = await reader.readline()
                    if not line:
                        break
                    # "#controllers <mask>": players with a real controller.
                    # ("ok" lines belong to scripted tests.)
                    if line.startswith(b"#controllers "):
                        try:
                            await self.set_controllers(int(line.split()[1]) & 0xF)
                        except ValueError:
                            pass
            except (ConnectionError, OSError):
                pass
            self.game_writer = None
            self.controller_mask = 0
            log("PartyBoard closed the connection.")
            await self.broadcast_status()

    async def keepalive(self):
        """Re-send each held pad so the game keeps it; release silent phones."""
        while True:
            await asyncio.sleep(KEEPALIVE)
            now = time.monotonic()
            for phone in list(self.phones):
                if phone.active():
                    self.hold(phone)
                elif not phone.released:
                    phone.state = NEUTRAL
                    self.trace.state(phone.pad, NEUTRAL)
                    self.release(phone)
                if phone.end_reason is None and now - phone.last_seen > DROP_AFTER:
                    phone.end_reason = f"timed out (no message for {DROP_AFTER:.0f} s)"
                    # abort, not close: close waits to send what is queued,
                    # which a phone that is gone never takes.
                    phone.writer.transport.abort()

    async def pinger(self):
        """Pings every phone every few seconds; logs each one's round trip
        about once a minute."""
        while True:
            await asyncio.sleep(PING_EVERY)
            now = time.monotonic()
            for phone in list(self.phones):
                if phone.end_reason is not None:
                    continue
                # A ping still unanswered after the next one is due counts as lost.
                lost = [i for i, t in phone.pings.items() if now - t > PING_EVERY]
                for i in lost:
                    del phone.pings[i]
                phone.unanswered += len(lost)
                phone.ping_id = (phone.ping_id + 1) & 0xFFFFFFFF
                phone.pings[phone.ping_id] = now
                try:
                    phone.writer.write(bytes([0x89, 4]) + struct.pack(">I", phone.ping_id))
                except (ConnectionError, OSError, RuntimeError):
                    pass
                if now - phone.ping_logged >= PING_LOG_EVERY:
                    phone.ping_logged = now
                    if phone.rtts:
                        r = phone.rtts
                        log(f"{phone.who()}: ping {phone.rtt:.0f} ms (last minute: min {min(r):.0f}, "
                            f"avg {sum(r) / len(r):.0f}, max {max(r):.0f} ms, {len(r)} answered, "
                            f"{phone.unanswered} unanswered)")
                    else:
                        log(f"{phone.who()}: no ping answered in the last minute ({phone.unanswered} unanswered)")
                    phone.rtts = []
                    phone.unanswered = 0

    def on_pong(self, phone, payload):
        if len(payload) != 4:
            return
        sent = phone.pings.pop(struct.unpack(">I", payload)[0], None)
        if sent is not None:
            phone.rtt = (time.monotonic() - sent) * 1000
            phone.rtts.append(phone.rtt)

    # ---- phone side ------------------------------------------------------

    async def tell(self, phone, msg):
        try:
            await ws_send(phone.writer, msg)
        except (ConnectionError, OSError):
            pass

    async def broadcast_status(self):
        """Tell every phone its player and whether the game is there, and the
        game which players have phones (shown in Settings > Input)."""
        self.send_game(f"#phones {self.phone_mask()}")
        game = "game:on" if self.game_writer is not None else "game:off"
        for phone in list(self.phones):
            await self.tell(phone, game)
            await self.tell(phone, f"player:{phone.pad + 1}" if phone.pad is not None else "player:0")
            if phone.state != NEUTRAL or phone.active():
                self.hold(phone)

    async def handle(self, reader, writer):
        try:
            request = await reader.readuntil(b"\r\n\r\n")
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
            writer.close()
            return
        lines = request.decode("latin-1").split("\r\n")
        try:
            method, target, _ = lines[0].split(" ", 2)
        except ValueError:
            writer.close()
            return
        headers = {}
        for h in lines[1:]:
            if ":" in h:
                k, v = h.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        url = urlsplit(target)
        query = parse_qs(url.query)

        if url.path == "/ws" and headers.get("upgrade", "").lower() == "websocket":
            peer = writer.get_extra_info("peername")
            ip = peer[0] if peer else "?"
            now = time.monotonic()
            if self.locked_until.get(ip, 0) > now:
                await http_reply(writer, 429, "text/plain", b"Too many wrong codes. Try again later.")
                return
            if not hmac.compare_digest(query.get("k", [""])[0].encode(), self.token.encode()):
                recent = [t for t in self.wrong_codes.get(ip, []) if now - t < WRONG_CODE_WINDOW] + [now]
                self.wrong_codes[ip] = recent
                if len(recent) >= MAX_WRONG_CODES:
                    self.locked_until[ip] = now + LOCKOUT
                    self.wrong_codes.pop(ip, None)
                    log(f"Blocked {ip} for {int(LOCKOUT // 60)} minutes after {MAX_WRONG_CODES} wrong codes.")
                await http_reply(writer, 403, "text/plain", b"Wrong code")
                return
            self.wrong_codes.pop(ip, None)
            await self.phone_session(reader, writer, headers)
        elif method == "GET" and url.path in ("/", "/index.html"):
            with open(os.path.join(HERE, "controller.html"), "rb") as f:
                await http_reply(writer, 200, "text/html; charset=utf-8", f.read())
        else:
            await http_reply(writer, 404, "text/plain", b"Not found")

    async def phone_session(self, reader, writer, headers):
        key = headers.get("sec-websocket-key", "")
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        writer.write(
            ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
        await writer.drain()

        peer = writer.get_extra_info("peername")
        phone = Phone(writer, peer[0] if peer else "?")
        self.phones.append(phone)
        reason = None
        try:
            while True:
                opcode, payload = await ws_recv(reader)
                phone.last_seen = time.monotonic()
                if opcode == 0x8:          # close
                    reason = close_reason(payload)
                    try:
                        await ws_send(writer, payload[:2], opcode=0x8)
                    except (ConnectionError, OSError):
                        pass
                    break
                if opcode == 0x9:          # ping
                    await ws_send(writer, payload, opcode=0xA)
                    continue
                if opcode == 0xA:          # pong (answer to our ping)
                    self.on_pong(phone, payload)
                    continue
                if opcode != 0x1:
                    continue
                text = payload.decode(errors="ignore")
                if text.startswith("hello:"):
                    await self.join(phone, text[6:40] or None)
                    if phone.pad is None:
                        phone.end_reason = "turned away (all players taken)"
                        break
                elif text == "switch":
                    await self.switch(phone)
                elif text.startswith(("vis:", "wake:", "prev:")):
                    self.on_page_note(phone, text)
                elif phone.pad is not None:
                    self.on_input(phone, text)
        except asyncio.IncompleteReadError:
            reason = "the connection ended without a close message"
        except (ConnectionError, OSError) as e:
            reason = f"network error ({describe_error(e)})"
        finally:
            self.phones.remove(phone)
            self.log_disconnect(phone, reason)
            if phone.pad is not None:
                if phone.client_id:
                    self.last_pad[phone.client_id] = phone.pad
                self.trace.state(phone.pad, NEUTRAL)
                self.trace.event(f"P{phone.pad + 1} phone disconnected")
                self.release(phone)
                await self.broadcast_status()
            writer.close()

    def log_disconnect(self, phone, reason):
        """One line on why a phone went away: bridge.log, plus a short one in game.log."""
        now = time.monotonic()
        # The bridge's own reason (timeout, replaced, turned away) wins over
        # what the socket saw when it was closed.
        why = phone.end_reason or reason or "closed"
        if phone.hidden_since is not None:
            why = f"page hidden {duration(now - phone.hidden_since)} before (screen off, app switched or page closed); then {why}"
        ping = f"last ping {phone.rtt:.0f} ms" if phone.rtt is not None else "no ping answered"
        silent = now - phone.last_seen
        details = [f"connected {duration(now - phone.connected_at)}", ping]
        if silent >= 2:
            details.append(f"last message {duration(silent)} before")
        if phone.wake:
            details.append(f"wake lock {phone.wake}")
        log(f"{phone.who()} disconnected: {why}; {', '.join(details)}.")
        self.note(f"{phone.short()} disconnected: {why}; {', '.join(details[:2])}")

    def on_page_note(self, phone, text):
        """What the page reports about itself: hidden/visible, its wake lock,
        and how its previous connection ended (after a reconnect)."""
        kind, _, value = text.partition(":")
        value = "".join(c for c in value[:120] if c.isprintable())
        now = time.monotonic()
        if kind == "vis":
            if value == "hidden" and phone.hidden_since is None:
                phone.hidden_since = now
                log(f"{phone.who()}: page hidden (screen off, locked or another app).")
            elif value == "visible" and phone.hidden_since is not None:
                log(f"{phone.who()}: page visible again after {duration(now - phone.hidden_since)}.")
                phone.hidden_since = None
        elif kind == "wake":
            if value != phone.wake:
                phone.wake = value
                state = {"held": "held (the screen stays on)",
                         "unsupported http": "not supported: browsers offer it only on https pages, "
                                             "and this link is http (the screen can sleep)",
                         "unsupported browser": "not supported by this browser (the screen can sleep)",
                         "released": "released (the screen can sleep)"}.get(value)
                if state is None:
                    state = (f"request refused ({value[7:]}; the screen can sleep)"
                             if value.startswith("failed ") else value)
                log(f"{phone.who()}: wake lock {state}.")
                self.note(f"{phone.short()} wake lock {value}")
        elif kind == "prev":
            log(f"{phone.who()}: the phone saw its previous connection end: {value}.")

    async def join(self, phone, client_id):
        phone.client_id = client_id
        # The same phone reconnecting (its old socket not closed yet) takes
        # over from its old connection rather than becoming a second player.
        for other in list(self.phones):
            if other is not phone and client_id and other.client_id == client_id:
                phone.pad = other.pad
                other.logged_pad = other.pad
                other.pad = None
                other.end_reason = "replaced by a new connection from the same phone"
                await self.tell(other, "replaced")
                other.writer.close()
        if phone.pad is None:
            phone.pad = self.pick_pad(phone)
        if phone.pad is None:
            await self.tell(phone, "full")
            log(f"Phone from {phone.peer} turned away: all four players are taken (phones or controllers).")
            return
        phone.logged_pad = phone.pad
        log(f"Phone from {phone.peer} joined as player {phone.pad + 1}.")
        self.trace.event(f"P{phone.pad + 1} phone connected ({phone.peer})")
        await self.broadcast_status()

    async def switch(self, phone):
        new = self.next_free_pad(phone)
        if new is None or new == phone.pad:
            await self.tell(phone, "nofree")
            return
        old = phone.pad
        self.release(phone, old)
        phone.pad = new
        phone.logged_pad = new
        log(f"Phone from {phone.peer} switched from player {old + 1} to {new + 1}.")
        self.trace.event(f"P{old + 1} phone switched to P{new + 1}")
        await self.broadcast_status()

    def on_input(self, phone, text):
        parts = text.split(",")
        if len(parts) != 3:
            return
        try:
            b = int(parts[0]) & 0x1F7F
            x = max(-127, min(127, int(parts[1])))
            y = max(-127, min(127, int(parts[2])))
        except ValueError:
            return
        phone.last_msg = time.monotonic()
        if (b, x, y) != phone.state or phone.released:
            phone.state = (b, x, y)
            self.trace.state(phone.pad, phone.state)
            self.hold(phone)


CLOSE_CODES = {1000: "normal", 1001: "going away: page closed, reloaded or put away",
               1002: "protocol error", 1005: "no code", 1006: "abnormal: no close message",
               1008: "policy", 1009: "message too big", 1011: "error"}


def close_reason(payload):
    """The phone's close frame as text: code, meaning and its reason, if any."""
    if len(payload) < 2:
        return "the phone closed the connection (no close code)"
    code = struct.unpack(">H", payload[:2])[0]
    text = "".join(c for c in payload[2:].decode(errors="replace")[:80] if c.isprintable())
    meaning = CLOSE_CODES.get(code, "")
    return (f"the phone closed the connection (code {code}" + (f", {meaning}" if meaning else "")
            + (f", \"{text}\"" if text else "") + ")")


def describe_error(e):
    name = type(e).__name__
    return f"{name}: {e.strerror}" if getattr(e, "strerror", None) else (f"{name}: {e}" if str(e) else name)


def duration(seconds):
    s = int(seconds)
    if s < 60:
        return f"{s} s"
    if s < 3600:
        return f"{s // 60} min {s % 60} s"
    return f"{s // 3600} h {s % 3600 // 60} min"


async def http_reply(writer, status, ctype, body):
    reason = {200: "OK", 403: "Forbidden", 404: "Not Found", 429: "Too Many Requests"}[status]
    writer.write(
        (f"HTTP/1.1 {status} {reason}\r\nContent-Type: {ctype}\r\nContent-Length: {len(body)}\r\n"
         "Cache-Control: no-store\r\nConnection: close\r\n\r\n").encode() + body)
    try:
        await writer.drain()
    finally:
        writer.close()


async def ws_recv(reader):
    head = await reader.readexactly(2)
    opcode = head[0] & 0x0F
    masked = head[1] & 0x80
    length = head[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", await reader.readexactly(2))[0]
    elif length == 127:
        length = struct.unpack(">Q", await reader.readexactly(8))[0]
    if length > 4096:
        raise ConnectionError("frame too large")
    mask = await reader.readexactly(4) if masked else b"\0\0\0\0"
    data = await reader.readexactly(length)
    return opcode, bytes(c ^ mask[i % 4] for i, c in enumerate(data))


async def ws_send(writer, data, opcode=0x1):
    if isinstance(data, str):
        data = data.encode()
    header = bytes([0x80 | opcode])
    header += bytes([len(data)]) if len(data) < 126 else bytes([126]) + struct.pack(">H", len(data))
    writer.write(header + data)
    await writer.drain()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--token", required=True)
    ap.add_argument("--http-port", type=int, default=8765)
    ap.add_argument("--game-port", type=int, default=7700)
    ap.add_argument("--trace", help="file to record every phone button change")
    ap.add_argument("--game-pid", type=int, help="exit when this process (the game) exits")
    ap.add_argument("--log-dir", help="where to copy a crash report if the game crashes")
    args = ap.parse_args()

    bridge = Bridge(args.token, args.game_port, InputTrace(args.trace))
    # The previous session's bridge may still be closing; retry for a while.
    server = None
    for attempt in range(15):
        try:
            server = await asyncio.start_server(bridge.handle, "0.0.0.0", args.http_port)
            break
        except OSError as e:
            if attempt == 0:
                log(f"Port {args.http_port} is busy ({e.strerror}); retrying ...")
            await asyncio.sleep(1)
    if server is None:
        log(f"Can't serve the controller page: port {args.http_port} is in use by another program.")
        return
    log(f"Controller page served on port {args.http_port}")
    tasks = [server.serve_forever(), bridge.game_link(), bridge.keepalive(), bridge.pinger()]
    if args.game_pid:
        tasks.append(follow_game(args.game_pid, args.log_dir, server))
    async with server:
        # return_exceptions: follow_game closes the server, which cancels
        # serve_forever; that must not abort the crash-report copy.
        await asyncio.gather(*tasks, return_exceptions=True)


def game_watcher(pid):
    """A function that says whether the game (process pid) is still running."""
    if os.name != "nt":
        def alive():
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return False
            except PermissionError:
                pass
            return True
        return alive
    # On Windows os.kill(pid, 0) would END the game (it calls TerminateProcess),
    # so hold a handle to the process and ask whether it has finished.
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    synchronize, wait_timeout, access_denied = 0x00100000, 0x102, 5
    handle = kernel32.OpenProcess(synchronize, False, pid)
    if not handle:
        if ctypes.get_last_error() == access_denied:
            log(f"Can't watch PartyBoard (process {pid}); PartyBoard stops this helper when it quits.")
            return lambda: True
        return lambda: False   # already gone
    return lambda: kernel32.WaitForSingleObject(handle, 0) == wait_timeout


async def follow_game(pid, log_dir, server):
    """Stop with the game, saving its crash report (if any) next to the logs.
    The port is released first, so a game reopened right away can take it."""
    started = time.time()
    alive = game_watcher(pid)
    while True:
        await asyncio.sleep(1)
        if not alive():
            break
    log("PartyBoard exited.")
    server.close()
    # A Mac crash report; on Windows the game writes its own crash files into
    # the session folder.
    if log_dir and sys.platform == "darwin":
        reports = os.path.expanduser("~/Library/Logs/DiagnosticReports")
        for _ in range(10):   # macOS writes the report a few seconds after the crash
            await asyncio.sleep(1)
            try:
                found = [os.path.join(reports, n) for n in os.listdir(reports)
                         if n.startswith("PartyBoard") and n.endswith(".ips")
                         and os.path.getmtime(os.path.join(reports, n)) >= started]
            except OSError:
                found = []
            if found:
                import shutil
                newest = max(found, key=os.path.getmtime)
                shutil.copy2(newest, log_dir)
                log(f"It crashed. Copied {os.path.basename(newest)} to the logs.")
                break
    os._exit(0)


if __name__ == "__main__":
    if sys.version_info < (3, 7):
        print(f"This helper needs Python 3.7 or later; this is {sys.version.split()[0]}.", flush=True)
        sys.exit(2)
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
