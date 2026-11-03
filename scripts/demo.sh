#!/usr/bin/env bash
# End-to-end demo: one server, two clients, one shared space.
# Usage: scripts/demo.sh [build-dir]   (default: build/dev)
set -euo pipefail

BUILD_DIR="${1:-build/dev}"
SYNCBOXD="${BUILD_DIR}/syncboxd"
SYNCBOX="${BUILD_DIR}/syncbox"

if [[ ! -x "${SYNCBOXD}" || ! -x "${SYNCBOX}" ]]; then
  echo "build the project first: cmake --build --preset dev" >&2
  exit 1
fi

TMP="$(mktemp -d)"
PORT="$((20000 + RANDOM % 20000))"
SERVER_PID=""

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  rm -rf "${TMP}"
}
trap cleanup EXIT

DB="${TMP}/server.db"
BLOBS="${TMP}/blobs"
mkdir -p "${TMP}/alice" "${TMP}/bob"

SPACE="$("${SYNCBOXD}" admin create-space --db "${DB}" --name Demo)"
ENROLL_A="$("${SYNCBOXD}" admin enroll --db "${DB}" --space "${SPACE}")"
ENROLL_B="$("${SYNCBOXD}" admin enroll --db "${DB}" --space "${SPACE}")"

"${SYNCBOXD}" --db "${DB}" --blob-root "${BLOBS}" \
  --listen 127.0.0.1 --port "${PORT}" &
SERVER_PID=$!

for _ in $(seq 1 50); do
  if curl -fsS "http://127.0.0.1:${PORT}/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

"${SYNCBOX}" enroll --server "http://127.0.0.1:${PORT}" --token "${ENROLL_A}" \
  --name alice --root "${TMP}/alice" --config "${TMP}/alice.json" >/dev/null
"${SYNCBOX}" enroll --server "http://127.0.0.1:${PORT}" --token "${ENROLL_B}" \
  --name bob --root "${TMP}/bob" --config "${TMP}/bob.json" >/dev/null

sync_client() {
  "${SYNCBOX}" sync --config "$1" >/dev/null
}

echo "creating alice/hello.txt"
echo "hello from alice" > "${TMP}/alice/hello.txt"
sync_client "${TMP}/alice.json"
sync_client "${TMP}/bob.json"
grep -q "hello from alice" "${TMP}/bob/hello.txt"

echo "editing on bob"
echo "edited by bob" > "${TMP}/bob/hello.txt"
sync_client "${TMP}/bob.json"
sync_client "${TMP}/alice.json"
grep -q "edited by bob" "${TMP}/alice/hello.txt"

echo "conflict: both edit from the same base"
echo "bob version" > "${TMP}/bob/hello.txt"
echo "alice version" > "${TMP}/alice/hello.txt"
sync_client "${TMP}/bob.json"
sync_client "${TMP}/alice.json"
sync_client "${TMP}/alice.json"
sync_client "${TMP}/bob.json"

CONFLICTS="$("${SYNCBOX}" conflicts --config "${TMP}/alice.json")"
if [[ -z "${CONFLICTS}" ]]; then
  echo "expected at least one conflict copy" >&2
  exit 1
fi

echo
echo "demo OK"
echo "  space: ${SPACE}"
echo "  alice: $(ls "${TMP}/alice")"
echo "  bob:   $(ls "${TMP}/bob")"
