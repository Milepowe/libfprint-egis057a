# Suspend / resume integration

Two small, fully reversible helpers that keep the EH57A usable across
suspend/resume. Both are installed automatically by [`../install.sh`](../install.sh);
this directory documents them and lets you install them by hand.

**Keep them installed.** They are the standard workaround for a real upstream
gnome-shell/fprintd bug, not a crutch for a driver shortcoming.

## Why

1. **"No fingerprint after resume"**: on a fraction of resumes the lock screen
   offers only the password. The journal shows the cause has nothing to do with
   the sensor:

   ```
   Requesting authorization … to call method 'Claim' for device '… EH57A'
   Authorization denied … : Device was already claimed
   ```

   `fprintd` still holds a **stale claim** taken by gnome-shell before suspend and
   never released; the post-resume claim is refused and libfprint is never
   reached. This is a known gnome-shell/fprintd bug (gnome-shell
   [#7791](https://gitlab.gnome.org/GNOME/gnome-shell/-/issues/7791), Ubuntu
   [#2067135](https://bugs.launchpad.net/bugs/2067135)), not specific to this
   sensor.
2. **Stale sensor state**: across s2idle the USB device stays powered and is not
   re-enumerated, so the sensor can keep a session from before the suspend. The
   driver re-runs its handshake after a resume; re-enumerating the device makes
   that start from a clean sensor.

## What these do

| File | Installs to | Effect |
|---|---|---|
| `50-egis057a-fp-resume.sh` | `/usr/lib/systemd/system-sleep/` | **`pre`: stops `fprintd` before sleep** so no stale claim survives. **`post`: re-enumerates the sensor** (`authorized` 0→1). |
| `egis057a-fp-wait` | `/usr/libexec/` | **Gate on `fprintd`'s start.** The hook's `post` phase takes about a second to re-enumerate the sensor, and `fprintd` is D-Bus activated, so the desktop can activate it inside exactly that window and claim a device that is about to leave the bus. This waits for the hook's marker to disappear, then lets `fprintd` start. Fail-open: after 15 s it starts it anyway, so a marker left by a hook that died cannot disable fingerprint authentication. |
| `egis057a-fprintd-wait.conf` | `/etc/systemd/system/fprintd.service.d/` | The `ExecStartPre=` drop-in that runs the gate above. |

Restarting `fprintd` around suspend is the community-standard workaround for the
upstream claim bug. Fingerprint stays enabled everywhere — login, `sudo`, **and**
unlock; nothing is disabled.

## Manual install

```bash
sudo install -m 0755 50-egis057a-fp-resume.sh    /usr/lib/systemd/system-sleep/
sudo install -Dm 0755 egis057a-fp-wait /usr/libexec/egis057a-fp-wait
sudo install -Dm 0644 egis057a-fprintd-wait.conf \
    /etc/systemd/system/fprintd.service.d/10-egis057a-resume-wait.conf
sudo systemctl daemon-reload
```

Revert by deleting the three files, then `sudo systemctl daemon-reload`.

The gate is the one piece here that was not written for this sensor: the race it
closes was diagnosed on an IdeaPad Flex 5 14ITL05 by
[sam-dant](https://github.com/sam-dant/egis0576-resume-workaround), who measured
`fprintd` starting one second before the hook finished and the claim still being
refused 46 s later, and who built and tested the marker-plus-gate design this
implements (reported in
[PHILIPPDEV5396/libfprint-egis0576#3](https://github.com/PHILIPPDEV5396/libfprint-egis0576/issues/3)).

## Test it

Lock the screen (`Super+L`), suspend, wait ~30 s, wake, and unlock with your
fingerprint. It should be offered immediately.
