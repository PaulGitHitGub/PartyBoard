#!/bin/bash
# Installed as PartyBoard.app/Contents/MacOS/PartyBoard (the real game is
# PartyBoard-bin beside it). Starts the phone-controller bridge, turns on the
# game's input socket and diagnostic logging, then becomes the game with exec,
# so the Dock and Activity Monitor see one PartyBoard process as before.

HERE="$(cd "$(dirname "$0")" && pwd)"
GAME="$HERE/PartyBoard-bin"
CONTROLLER="$HERE/../Resources/PhoneController"
SUPPORT="$HOME/Library/Application Support/MarioPartyRD/Party Board"
LOGROOT="$HOME/Library/Logs/PartyBoard"
LOGDIR="$LOGROOT/$(date +%Y-%m-%d_%H-%M-%S)"
GAME_PORT=7700
HTTP_PORT=48765   # uncommon, to stay clear of local dev servers (8765 is a popular one)

mkdir -p "$LOGDIR" "$SUPPORT"
# Keep the newest 20 sessions.
ls -1d "$LOGROOT"/*/ 2>/dev/null | sort -r | tail -n +21 | while read -r old; do rm -rf "$old"; done

export PARTYBOARD_INPUT_LOG=1

# The phone controller is off unless turned on in Settings > Input > Phone
# Controller (which creates this file): it opens a port on the Wi-Fi network,
# and most players never use it.
PHONE_ON="$SUPPORT/phone-controller-enabled"
# /usr/bin/python3 is only a stub on a Mac without Apple's command line tools,
# and running it pops up an installer every launch. Ask first, quietly.
HAVE_PYTHON=0
/usr/bin/xcode-select -p >/dev/null 2>&1 && [ -x /usr/bin/python3 ] && HAVE_PYTHON=1
if [ -f "$PHONE_ON" ] && [ "$HAVE_PYTHON" = 0 ]; then
  export PARTYBOARD_PHONE_PROBLEM=python
fi

if [ -f "$PHONE_ON" ] && [ "$HAVE_PYTHON" = 1 ] && [ -f "$CONTROLLER/bridge.py" ]; then
  # A fixed code, so a Home Screen bookmark keeps working. New codes are 10
  # characters; an existing shorter one is kept so saved bookmarks still work
  # (the helper blocks a device after a few wrong codes either way).
  CODEFILE="$SUPPORT/phone-controller-code"
  [ -s "$CODEFILE" ] || LC_ALL=C tr -dc 'a-z0-9' </dev/urandom | head -c 10 > "$CODEFILE"
  chmod 600 "$CODEFILE" 2>/dev/null
  TOKEN=$(cat "$CODEFILE")

  # $$ stays this process's PID through the exec below, so the bridge can
  # follow the game and stop when it quits.
  /usr/bin/python3 -u "$CONTROLLER/bridge.py" --token "$TOKEN" --http-port "$HTTP_PORT" \
    --game-port "$GAME_PORT" --trace "$LOGDIR/input.log" --game-pid $$ --log-dir "$LOGDIR" \
    > "$LOGDIR/bridge.log" 2>&1 &
  export PARTYBOARD_TEST_INPUT=$GAME_PORT
  # For Settings > Input > Phone Controller, which builds the link and QR code.
  export PARTYBOARD_PHONE_CODE="$TOKEN" PARTYBOARD_PHONE_PORT="$HTTP_PORT"
  export PARTYBOARD_PHONE_HOST="$(scutil --get LocalHostName 2>/dev/null)"

  HOSTURL="http://$(scutil --get LocalHostName 2>/dev/null).local:$HTTP_PORT/?k=$TOKEN"
  IP=$(ipconfig getifaddr en0 || ipconfig getifaddr en1)
  {
    echo "Open one of these in Safari on your iPhone (same Wi-Fi):"
    echo
    [ -n "$IP" ] && echo "  http://$IP:$HTTP_PORT/?k=$TOKEN"
    echo "  $HOSTURL"
    echo
    echo "Tip: Share > Add to Home Screen for a full-screen pad."
  } > "$LOGROOT/Phone Controller Link.txt"
  if [ -n "$IP" ]; then
    /usr/bin/osascript -e "display notification \"http://$IP:$HTTP_PORT/?k=$TOKEN\" with title \"PartyBoard phone controller\" subtitle \"Open on your iPhone (same Wi-Fi)\"" >/dev/null 2>&1 &
  fi
fi

# Millisecond timestamps on every game line, to line it up with input.log.
exec > >(/usr/bin/perl -MTime::HiRes=time -MPOSIX=strftime -ne 'BEGIN{$|=1} my $t=time; printf "%s.%03d %s", strftime("%H:%M:%S",localtime $t), ($t-int $t)*1000, $_' > "$LOGDIR/game.log") 2>&1
exec "$GAME" "$@"
