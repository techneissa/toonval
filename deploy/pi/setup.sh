#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Sets up a Raspberry Pi OS Lite machine to build and run toonval.
#
#   sudo deploy/pi/setup.sh [--dry-run] [--no-build] [--enable kmsdrm|labwc] [--prefix DIR]
#
# What it does, in order (each step says [ok] when nothing needed doing):
#   1. apt: installs the packages the build needs (the CI list plus KMSDRM and labwc extras).
#   2. user: creates the `toonval` user and adds it to video, render, input and audio.
#      The login user that ran sudo is added to the same groups, so display-spike runs from it.
#   3. build: Release build in <repo>/build/pi-release, as the login user (not root).
#   4. install: toonval, display-spike and midi-ports into <prefix>/bin (default /opt/toonval).
#   5. icon: the hicolor PNGs and a toonval.desktop entry under /usr/local/share, then refreshes the
#      icon cache when gtk-update-icon-cache is there.
#   6. systemd: installs both units and the PAM file. A unit is enabled only with --enable.
# It never removes anything, never starts the service, and is safe to run again.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

APP_USER=toonval
APP_GROUPS=(video render input audio)
PREFIX=/opt/toonval
BUILD_DIR="$REPO/build/pi-release"
UNIT_DIR=/etc/systemd/system
PAM_FILE=/etc/pam.d/toonval
DRY_RUN=0
DO_BUILD=1
ENABLE=""
JOBS="$(nproc 2>/dev/null || echo 2)"

# The CI package list (.github/workflows/ci.yml), plus cmake and a compiler.
BUILD_PACKAGES=(
    build-essential cmake ninja-build pkg-config ca-certificates
    libasound2-dev libudev-dev
    libdrm-dev libgbm-dev libegl-dev libgles-dev
    libx11-dev libxext-dev libxcursor-dev libxi-dev libxfixes-dev libxrandr-dev
    libxss-dev libxtst-dev libwayland-dev libxkbcommon-dev wayland-protocols
    libdecor-0-dev
)
# The GPU drivers (Mesa) the KMSDRM and labwc paths draw with, and the tools the checklist uses.
RUNTIME_PACKAGES=(libgl1-mesa-dri libegl1 libgles2 libgbm1 evtest alsa-utils kbd)
# For the labwc kiosk path only. Skipped with a warning if the repository does not carry them.
LABWC_PACKAGES=(labwc wlr-randr)

usage() {
    sed -n '3,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

step() { printf '\n== %s\n' "$*"; }
ok() { printf '  [ok]     %s\n' "$*"; }
change() { printf '  [change] %s\n' "$*"; }
warn() { printf '  [warn]   %s\n' "$*" >&2; }
die() {
    printf 'setup.sh: %s\n' "$*" >&2
    exit 1
}

# Prints the command and runs it, unless this is a dry run.
run() {
    printf '  +'
    printf ' %q' "$@"
    printf '\n'
    if ((DRY_RUN)); then
        return 0
    fi
    "$@"
}

while (($#)); do
    case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --no-build) DO_BUILD=0 ;;
    --enable)
        shift
        [[ "${1:-}" == kmsdrm || "${1:-}" == labwc ]] || die "--enable needs kmsdrm or labwc"
        ENABLE="$1"
        ;;
    --prefix)
        shift
        [[ -n "${1:-}" ]] || die "--prefix needs a directory"
        PREFIX="$1"
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *) die "unknown option $1 (try --help)" ;;
    esac
    shift
done

# --- checks ------------------------------------------------------------------------------------

step "Checking the machine"
if [[ "$(uname -s)" != Linux ]]; then
    ((DRY_RUN)) || die "this only runs on Linux (Pi OS); use --dry-run to read the plan"
    warn "not Linux: showing the plan only, package and user checks are skipped"
fi
if ((EUID != 0)) && ((!DRY_RUN)); then
    die "run it as root: sudo $0 $*"
fi
if [[ -r /proc/device-tree/model ]]; then
    ok "board: $(tr -d '\0' </proc/device-tree/model)"
else
    warn "no /proc/device-tree/model: not a Raspberry Pi? continuing"
fi
[[ -f "$REPO/CMakeLists.txt" ]] || die "no CMakeLists.txt in $REPO"
ok "repository: $REPO"
((DRY_RUN)) && ok "dry run: commands are printed with a leading +, not run"

