#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${STRONGKV_BUILD_DIR:-${ROOT_DIR}/build}"
CLI="${BUILD_DIR}/strongkv-cli"
LOAD="${BUILD_DIR}/strongkv-concurrent-incr"
PASSWORD="${STRONGKV_PASSWORD:?Set STRONGKV_PASSWORD before running tests}"
THREADS="${STRONGKV_INCR_THREADS:-10}"
INCREMENTS="${STRONGKV_INCREMENTS_PER_THREAD:-1000}"
SEEDS=(
  --seed 127.0.0.1:7401
  --seed 127.0.0.1:7402
  --seed 127.0.0.1:7403
)

"${CLI}" "${SEEDS[@]}" -a "${PASSWORD}" SET integration-key value
[[ "$("${CLI}" "${SEEDS[@]}" -a "${PASSWORD}" GET integration-key)" == \
   "value" ]]
[[ "$("${CLI}" "${SEEDS[@]}" -a "${PASSWORD}" DEL integration-key)" == \
   "(integer) 1" ]]
[[ "$("${CLI}" "${SEEDS[@]}" -a "${PASSWORD}" GET integration-key)" == \
   "(nil)" ]]

"${CLI}" "${SEEDS[@]}" -a "${PASSWORD}" SET integration-counter 10
[[ "$("${CLI}" "${SEEDS[@]}" -a "${PASSWORD}" INCR integration-counter)" == \
   "(integer) 11" ]]
[[ "$("${CLI}" "${SEEDS[@]}" -a "${PASSWORD}" DECR integration-counter)" == \
   "(integer) 10" ]]

"${LOAD}" "${PASSWORD}" "${THREADS}" "${INCREMENTS}" \
  127.0.0.1:7401 127.0.0.1:7402 127.0.0.1:7403
echo "basic and concurrent integration tests passed"
