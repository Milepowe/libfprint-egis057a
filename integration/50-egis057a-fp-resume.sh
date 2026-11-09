#!/bin/bash
# systemd system-sleep hook: keep the EgisTec EH57A fingerprint sensor working
# across suspend/resume.
#
# WHY: two independent things go wrong across suspend/resume:
#  1. fprintd keeps the device *claim* gnome-shell took before suspend and never
#     releases it; the post-resume Claim is refused ("Device was already
#     claimed") and libfprint is never reached -> lock screen offers only the
#     password (upstream gnome-shell #7791 / Ubuntu #2067135, not EH57A-specific).
#  2. Across s2idle the USB device stays powered and is NOT re-enumerated, so the
#     sensor may keep a stale ECDH session / scan state from before the suspend.
#     (The driver also re-runs its session handshake after a resume; the
#     re-enumeration makes that start from a clean sensor.)
#
# FIX: stop fprintd before suspend so no stale claim survives (fprintd is
# started again on demand on resume), and force a USB re-enumeration of the
# sensor after resume so the driver re-opens it and re-runs the bring-up on next
# use. Fingerprint stays enabled everywhere (login, sudo, unlock).
#
# Install: copy to /usr/lib/systemd/system-sleep/ (or /etc/systemd/system-sleep/)
# and chmod 0755. Delete the file to revert. install.sh does this for you.
set +e

VID=1c7a
PID=057a
# Consumed by /usr/libexec/egis057a-fp-wait (fprintd's ExecStartPre gate).
MARKER=/run/egis057a-fp-resume.pending

find_dev() {
    local v d
    for v in /sys/bus/usb/devices/*/idVendor; do
        [ "$(cat "$v" 2>/dev/null)" = "$VID" ] || continue
        d=$(dirname "$v")
        [ "$(cat "$d/idProduct" 2>/dev/null)" = "$PID" ] && { echo "$d"; return; }
    done
}

case "$1" in
    pre)
        # Mark the sensor as not ready BEFORE stopping fprintd, so a D-Bus
        # activation that lands in the resume window waits for `post` to finish
        # instead of claiming a device that is about to leave the bus. The
        # egis057a-fp-wait gate consumes this; without that gate installed the
        # marker is simply ignored. /run is a tmpfs, so a reboot clears it.
        : > "$MARKER" 2>/dev/null
        # drop fprintd's stale device claim before we sleep (upstream
        # gnome-shell/fprintd bug); also ends any in-flight operation cleanly
        systemctl stop fprintd.service 2>/dev/null
        ;;
    post)
        # The marker is removed on EVERY exit path below, including the one
        # where the sensor is not found: a marker that outlives its hook would
        # cost every later fprintd start the gate's full timeout.
        dev=$(find_dev)
        if [ -n "$dev" ]; then
            # deauthorize + reauthorize = USB re-enumeration -> the driver
            # re-opens the device and re-runs the sensor bring-up on next use
            echo 0 > "$dev/authorized" 2>/dev/null
            sleep 1
            echo 1 > "$dev/authorized" 2>/dev/null
            logger -t egis057a-fp-resume "EH57A re-enumerated after resume ($dev)" 2>/dev/null
        else
            logger -t egis057a-fp-resume "EH57A not found after resume" 2>/dev/null
        fi
        rm -f "$MARKER" 2>/dev/null
        ;;
esac
exit 0
