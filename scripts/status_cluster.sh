#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${STRONGKV_BUILD_DIR:-${ROOT_DIR}/build}"
CLI="${BUILD_DIR}/strongkv-cli"
PASSWORD="${STRONGKV_PASSWORD:?Set STRONGKV_PASSWORD before checking status}"

for node in 1 2 3; do
  pid_file="${ROOT_DIR}/run/node${node}.pid"
  if [[ -f "${pid_file}" ]] && kill -0 "$(cat "${pid_file}")" 2>/dev/null; then
    echo "node${node}: running pid=$(cat "${pid_file}")"
    if [[ -x "${CLI}" ]]; then
      "${CLI}" -h 127.0.0.1 -p "740${node}" \
        -a "${PASSWORD}" ROLE || true
    fi
  else
    echo "node${node}: stopped"
  fi
done
