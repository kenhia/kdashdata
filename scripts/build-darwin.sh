#!/usr/bin/env bash
# build-darwin.sh -- wake kimac, build kdash-pub there natively, bring it back.
#
#     scripts/build-darwin.sh <version> <outdir>
#
# Run from the root of a CLEAN checkout whose `just version` is <version>, on
# a host on kimac's LAN segment (kai): a magic packet is a broadcast, and
# Tailscale cannot carry one. On success writes <outdir>/kdash-pub-<arch> --
# `kdash-pub-arm64-darwin` on kimac, the name knarr asks for by the target's
# platform.
#
#   0  built, and the binary's own stamp equals <version>
#   3  kimac could not be woken: NOTHING was built -- an advisory, not a fault
#   1  anything else: kimac answered and the build did not complete
#
# Exit 3 and exit 1 are kept apart on purpose, the way `deploy-all`'s komarchy
# probe keeps asleep and broken apart: a Mac that never answered is its resting
# state, while a Mac that answered and then failed -- including by dropping the
# ssh session mid-build because it went back to sleep -- is a fault to report,
# never a quiet skip.
#
# Why the wake is shaped like this (CD-13, sprint 017; evidence in k-homelab
# WI 3123 and kdashdata WI 3139): a magic packet wakes a deep-idle kimac within
# a second, but into a DARK wake that falls back to sleep ~28 s later. From a
# shallower sleep the packet may wake nothing, and the ssh probe's own traffic
# wakes it instead. Either wake is fine; the packet is best-effort, and the
# hold is what matters (confirmed from forced sleep in sprint 017). In order:
#   1. send the packet, and give it a few seconds alone (PROBE_DELAY)
#   2. ssh with a retry loop and a long ConnectTimeout
#   3. `caffeinate -u -t 2` on the first answer -- declares user activity,
#      which promotes the dark wake to a full one
#   4. the build itself runs under `caffeinate -i -s`, which holds idle AND
#      system sleep off for exactly as long as the build process lives
#
# Everything local (bundle, vendor) is prepared BEFORE the packet goes out, so
# the woken window is spent building rather than waiting on kai.
#
# Overridable: KIMAC_HOST KIMAC_MAC KIMAC_BCAST WAKE_TIMEOUT PROBE_DELAY
set -euo pipefail

version="${1:?usage: build-darwin.sh <version> <outdir>}"
outdir="${2:?usage: build-darwin.sh <version> <outdir>}"
host="${KIMAC_HOST:-kimac}"
mac="${KIMAC_MAC:-3c:a6:f6:66:9c:4e}"        # kimac en0, built-in Ethernet
bcast="${KIMAC_BCAST:-192.168.1.255}"
wake_timeout="${WAKE_TIMEOUT:-180}"

say() { echo "darwin: $*" >&2; }
here="$(cd "$(dirname "$0")" && pwd)"

# -- 0. local preparation, before anything is sent to kimac -----------------
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
sha="$(git rev-parse HEAD)"
mkdir "$work/payload"
git bundle create -q "$work/payload/src.bundle" HEAD
# stdout is the source-replacement config, and stderr is cargo's progress
# chatter. Not `--quiet`: that suppresses the config too, and the build then
# fails offline on the git dependency.
cargo vendor --locked --manifest-path publishers/rust/Cargo.toml \
    "$work/payload/vendor" > "$work/payload/vendor.toml" 2> "$work/vendor.log" \
    || { cat "$work/vendor.log" >&2; exit 1; }
grep -q '^replace-with' "$work/payload/vendor.toml" \
    || { say "cargo vendor printed no source-replacement config"; exit 1; }
tar czf "$work/payload.tgz" -C "$work/payload" .

# -- 1. the magic packet ----------------------------------------------------
# 6 x 0xFF then the MAC 16 times, UDP broadcast to ports 9 and 7, three times.
# Inlined from kai:~/src/kimac-wol-test/wake.py rather than depending on it.
python3 - "$mac" "$bcast" <<'PY'
import socket, sys, time
mac, bcast = sys.argv[1], sys.argv[2]
payload = b"\xff" * 6 + bytes.fromhex(mac.replace(":", "")) * 16
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
for i in range(3):
    for port in (9, 7):
        s.sendto(payload, (bcast, port))
    if i < 2:
        time.sleep(1)
PY
say "sent magic packets for $mac to $bcast at $(date '+%T %Z'); waiting up to ${wake_timeout}s for $host"
# Give the packet the first word. An ssh SYN sent in the same instant also wakes
# a sleeping Mac (pmset logs it as `Enet.Service`), and when the two race, the
# ssh often wins. The Mac still wakes, but the packet has then woken nothing.
# That is not what it's for: it is here for the Mac an ssh cannot reach.
# Measured 2026-09-23 22:17: a probe with no delay logged Enet.Service; the
# WoL harness waited before probing and logged Enet.MagicPacket at +1 s.
sleep "${PROBE_DELAY:-5}"

# -- 2 + 3. ssh until it answers, and the answer IS the full-wake -----------
# A failure that says the host answered but refused us is not absence: stop
# rather than spend the whole timeout reporting "asleep" for a broken key.
ssh_opts=(-o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=15 -o ServerAliveCountMax=4)
t0=$SECONDS; woke=""
while (( SECONDS - t0 < wake_timeout )); do
    if err="$(ssh -n "${ssh_opts[@]}" "$host" 'caffeinate -u -t 2' 2>&1)"; then
        woke=1; break
    fi
    case "$err" in
        *"Host key verification failed"*|*"Permission denied"*)
            say "$host answered but refused the connection: $err"
            exit 1 ;;
    esac
    sleep 3
done
if [[ -z "$woke" ]]; then
    say "$host did not answer within ${wake_timeout}s of the wake -- nothing built"
    exit 3
fi
say "$host is awake after $(( SECONDS - t0 ))s; building under caffeinate -i -s"

# -- 4. copy the payload and build under the hold ---------------------------
rdir="$(ssh -n "${ssh_opts[@]}" "$host" 'mktemp -d -t kdash-pub-build')"
scp -q "${ssh_opts[@]}" "$work/payload.tgz" "$host:$rdir/payload.tgz"
set +e
ssh "${ssh_opts[@]}" "$host" "caffeinate -i -s /bin/bash -s -- '$rdir' '$sha' '$version'" \
    < "$here/build-darwin-remote.sh" > "$work/remote.log" 2>&1
rc=$?
set -e
if (( rc == 255 )); then
    cat "$work/remote.log" >&2
    say "the ssh session to $host dropped mid-build -- the caffeinate hold did not hold"
    exit 1
elif (( rc != 0 )); then
    cat "$work/remote.log" >&2
    say "the build on $host failed (exit $rc)"
    exit 1
fi
built="$(sed -n 's/^BUILT //p' "$work/remote.log")"
grep '^build-darwin-remote:' "$work/remote.log" >&2 || true
if [[ -z "$built" ]]; then
    cat "$work/remote.log" >&2
    say "the build on $host reported no artifact"
    exit 1
fi
mkdir -p "$outdir"
scp -q "${ssh_opts[@]}" "$host:$rdir/$built" "$outdir/$built"
ssh -n "${ssh_opts[@]}" "$host" "rm -rf '$rdir'" || true
say "built $built for $version"
