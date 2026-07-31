#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${STRONGKV_BUILD_DIR:-${ROOT_DIR}/build}"
CLI="${BUILD_DIR}/strongkv-cli"
PASSWORD="${STRONGKV_PASSWORD:?Set STRONGKV_PASSWORD before running tests}"
SEEDS=(
  --seed 127.0.0.1:7401
  --seed 127.0.0.1:7402
  --seed 127.0.0.1:7403
)

"${CLI}" "${SEEDS[@]}" -a "${PASSWORD}" SET failover-key 1

leader=0
for node in 1 2 3; do
  info="$("${CLI}" -h 127.0.0.1 -p "740${node}" \
    -a "${PASSWORD}" INFO)"
  if grep -q '^role:leader' <<<"${info}"; then
    leader="${node}"
  fi
done
if [[ "${leader}" -eq 0 ]]; then
  echo "no leader found" >&2
  exit 1
fi

pid_file="${ROOT_DIR}/run/node${leader}.pid"
pid="$(cat "${pid_file}")"
echo "crashing leader node${leader}, pid=${pid}"
kill -KILL "${pid}"
rm -f "${pid_file}"

for attempt in $(seq 1 100); do
  if value="$("${CLI}" "${SEEDS[@]}" -a "${PASSWORD}" \
      GET failover-key 2>/dev/null)" && [[ "${value}" == "1" ]]; then
    result="$("${CLI}" "${SEEDS[@]}" -a "${PASSWORD}" \
      INCR failover-key)"
    [[ "${result}" == "(integer) 2" ]] || {
      echo "unexpected INCR result: ${result}" >&2
      exit 1
    }
    echo "failover passed; new leader retained data and committed INCR"
    exit 0
  fi
  sleep 0.1
done

echo "new leader did not become available" >&2
exit 1
