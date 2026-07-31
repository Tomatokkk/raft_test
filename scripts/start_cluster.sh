#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${STRONGKV_BUILD_DIR:-${ROOT_DIR}/build}"
SERVER="${BUILD_DIR}/strongkv-server"
: "${STRONGKV_PASSWORD:?Set STRONGKV_PASSWORD before starting the cluster}"

if [[ ! -x "${SERVER}" ]]; then
  echo "strongkv-server not found: ${SERVER}" >&2
  echo "Set STRONGKV_BUILD_DIR to the CMake build directory." >&2
  exit 1
fi

mkdir -p "${ROOT_DIR}/run" "${ROOT_DIR}/logs"
cd "${ROOT_DIR}"

for node in 1 2 3; do
  pid_file="${ROOT_DIR}/run/node${node}.pid"
  if [[ -f "${pid_file}" ]] && kill -0 "$(cat "${pid_file}")" 2>/dev/null; then
    echo "node${node} already running, pid=$(cat "${pid_file}")"
    continue
  fi
  nohup "${SERVER}" "conf/node${node}.yaml" \
    >"${ROOT_DIR}/run/node${node}.out" 2>&1 </dev/null &
  echo "$!" >"${pid_file}"
  echo "started node${node}, pid=$!"
done

for attempt in $(seq 1 100); do
  ready=0
  for port in 7401 7402 7403; do
    if (echo >/dev/tcp/127.0.0.1/"${port}") 2>/dev/null; then
      ready=$((ready + 1))
    fi
  done
  if [[ "${ready}" -eq 3 ]]; then
    echo "all client ports are ready"
    exit 0
  fi
  sleep 0.1
done

echo "cluster did not become ready; inspect run/node*.out and logs/" >&2
exit 1
