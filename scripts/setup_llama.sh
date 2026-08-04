#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LLAMA_DIR="${ROOT}/ui/llama.cpp/llama.cpp"
PATCH_FILE="${ROOT}/patches/turboquant/0001-turboquant.patch"
SHA_FILE="${ROOT}/patches/turboquant/UPSTREAM_SHA"
LLAMA_REPO="${LLAMA_CPP_REPO:-https://github.com/ggml-org/llama.cpp.git}"

info() { printf '[llama.cpp] %s\n' "$*"; }
die()  { printf '[llama.cpp] ERROR: %s\n' "$*" >&2; exit 1; }

[[ -s "${SHA_FILE}" ]] || die "Missing upstream pin: ${SHA_FILE}"
[[ -s "${PATCH_FILE}" ]] || die "Missing TurboQuant patch: ${PATCH_FILE}"

UPSTREAM_SHA="$(tr -d '[:space:]' < "${SHA_FILE}")"
[[ "${UPSTREAM_SHA}" =~ ^[0-9a-f]{40}$ ]] ||
  die "Invalid upstream SHA in ${SHA_FILE}: ${UPSTREAM_SHA}"

if [[ ! -d "${LLAMA_DIR}/.git" ]]; then
  [[ ! -e "${LLAMA_DIR}" ]] ||
    die "${LLAMA_DIR} exists but is not a Git checkout"
  info "Initializing ${LLAMA_REPO}"
  mkdir -p "$(dirname "${LLAMA_DIR}")"
  git init --quiet "${LLAMA_DIR}"
  git -C "${LLAMA_DIR}" remote add origin "${LLAMA_REPO}"
fi

CURRENT_SHA="$(git -C "${LLAMA_DIR}" rev-parse HEAD 2>/dev/null || true)"
if [[ "${CURRENT_SHA}" == "${UPSTREAM_SHA}" ]] &&
   git -C "${LLAMA_DIR}" apply --reverse --check "${PATCH_FILE}" 2>/dev/null; then
  info "Ready at ${UPSTREAM_SHA} with TurboQuant applied"
  exit 0
fi

if [[ -n "$(git -C "${LLAMA_DIR}" status --porcelain)" ]]; then
  die "Checkout has local changes that are not the pinned TurboQuant patch"
fi

if [[ "${CURRENT_SHA}" != "${UPSTREAM_SHA}" ]]; then
  info "Fetching pinned upstream commit ${UPSTREAM_SHA}"
  git -C "${LLAMA_DIR}" fetch --quiet --depth 1 origin "${UPSTREAM_SHA}"
  git -C "${LLAMA_DIR}" checkout --quiet --detach FETCH_HEAD
fi

if git -C "${LLAMA_DIR}" apply --check "${PATCH_FILE}"; then
  info "Applying TurboQuant compatibility patch"
  git -C "${LLAMA_DIR}" apply "${PATCH_FILE}"
elif ! git -C "${LLAMA_DIR}" apply --reverse --check "${PATCH_FILE}"; then
  die "TurboQuant patch does not apply cleanly to ${UPSTREAM_SHA}"
fi

info "Ready at ${UPSTREAM_SHA} with TurboQuant applied"
