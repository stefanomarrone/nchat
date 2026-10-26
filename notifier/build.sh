#!/usr/bin/env bash

# build.sh - build and install nchat-notify, see doc/NOTIFIER.md
#
# Usage: notifier/build.sh [--no-telegram] [--no-whatsapp] [--with-signal]
#                          [--no-install] [--jobs N]
#
# Env:
#   NCHAT_PREFIX - install prefix (default: /usr/local). A user-writable prefix
#                  installs without sudo/doas.

exiterr()
{
  >&2 echo "${1}"
  exit 1
}

show_usage()
{
  echo "usage: notifier/build.sh [OPTIONS]"
  echo ""
  echo "Options:"
  echo "  --no-telegram   - build without telegram support"
  echo "  --no-whatsapp   - build without whatsapp support"
  echo "  --with-signal   - build with signal support"
  echo "  --no-install    - build only, do not install"
  echo "  --jobs,-j N     - parallel build jobs (default: 4)"
  echo "  --help,-h       - show this help"
  echo ""
}

# target_writable - true if the dir, or its nearest existing ancestor, is
# writable by the current user
target_writable()
{
  local d="${1}"
  while [[ ! -e "${d}" ]]; do d="$(dirname "${d}")"; done
  [[ -w "${d}" ]]
}

# process arguments
TELEGRAM="ON"
WHATSAPP="ON"
SIGNAL="OFF"
INSTALL="1"
JOBS="4"
while [[ ${#} -gt 0 ]]; do
  case "${1}" in
    --no-telegram) TELEGRAM="OFF" ;;
    --no-whatsapp) WHATSAPP="OFF" ;;
    --with-signal) SIGNAL="ON" ;;
    --no-install) INSTALL="0" ;;
    --jobs|-j) shift; JOBS="${1}" ;;
    --help|-h) show_usage; exit 0 ;;
    *) show_usage; exiterr "unsupported argument \"${1}\", exiting." ;;
  esac
  shift
done
[[ "${JOBS}" =~ ^[1-9][0-9]*$ ]] || exiterr "invalid jobs \"${JOBS}\", exiting."

# run from the repo root
cd "$(dirname "${0}")/.." || exiterr "repo root not found, exiting."

# checks
command -v cmake > /dev/null || exiterr "cmake not found, exiting."
command -v make > /dev/null || exiterr "make not found, exiting."
command -v curl > /dev/null || >&2 echo "warning: curl not found, it is needed at runtime to publish to ntfy."

# targets: the daemon and the enabled protocol libraries
TARGETS="nchat-notify"
[[ "${TELEGRAM}" == "ON" ]] && TARGETS="${TARGETS} tgchat"
[[ "${WHATSAPP}" == "ON" ]] && TARGETS="${TARGETS} wmchat"
[[ "${SIGNAL}" == "ON" ]] && TARGETS="${TARGETS} sgchat"

CMAKEARGS="-DCMAKE_BUILD_TYPE=Release -DHAS_TESTS=OFF -DHAS_TELEGRAM=${TELEGRAM} -DHAS_WHATSAPP=${WHATSAPP} -DHAS_SIGNAL=${SIGNAL}"
if [[ -n "${NCHAT_PREFIX:-}" ]]; then
  CMAKEARGS="${CMAKEARGS} -DCMAKE_INSTALL_PREFIX=${NCHAT_PREFIX%/}"
fi

# build
echo "-- Using cmake ${CMAKEARGS}"
echo "-- Building ${TARGETS} with -j${JOBS}"
mkdir -p build && cd build || exiterr "build dir failed, exiting."
cmake ${CMAKEARGS} .. || exiterr "cmake failed, exiting."
make -j"${JOBS}" ${TARGETS} || exiterr "build failed, exiting."

# install
if [[ "${INSTALL}" == "1" ]]; then
  PREFIX="$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' CMakeCache.txt)"
  if target_writable "${PREFIX:-/usr/local}"; then
    make -j"${JOBS}" install || exiterr "install failed, exiting."
  elif command -v sudo > /dev/null; then
    sudo make -j"${JOBS}" install || exiterr "install failed, exiting."
  elif command -v doas > /dev/null; then
    doas make -j"${JOBS}" install || exiterr "install failed, exiting."
  else
    exiterr "install prefix ${PREFIX} not writable and no sudo/doas, exiting."
  fi
  echo "-- Installed in ${PREFIX}"
fi

echo "-- Done"
