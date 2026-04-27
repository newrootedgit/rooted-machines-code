#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SEEDER_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DEPLOY_ITEMS=(
  "runtime"
  "shared"
  "third_party"
)

DEFAULT_PI_HOST="${PI_HOST:-rootedpi}"
DEFAULT_PI_USER="${PI_USER:-rooted}"
DEFAULT_PI_PASSWORD="${PI_PASSWORD:-}"
DEFAULT_REMOTE_DIR="${REMOTE_DIR:-/home/rooted/}"

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
  local default_value="${3:-}"
  local secret="${4:-false}"
  local value=""

  while [[ -z "${value}" ]]; do
    if [[ "${secret}" == "true" ]]; then
      if [[ -n "${default_value}" ]]; then
        read -r -s -p "${prompt_text} [saved]: " value
      else
        read -r -s -p "${prompt_text}: " value
      fi
      echo
    else
      if [[ -n "${default_value}" ]]; then
        read -r -p "${prompt_text} [${default_value}]: " value
      else
        read -r -p "${prompt_text}: " value
      fi
    fi

    if [[ -z "${value}" && -n "${default_value}" ]]; then
      value="${default_value}"
    fi
  done

  printf -v "${result_var}" '%s' "${value}"
}

require_command sshpass
require_command ssh
require_command scp

if [[ ! -d "${SEEDER_ROOT}" ]]; then
  echo "Seeder source root not found: ${SEEDER_ROOT}" >&2
  exit 1
fi

prompt_value "Pi host or IP" PI_HOST "${DEFAULT_PI_HOST}"
prompt_value "Pi username" PI_USER "${DEFAULT_PI_USER}"
prompt_value "Pi password" PI_PASSWORD "${DEFAULT_PI_PASSWORD}" true
prompt_value "Remote target directory" REMOTE_DIR "${DEFAULT_REMOTE_DIR}"

echo "Deploying seeder sources to ${PI_USER}@${PI_HOST}:${REMOTE_DIR}"

sshpass -p "${PI_PASSWORD}" ssh \
  -o StrictHostKeyChecking=no \
  "${PI_USER}@${PI_HOST}" \
  "mkdir -p '${REMOTE_DIR}'"

for item in "${DEPLOY_ITEMS[@]}"; do
  local_path="${SEEDER_ROOT}/${item}"

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
