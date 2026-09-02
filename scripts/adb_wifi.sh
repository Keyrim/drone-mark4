#!/usr/bin/env bash
# Wireless adb connection to the development phone, interactive:
#   1. discovers through mDNS the phones whose wireless debugging is on,
#   2. offers the numbered list (plus pairing a new phone),
#   3. connects the chosen one.
# The devcontainer runs with host networking, so the phone's mDNS
# announcements reach it: no ip:port to copy from the phone screen.
# Pairing (the "pair device with a code" popup on the phone) is needed once
# per container; the debug port changes at every toggle of wireless
# debugging or reboot, the pairing survives.
set -euo pipefail

# adb's built-in mDNS backend (no Bonjour / avahi daemon needed).
export ADB_MDNS_OPENSCREEN=1

CONNECT_SVC=_adb-tls-connect._tcp
PAIRING_SVC=_adb-tls-pairing._tcp
DISCOVERY_TIMEOUT=6 # s, the time the mDNS announcements take to show up

# Starts the adb server if needed, with the right mDNS backend. A server
# started without ADB_MDNS_OPENSCREEN discovers nothing: restart it.
ensure_server() {
    if ! adb mdns check >/dev/null 2>&1; then
        adb kill-server >/dev/null 2>&1 || true
        adb start-server >/dev/null
    fi
}

# Services of one type, one "name<TAB>ip:port" line per phone.
services_of_type() {
    adb mdns services 2>/dev/null | awk -v type="$1" '$2 == type { print $1 "\t" $3 }' | sort -u
}

# Waits for at least one service of the type (or times out).
discover() {
    local type=$1 found=""
    for _ in $(seq "$DISCOVERY_TIMEOUT"); do
        found=$(services_of_type "$type")
        [ -n "$found" ] && break
        sleep 1
    done
    printf '%s' "$found"
}

is_connected() {
    adb devices | awk -v addr="$1" '$1 == addr && $2 == "device" { found = 1 } END { exit !found }'
}

# Pairing: the pairing service is announced ONLY while the "pair device
# with a code" popup is open on the phone.
pair_device() {
    echo
    echo "On the phone: Developer options -> Wireless debugging ->"
    echo "  \"Pair device with pairing code\" (keep the popup open)."
    echo
    echo "Looking for phones waiting to pair..."

    local pairing addr
    pairing=$(discover "$PAIRING_SVC")
    if [ -n "$pairing" ]; then
        addr=$(printf '%s' "$pairing" | head -n1 | cut -f2)
        echo "Found: $(printf '%s' "$pairing" | head -n1 | cut -f1) ($addr)"
    else
        echo "No pairing announcement received."
        read -r -p "Pairing address shown on the phone (ip:port): " addr
        [ -n "$addr" ] || {
            echo "Aborted." >&2
            return 1
        }
    fi

    local code
    read -r -p "6-digit pairing code: " code
    adb pair "$addr" "$code"
}

connect_to() {
    local name=$1 addr=$2
    echo
    echo "Connecting to $name ($addr)..."
    # adb connect exits 0 even on failure: check adb devices instead.
    adb connect "$addr" || true

    if ! is_connected "$addr"; then
        echo
        echo "The phone is announced but refuses the connection: it is"
        echo "probably not (or no longer) paired with this container."
        local answer
        read -r -p "Pair it now? [Y/n] " answer
        case "$answer" in
        [nN]*) return 1 ;;
        esac
        pair_device || return 1
        adb connect "$addr" || true
        is_connected "$addr" || {
            echo "Still not connected: the port may have changed, run the script again." >&2
            return 1
        }
    fi

    echo
    adb devices -l
    echo
    echo "Run the app:  cd software/mobile && flutter run -d $addr"
}

main() {
    ensure_server

    echo "Looking for phones (wireless debugging on)..."
    local devices
    devices=$(discover "$CONNECT_SVC")

    if [ -z "$devices" ]; then
        echo
        echo "No phone found. Check that:"
        echo "  - wireless debugging is on in the phone's developer options,"
        echo "  - phone and PC are on the same Wi-Fi,"
        echo "  - the phone was paired with this container once (otherwise answer Y below)."
        echo
        read -r -p "Pair a phone now? [Y/n] " answer
        case "$answer" in
        [nN]*) exit 1 ;;
        esac
        pair_device || exit 1
        echo
        echo "Paired, looking for the phone..."
        devices=$(discover "$CONNECT_SVC")
        [ -n "$devices" ] || {
            echo "Still nothing: run the script again." >&2
            exit 1
        }
    fi

    local -a names=() addrs=()
    while IFS=$'\t' read -r name addr; do
        names+=("$name")
        addrs+=("$addr")
    done <<<"$devices"

    echo
    local i
    for i in "${!names[@]}"; do
        echo "  $((i + 1))) ${names[$i]}  ${addrs[$i]}"
    done
    echo "  p) pair a new phone"
    echo
    local choice
    read -r -p "Choice [1]: " choice
    choice=${choice:-1}

    case "$choice" in
    [pP])
        pair_device || exit 1
        exec "$0"
        ;;
    esac
    [[ "$choice" =~ ^[0-9]+$ ]] && [ "$choice" -ge 1 ] && [ "$choice" -le "${#names[@]}" ] || {
        echo "Invalid choice." >&2
        exit 1
    }
    connect_to "${names[$((choice - 1))]}" "${addrs[$((choice - 1))]}"
}

main "$@"
