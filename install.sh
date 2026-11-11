#!/usr/bin/env bash
# Install a locally built bin/skbd, the systemd user unit, and the man page.
# No download. The unit has no WatchdogSec. It sets NoNewPrivileges,
# PrivateTmp, RestrictAddressFamilies=AF_UNIX, and ProtectSystem=strict.
#
# The binary lands in ~/.local/bin/skbd. The unit is
# $XDG_CONFIG_HOME/systemd/user/skbd.service when that variable is an
# absolute path, or ~/.config/systemd/user/skbd.service when it is
# unset, empty, or not absolute. The unit directory must stay inside the
# passwd home.
# The man page is ~/.local/share/man/man1/skbd.1. The service is not enabled:
# SWAYSOCK contains Sway's pid, so start it only after Sway is up.
#
# Usage: install.sh [--self-check]
# Exit: 0 installed or self-check passed, 1 failure, 2 bad arguments.

set -Eeuo pipefail
shopt -s inherit_errexit
IFS=$'\n\t'

readonly script_name="install.sh"
passwd_home=""

usage() {
  cat <<'EOF'
Usage: install.sh [--self-check]

Install the locally built bin/skbd, the user unit, and the man page:

  ~/.local/bin/skbd
  ${XDG_CONFIG_HOME:-~/.config}/systemd/user/skbd.service
  ~/.local/share/man/man1/skbd.1

Run make first. Nothing is downloaded. The unit is not enabled. After Sway
is running:

  systemctl --user import-environment SWAYSOCK
  systemctl --user restart skbd.service

--self-check checks the unit text and the unit path rules. It does not
install, and it needs no network and no root.

Exit codes: 0 installed or self-check passed, 1 failure, 2 bad arguments.
EOF
}

die() {
  printf '%s: %s\n' "$script_name" "$1" >&2
  exit 1
}

source_root() {
  cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd
}

# HOME must be the passwd home for this uid. A borrowed HOME would install
# the unit and the binary into someone else's files.
require_own_home() {
  local entry
  entry=$(getent passwd "$EUID") || die "no passwd entry for uid ${EUID}"
  IFS=: read -r _ _ _ _ _ passwd_home _ <<<"$entry"
  if [[ -z "${passwd_home}" || "${HOME}" != "${passwd_home}" ]]; then
    die "HOME is not the home directory for uid ${EUID}"
  fi
  passwd_home=${passwd_home%/}
}

