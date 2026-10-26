#!/usr/bin/env bash

# install-service.sh - install the nchat-notify systemd service, see
# doc/NOTIFIER.md
#
# Usage: notifier/install-service.sh [--no-start]
#
# Env:
#   NCHAT_PREFIX - prefix nchat-notify was installed in (default: /usr/local)

exiterr()
{
  >&2 echo "${1}"
  exit 1
}

show_usage()
{
  echo "usage: notifier/install-service.sh [OPTIONS]"
  echo ""
  echo "Options:"
  echo "  --no-start      - enable the service without starting it"
  echo "  --help,-h       - show this help"
  echo ""
}

# process arguments
START="1"
while [[ ${#} -gt 0 ]]; do
  case "${1}" in
    --no-start) START="0" ;;
    --help|-h) show_usage; exit 0 ;;
    *) show_usage; exiterr "unsupported argument \"${1}\", exiting." ;;
  esac
  shift
done

SVCUSER="nchat"
CONFDIR="/var/lib/nchat-notify"
UNITFILE="/etc/systemd/system/nchat-notify.service"
PREFIX="${NCHAT_PREFIX:-/usr/local}"
BINARY="${PREFIX%/}/bin/nchat-notify"
SRCUNIT="$(cd "$(dirname "${0}")" && pwd)/nchat-notify.service"

# re-run as root, keeping the prefix
if [[ "${EUID}" -ne 0 ]]; then
  command -v sudo > /dev/null || exiterr "must be run as root, exiting."
  exec sudo NCHAT_PREFIX="${PREFIX}" "${0}" "${@}"
fi

# checks
command -v systemctl > /dev/null || exiterr "systemctl not found, exiting."
[[ -f "${SRCUNIT}" ]] || exiterr "unit ${SRCUNIT} not found, exiting."
[[ -x "${BINARY}" ]] || exiterr "${BINARY} not found, run notifier/build.sh first or set NCHAT_PREFIX, exiting."
command -v curl > /dev/null || >&2 echo "warning: curl not found, it is needed to publish to ntfy."

# service user
if id "${SVCUSER}" > /dev/null 2>&1; then
  echo "-- User ${SVCUSER} exists"
else
  echo "-- Creating user ${SVCUSER}"
  useradd --system --home-dir "${CONFDIR}" --shell /usr/sbin/nologin "${SVCUSER}" || exiterr "useradd failed, exiting."
fi

# confdir, also needed by the setup run before the service is started
install -d -m 0700 -o "${SVCUSER}" -g "${SVCUSER}" "${CONFDIR}" || exiterr "confdir ${CONFDIR} failed, exiting."

# unit, with ExecStart pointing to the installed binary
echo "-- Installing ${UNITFILE}"
sed "s|^ExecStart=.*|ExecStart=${BINARY} --confdir ${CONFDIR}|" "${SRCUNIT}" > "${UNITFILE}" || exiterr "unit install failed, exiting."
chmod 0644 "${UNITFILE}"
systemctl daemon-reload || exiterr "daemon-reload failed, exiting."
systemctl enable nchat-notify || exiterr "enable failed, exiting."

# warn about missing setup / configuration
if ! ls "${CONFDIR}"/profiles/* > /dev/null 2>&1; then
  echo "-- No account set up yet, run (with the service stopped):"
  echo "     sudo -u ${SVCUSER} ${BINARY} --confdir ${CONFDIR} --setup"
fi
if ! grep -qE '^ntfy_topic=.+' "${CONFDIR}/notifier.conf" 2>/dev/null; then
  echo "-- ntfy_topic not set yet in ${CONFDIR}/notifier.conf"
fi

if [[ "${START}" == "1" ]]; then
  echo "-- (Re)starting nchat-notify"
  systemctl restart nchat-notify || exiterr "start failed, see journalctl -u nchat-notify, exiting."
  systemctl --no-pager status nchat-notify
fi

echo "-- Done, logs: journalctl -u nchat-notify -f"
