#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

for node in 1 2 3; do
  pid_file="${ROOT_DIR}/run/node${node}.pid"
  [[ -f "${pid_file}" ]] || continue
  pid="$(cat "${pid_file}")"
  if kill -0 "${pid}" 2>/dev/null; then
    kill -TERM "${pid}"
    echo "stopping node${node}, pid=${pid}"
  fi
done

for attempt in $(seq 1 100); do
  remaining=0
  for node in 1 2 3; do
    pid_file="${ROOT_DIR}/run/node${node}.pid"
    if [[ -f "${pid_file}" ]] &&
       kill -0 "$(cat "${pid_file}")" 2>/dev/null; then
      remaining=$((remaining + 1))
    fi
  done
  [[ "${remaining}" -eq 0 ]] && break
  sleep 0.1
done

for node in 1 2 3; do
  pid_file="${ROOT_DIR}/run/node${node}.pid"
  [[ -f "${pid_file}" ]] || continue
  pid="$(cat "${pid_file}")"
  if kill -0 "${pid}" 2>/dev/null; then
    echo "node${node} did not stop in 10 seconds; sending SIGKILL" >&2
    kill -KILL "${pid}"
  fi
  rm -f "${pid_file}"
done