# config_home_for XDG HOME
# An unset, empty, or relative XDG_CONFIG_HOME is HOME/.config.
config_home_for() {
  local xdg=$1
  local home=$2
  if [[ -z "$xdg" || "$xdg" != /* ]]; then
    printf '%s\n' "${home}/.config"
    return 0
  fi
  printf '%s\n' "$xdg"
}

# canonical_unit_dir PASSWD_HOME CONFIG_HOME
# Refuses a config home outside PASSWD_HOME before creating anything.
# After mkdir, canonicalizes again and refuses a path that escaped.
canonical_unit_dir() {
  local passwd_home_arg=$1
  local config_home=$2
  local unitdir canon home_canon config_canon
  if [[ "$passwd_home_arg" != "/" ]]; then
    passwd_home_arg=${passwd_home_arg%/}
  fi
  home_canon=$(realpath -- "$passwd_home_arg") || return 1
  if [[ -d "$config_home" ]]; then
    config_canon=$(realpath -- "$config_home") || return 1
  else
    config_canon=$(realpath -m -- "$config_home") || return 1
  fi
  case "$config_canon" in
    "${home_canon}" | "${home_canon}"/*)
      ;;
    *)
      printf 'unit directory is outside the home directory: %s\n' "$config_canon" >&2
      return 1
      ;;
  esac
  unitdir="${config_home}/systemd/user"
  mkdir -p -- "$unitdir"
  canon=$(realpath -- "$unitdir") || return 1
  case "$canon" in
    "${home_canon}"/*)
      printf '%s\n' "$canon"
      ;;
    *)
      printf 'unit directory is outside the home directory: %s\n' "$canon" >&2
      return 1
      ;;
  esac
}

require_tools() {
  local tool
  for tool in install mkdir getent realpath; do
    command -v "$tool" >/dev/null 2>&1 || die "missing command: $tool"
  done
}

write_unit() {
  local dest=$1
  cat >"$dest" <<'EOF'
[Unit]
Description=Per-window keyboard layout for Sway
StartLimitIntervalSec=10
StartLimitBurst=5

[Service]
Type=simple
# SWAYSOCK contains Sway's pid. Import it after Sway starts, then restart
# this service. A copy started at login, before that import, cannot see Sway.
ExecStart=%h/.local/bin/skbd
Restart=on-failure
RestartSec=2
RestartPreventExitStatus=2
NoNewPrivileges=yes
PrivateTmp=yes
RestrictAddressFamilies=AF_UNIX
ProtectSystem=strict
EOF
}

install_built() {
  local root binary man_src tmp bindir mandir unitdir
  root=$(source_root)
  binary="${root}/bin/skbd"
  man_src="${root}/man/skbd.1"
  if [[ ! -f "$binary" || ! -x "$binary" ]]; then
    die "missing local binary: ${binary}. Run make"
  fi
  if [[ ! -f "$man_src" ]]; then
    die "missing man page: ${man_src}"
  fi

  tmp=$(mktemp -d) || die "could not create a temp directory"
  # shellcheck disable=SC2064 # tmp is set; the trap must keep this directory.
  trap "rm -rf -- ${tmp@Q}" EXIT

  bindir="${HOME}/.local/bin"
  mandir="${HOME}/.local/share/man/man1"
  unitdir=$(
    canonical_unit_dir "$passwd_home" "$(config_home_for "${XDG_CONFIG_HOME:-}" "$HOME")"
  ) || die "refusing unit directory outside the home directory"
  mkdir -p -- "$bindir" "$mandir"
  install -m 755 -- "$binary" "${bindir}/skbd"
  write_unit "${tmp}/skbd.service"
  install -m 644 -- "${tmp}/skbd.service" "${unitdir}/skbd.service"
  install -m 644 -- "$man_src" "${mandir}/skbd.1"

  if command -v systemctl >/dev/null 2>&1 && [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
    systemctl --user daemon-reload
  else
    printf 'user systemd is not available in this shell. After you log in, run: systemctl --user daemon-reload\n'
  fi

  printf 'installed %s\n' "${bindir}/skbd"
  printf 'installed %s\n' "${unitdir}/skbd.service"
  printf 'installed %s\n' "${mandir}/skbd.1"
  printf 'The service is not started. After Sway is up:\n'
  printf '  systemctl --user import-environment SWAYSOCK\n'
  printf '  systemctl --user restart skbd.service\n'
}

self_check_unit() {
  local tmp
  tmp=$(mktemp)
  write_unit "$tmp"
  grep -q '^Description=Per-window keyboard layout for Sway$' "$tmp"
  grep -q '^RestartPreventExitStatus=2$' "$tmp"
  grep -q '^StartLimitIntervalSec=10$' "$tmp"
  grep -q '^StartLimitBurst=5$' "$tmp"
  grep -q '^Type=simple$' "$tmp"
  grep -q '^ExecStart=%h/.local/bin/skbd$' "$tmp"
  grep -q '^Restart=on-failure$' "$tmp"
  grep -q '^RestartSec=2$' "$tmp"
  grep -q '^NoNewPrivileges=yes$' "$tmp"
  grep -q '^PrivateTmp=yes$' "$tmp"
  grep -q '^RestrictAddressFamilies=AF_UNIX$' "$tmp"
  grep -q '^ProtectSystem=strict$' "$tmp"
  if grep -q 'WatchdogSec' "$tmp"; then
    rm -f -- "$tmp"
    printf '%s: unit must not set WatchdogSec\n' "$script_name" >&2
    exit 1
  fi
  rm -f -- "$tmp"
}

self_check_unit_dir() {
  local tmp home unit
  tmp=$(mktemp -d)
  home="${tmp}/home"
  mkdir -p -- "$home"
  [[ "$(config_home_for "" "$home")" == "${home}/.config" ]]
  [[ "$(config_home_for "/xdg" "$home")" == "/xdg" ]]
  [[ "$(config_home_for "relative" "$home")" == "${home}/.config" ]]
  unit=$(canonical_unit_dir "$home" "${home}/.config")
  [[ "$unit" == "${home}/.config/systemd/user" ]]
  mkdir -p -- "${tmp}/realhome"
  ln -s "${tmp}/realhome" "${tmp}/linkhome"
  unit=$(canonical_unit_dir "${tmp}/linkhome" "${tmp}/linkhome/.config")
  [[ "$unit" == "${tmp}/realhome/.config/systemd/user" ]]
  mkdir -p -- "${tmp}/outside"
  if canonical_unit_dir "$home" "${tmp}/outside" >/dev/null 2>&1; then
    rm -rf -- "$tmp"
    exit 1
  fi
  if [[ -e "${tmp}/outside/systemd" || -e "${tmp}/outside/systemd/user" ]]; then
    rm -rf -- "$tmp"
    printf '%s: refused unit path still created systemd/user\n' "$script_name" >&2
    exit 1
  fi
  ln -s "${tmp}/outside" "${home}/escape"
  if canonical_unit_dir "$home" "${home}/escape" >/dev/null 2>&1; then
    rm -rf -- "$tmp"
    exit 1
  fi
  if [[ -e "${tmp}/outside/systemd" || -e "${home}/escape/systemd" ]]; then
    rm -rf -- "$tmp"
    printf '%s: refused unit path still created systemd/user\n' "$script_name" >&2
    exit 1
  fi
  rm -rf -- "$tmp"
}

self_check() {
  [[ -f "$(source_root)/man/skbd.1" ]]
  self_check_unit
  self_check_unit_dir
  printf 'ok\n'
}

main() {
  case "${1:-}" in
    -h | --help)
      usage
      exit 0
      ;;
    --self-check)
      if [[ "$#" -ne 1 ]]; then
        usage >&2
        exit 2
      fi
      self_check
      exit 0
      ;;
    "")
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac

  if [[ "$(uname -s)" != "Linux" ]]; then
    die "Linux only"
  fi
  if [[ "${EUID}" -eq 0 ]]; then
    die "run as your user, not root. The unit is a systemd user service"
  fi
  require_tools
  require_own_home
  install_built
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
