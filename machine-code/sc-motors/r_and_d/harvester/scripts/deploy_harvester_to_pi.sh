#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HARVESTER_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DEPLOY_ITEMS=(
  "runtime"
  "shared"
  "third_party"
)

require_command() {
  local command_name="$1"
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    echo "Missing required command: ${command_name}" >&2
    exit 1
  fi
}

prompt_value() {
  local prompt_text="$1"
  local result_var="$2"
  local secret="${3:-false}"
  local value=""

  while [[ -z "${value}" ]]; do
    if [[ "${secret}" == "true" ]]; then
      read -r -s -p "${prompt_text}: " value
      echo
    else
      read -r -p "${prompt_text}: " value
    fi
  done

  printf -v "${result_var}" '%s' "${value}"
}

require_command sshpass
require_command ssh
require_command scp

if [[ ! -d "${HARVESTER_ROOT}" ]]; then
  echo "Harvester source root not found: ${HARVESTER_ROOT}" >&2
  exit 1
fi

prompt_value "Pi host or IP" PI_HOST
prompt_value "Pi username" PI_USER
prompt_value "Pi password" PI_PASSWORD true
prompt_value "Remote target directory" REMOTE_DIR

echo "Deploying harvester sources to ${PI_USER}@${PI_HOST}:${REMOTE_DIR}"

sshpass -p "${PI_PASSWORD}" ssh \
  -o StrictHostKeyChecking=no \
  "${PI_USER}@${PI_HOST}" \
  "mkdir -p '${REMOTE_DIR}'"

for item in "${DEPLOY_ITEMS[@]}"; do
  local_path="${HARVESTER_ROOT}/${item}"

  if [[ ! -e "${local_path}" ]]; then
    echo "Skipping missing path: ${local_path}"
    continue
  fi

  echo "Copying ${item}/"
  sshpass -p "${PI_PASSWORD}" scp \
    -r \
    -o StrictHostKeyChecking=no \
    "${local_path}" \
    "${PI_USER}@${PI_HOST}:${REMOTE_DIR}/"
done

echo "Deploy complete."