# The login user who ran sudo; builds and group membership use it, not root.
LOGIN_USER=""
if [[ -n "${SUDO_USER:-}" && "${SUDO_USER}" != root ]]; then
    LOGIN_USER="$SUDO_USER"
fi

# --- 1. packages -------------------------------------------------------------------------------

is_installed() {
    command -v dpkg-query >/dev/null 2>&1 || return 1
    local status
    status="$(dpkg-query -W -f='${Status}' "$1" 2>/dev/null || true)"
    [[ "$status" == *"install ok installed"* ]]
}

APT_UPDATED=0
apt_update_once() {
    if ((!APT_UPDATED)); then
        run apt-get update
        APT_UPDATED=1
    fi
}

# install_packages REQUIRED|OPTIONAL pkg...
install_packages() {
    local kind="$1"
    shift
    local missing=() pkg
    for pkg in "$@"; do
        is_installed "$pkg" || missing+=("$pkg")
    done
    if ((${#missing[@]} == 0)); then
        ok "$kind packages installed ($#)"
        return 0
    fi
    if [[ "$kind" == OPTIONAL ]] && command -v apt-cache >/dev/null 2>&1; then
        local available=()
        for pkg in "${missing[@]}"; do
            if apt-cache show "$pkg" >/dev/null 2>&1; then
                available+=("$pkg")
            else
                warn "optional package $pkg is not in the apt sources; skipping it"
            fi
        done
        if ((${#available[@]} == 0)); then
            return 0
        fi
        missing=("${available[@]}")
    fi
    change "installing ${#missing[@]} $kind package(s): ${missing[*]}"
    apt_update_once
    run env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${missing[@]}"
}

step "1. Packages"
install_packages REQUIRED "${BUILD_PACKAGES[@]}" "${RUNTIME_PACKAGES[@]}"
install_packages OPTIONAL "${LABWC_PACKAGES[@]}"

# --- 2. user and groups ------------------------------------------------------------------------

in_group() { # in_group USER GROUP
    local members
    members="$(id -nG "$1" 2>/dev/null || true)"
    [[ " $members " == *" $2 "* ]]
}

ensure_groups() { # ensure_groups USER
    local user="$1" group
    for group in "${APP_GROUPS[@]}"; do
        if ! getent group "$group" >/dev/null 2>&1; then
            warn "group $group does not exist on this image; drop it from SupplementaryGroups in the units"
        elif in_group "$user" "$group"; then
            ok "$user is in $group"
        else
            change "adding $user to $group"
            run usermod -aG "$group" "$user"
        fi
    done
}

step "2. User and groups"
if id "$APP_USER" >/dev/null 2>&1; then
    ok "user $APP_USER exists"
else
    change "creating user $APP_USER (no login shell, no password)"
    run useradd --create-home --shell /usr/sbin/nologin --comment "toonval kiosk" "$APP_USER"
fi
ensure_groups "$APP_USER"
if [[ -n "$LOGIN_USER" ]]; then
    ensure_groups "$LOGIN_USER"
    ok "log out and in again for $LOGIN_USER's new groups to apply"
fi

# --- 3. build ----------------------------------------------------------------------------------

as_builder() {
    if [[ -n "$LOGIN_USER" ]]; then
        run runuser -u "$LOGIN_USER" -- "$@"
    else
        run "$@"
    fi
}

step "3. Build (Release)"
if ((DO_BUILD)); then
    change "configuring and building in $BUILD_DIR (the first run downloads dependencies)"
    as_builder cmake -S "$REPO" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DTOONVAL_BUILD_TESTS=OFF
    as_builder cmake --build "$BUILD_DIR" --parallel "$JOBS" \
        --target toonval display-spike midi-ports
else
    ok "skipped (--no-build)"
fi

# --- 4. install --------------------------------------------------------------------------------

CHANGED=0 # set by install_file: 1 when it wrote the destination

# install_file SRC DEST MODE: copies only when the content differs.
install_file() {
    local src="$1" dest="$2" mode="$3"
    CHANGED=0
    if [[ ! -e "$src" ]]; then
        if ((DRY_RUN)); then
            change "would install $dest (not built yet)"
            CHANGED=1
            return 0
        fi
        die "missing $src: build first, or drop --no-build"
    fi
    if [[ -f "$dest" ]] && cmp -s "$src" "$dest"; then
        ok "$dest unchanged"
        return 0
    fi
    change "installing $dest"
    CHANGED=1
    run install -D -m "$mode" "$src" "$dest"
}

step "4. Install binaries to $PREFIX/bin"
install_file "$BUILD_DIR/app/toonval" "$PREFIX/bin/toonval" 0755
install_file "$BUILD_DIR/tools/display-spike" "$PREFIX/bin/display-spike" 0755
install_file "$BUILD_DIR/tools/midi-ports" "$PREFIX/bin/midi-ports" 0755
# The built-in piano samples, made by the build from FluidR3_GM (see cmake/PianoAsset.cmake).
if [[ -f "$BUILD_DIR/app/toonval-piano.sf2" ]] || ((DRY_RUN)); then
    install_file "$BUILD_DIR/app/toonval-piano.sf2" "$PREFIX/share/toonval/toonval-piano.sf2" 0644
else
    warn "no built-in piano samples in the build; taps and typing will be silent"
fi

# --- 5. icon and desktop entry -----------------------------------------------------------------

step "5. Icon and desktop entry"
# Desktops only look in the XDG data dirs, so the icon and the entry go to /usr/local/share
# (not under the prefix). The entry's Exec points at this install's binary.
XDG_SHARE=/usr/local/share
ICON_SIZES=(16 32 48 64 128 256 512)
ICONS_CHANGED=0
for size in "${ICON_SIZES[@]}"; do
    install_file "$REPO/app/assets/icon/toonval-$size.png" \
        "$XDG_SHARE/icons/hicolor/${size}x${size}/apps/toonval.png" 0644
    ((CHANGED)) && ICONS_CHANGED=1
done
DESKTOP_TMP="$(mktemp)"
sed "s|^Exec=.*|Exec=$PREFIX/bin/toonval|" "$REPO/deploy/linux/toonval.desktop" >"$DESKTOP_TMP"
install_file "$DESKTOP_TMP" "$XDG_SHARE/applications/toonval.desktop" 0644
rm -f "$DESKTOP_TMP"
if ((ICONS_CHANGED)); then
    if command -v gtk-update-icon-cache >/dev/null 2>&1 || ((DRY_RUN)); then
        run gtk-update-icon-cache -f -t "$XDG_SHARE/icons/hicolor"
    else
        warn "gtk-update-icon-cache not found; the icon appears after the next icon cache refresh"
    fi
fi

# --- 6. systemd --------------------------------------------------------------------------------

step "6. systemd units"
if ! command -v systemctl >/dev/null 2>&1; then
    warn "no systemctl here; skipping units"
else
    UNITS_CHANGED=0
    install_file "$SCRIPT_DIR/pam-toonval" "$PAM_FILE" 0644
    install_file "$SCRIPT_DIR/toonval.service" "$UNIT_DIR/toonval.service" 0644
    ((CHANGED)) && UNITS_CHANGED=1
    install_file "$SCRIPT_DIR/toonval-labwc.service" "$UNIT_DIR/toonval-labwc.service" 0644
    ((CHANGED)) && UNITS_CHANGED=1
    if ((UNITS_CHANGED)); then
        run systemctl daemon-reload
    fi
    if ((!DRY_RUN)); then
        systemd-analyze verify "$UNIT_DIR/toonval.service" "$UNIT_DIR/toonval-labwc.service" ||
            warn "systemd-analyze verify reported the issues above; read them before enabling"
    fi

    case "$ENABLE" in
    kmsdrm | labwc)
        [[ "$ENABLE" == kmsdrm ]] && chosen=toonval.service other=toonval-labwc.service
        [[ "$ENABLE" == labwc ]] && chosen=toonval-labwc.service other=toonval.service
        if systemctl is-enabled --quiet "$other" 2>/dev/null; then
            change "disabling $other"
            run systemctl disable "$other"
        fi
        if systemctl is-enabled --quiet "$chosen" 2>/dev/null; then
            ok "$chosen already enabled"
        else
            change "enabling $chosen (starts at the next boot)"
            run systemctl enable "$chosen"
        fi
        ;;
    *)
        ok "no unit enabled (pass --enable kmsdrm or --enable labwc once the checklist passes)"
        ;;
    esac
fi

step "Done"
cat <<EOF
Nothing was started. Next:
  - Log in on the Pi's own console (tty1, not over SSH), then run the checks in
    $SCRIPT_DIR/CHECKLIST.md, for example:
      $PREFIX/bin/display-spike --seconds 10
  - Test the service by hand with: sudo systemctl start toonval.service
    (this takes over tty1; stop it from SSH with: sudo systemctl stop toonval.service)
EOF
