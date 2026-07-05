#!/usr/bin/env bash

set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
	echo "error: run this inside the dev container as root." >&2
	exit 1
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_DIR="$(cd -- "${PROJECT_DIR}/../.." && pwd)"
FONT_DIR="${WORKSPACE_DIR}/tools/fonts"
FONT_PATH="${FONT_DIR}/MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf"
FONT_URL="https://github.com/google/material-design-icons/raw/master/variablefont/MaterialSymbolsRounded%5BFILL%2CGRAD%2Copsz%2Cwght%5D.ttf"

if ! command -v node >/dev/null 2>&1 || ! command -v npm >/dev/null 2>&1; then
	apt-get update
	apt-get install -y --no-install-recommends nodejs npm
	apt-get clean
	rm -rf /var/lib/apt/lists/*
else
	echo "node/npm already installed"
fi

if ! command -v lv_font_conv >/dev/null 2>&1; then
	npm install -g lv_font_conv
else
	echo "lv_font_conv already installed at $(command -v lv_font_conv)"
fi

mkdir -p "${FONT_DIR}"

if [[ ! -f "${FONT_PATH}" ]]; then
	curl -L "${FONT_URL}" -o "${FONT_PATH}"
else
	echo "font already present at ${FONT_PATH}"
fi

echo "ready:"
echo "  node=$(command -v node)"
echo "  npm=$(command -v npm)"
echo "  lv_font_conv=$(command -v lv_font_conv)"
echo "  font=${FONT_PATH}"