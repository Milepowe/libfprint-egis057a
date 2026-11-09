#!/usr/bin/env bash
#
# Build and install libfprint with the EgisTec EH57A (egis057a) driver.
#
# This clones a pinned upstream libfprint, drops the egis057a driver into it,
# applies a small meson patch, builds the full default driver set (so your other
# fingerprint hardware keeps working), and installs it.
#
# A libfprint driver is compiled *into* libfprint-2.so -- it is not a loadable
# module -- so "installing the driver" means installing a libfprint that contains
# it. This does NOT rebuild fprintd; the stock fprintd loads the new library via
# its unchanged ABI.
#
set -euo pipefail

LIBFPRINT_VERSION="1.94.100"
LIBFPRINT_URL="https://gitlab.freedesktop.org/libfprint/libfprint.git"
PREFIX="${PREFIX:-/usr/local}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH="$REPO/patches/libfprint-${LIBFPRINT_VERSION}-egis057a.patch"

echo ">>> EgisTec EH57A (1c7a:057a) libfprint driver installer"
echo "    libfprint version : $LIBFPRINT_VERSION"
echo "    install prefix    : $PREFIX   (override with: PREFIX=/usr ./install.sh)"
echo

# --- required build tools ---------------------------------------------------
missing=0
for tool in git meson ninja gcc pkg-config; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "  MISSING build tool: $tool"; missing=1
    fi
done
if [ "$missing" = 1 ]; then
    echo
    echo "Install the build tools + dev libraries first (see README.md, 'Building')."
    echo "You also need the -dev packages for: glib2, gusb, openssl/libcrypto (>=3.0),"
    echo "gobject-introspection, gudev, nss, pixman, cairo, udev/systemd (Debian: systemd-dev)"
    echo "and gtk-doc (Debian: gtk-doc-tools; needed because the API docs are built by default)."
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo ">>> cloning libfprint $LIBFPRINT_VERSION ..."
git clone --quiet --depth 1 --branch "v$LIBFPRINT_VERSION" "$LIBFPRINT_URL" "$WORK/libfprint"
cd "$WORK/libfprint"

echo ">>> installing the egis057a driver sources ..."
cp "$REPO/driver/egis057a.c" "$REPO/driver/egis057a.h" libfprint/drivers/
cp -r "$REPO/driver/egis057a" libfprint/drivers/

echo ">>> applying the meson integration patch ..."
git apply "$PATCH"

echo ">>> configuring + building (full default driver set + egis057a) ..."
# Extra meson options can be passed with e.g.
#   EGIS057A_MESON_ARGS="-Ddoc=false -Dgtk-examples=false" ./install.sh
meson setup builddir --prefix="$PREFIX" --buildtype=release ${EGIS057A_MESON_ARGS:-}
ninja -C builddir

echo ">>> installing to $PREFIX (sudo) ..."
sudo ninja -C builddir install
sudo ldconfig

# --- suspend/resume integration --------------------------------------------
# The sleep hook stops fprintd before suspend so no stale device claim survives
# (an upstream gnome-shell/fprintd bug, not a driver issue) and re-enumerates the
# sensor after resume. See integration/README.md. Fully reversible (delete the
# files).
echo ">>> installing suspend/resume integration (sleep hook + fprintd gate) ..."
if [ -f "$REPO/integration/50-egis057a-fp-resume.sh" ]; then
    sudo install -m 0755 "$REPO/integration/50-egis057a-fp-resume.sh"    /usr/lib/systemd/system-sleep/
    # Gate: hold fprintd's start while the resume hook re-enumerates the sensor
    # (fail-open after 15 s). See integration/egis057a-fp-wait.
    sudo install -Dm 0755 "$REPO/integration/egis057a-fp-wait" /usr/libexec/egis057a-fp-wait
    sudo install -Dm 0644 "$REPO/integration/egis057a-fprintd-wait.conf" \
        /etc/systemd/system/fprintd.service.d/10-egis057a-resume-wait.conf
    sudo systemctl daemon-reload 2>/dev/null || true
    sudo udevadm control --reload-rules 2>/dev/null || true
    sudo udevadm trigger --attr-match=idVendor=1c7a 2>/dev/null || true
    echo "    installed (see integration/README.md; delete the three files to revert)."
fi

echo
echo ">>> Done. libfprint (with egis057a) installed to $PREFIX."
echo
echo "    IMPORTANT: fprintd must load THIS libfprint, not the distro's."
echo "    - If you installed to /usr/local, make sure $PREFIX/lib (or lib64) is on"
echo "      the linker path (it usually is on Fedora/Arch). Verify with:"
echo "          ldconfig -p | grep libfprint-2"
echo "      It should point at $PREFIX. If not, add $PREFIX/lib* to"
echo "      /etc/ld.so.conf.d/ and re-run 'sudo ldconfig'."
echo "    - Then restart the daemon:  sudo systemctl restart fprintd"
echo
echo "    Next: enroll a finger (fprintd-enroll) and enable PAM -- see README.md."
