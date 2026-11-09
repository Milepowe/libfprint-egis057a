# EH57A protocol notes

How `driver/egis057a/egis057a_proto.c` and `driver/egis057a.c` talk to the
LighTuning Technology Inc. EgisTec EH57A (`1c7a:057a`). Everything below is what
the driver relies on, verified against a live sensor (firmware `4.1.1.1`).

## Transport

| Step | Bytes (bulk OUT EP `0x01`) | Reply (bulk IN EP `0x82`) |
|---|---|---|
| send APDU | `"EGIS" a0 <len16> <APDU>` | `"SIGE" … status` (`01` ok, `02` busy → retry) |
| poll answer | `"EGIS" a1 00 00` | `"SIGE" … status` (`01` ready, `02` pending), then the data transfer ending in `SW1 SW2` (`90 00` ok, `6F 00` retry) |
| scan keepalive on / off | `"EGIS" a3 00 01` / `a3 00 00` | — |

The keepalive (`a3 00 01`) keeps the scanner and LED armed; it must be refreshed
about every 450 ms while a command waits for a finger.

## 55AA commands

APDU `00 50 00 00`, payload `55 AA 00 <cmd> <sub> 00 <len16> <data>` followed by a
reflected CRC-16-CCITT (poly `0x8408`, init `0`, big-endian on the wire). The reply
echoes `cmd | 0x80`, carries a status byte and data.

| Command | Meaning |
|---|---|
| `7f 00` | firmware version string |
| `07 00` | calibrate |
| `19 00` | list template ids stored on the chip (32 bytes each, max 10) |
| `18 00 [id]` | delete one template id (all if no payload) |
| `16 01` | start enrolment → 32-byte nonce |
| `16 02` | capture one enrolment press → `[progress, total]` (`fa` when complete; `97` no finger; `01`/`19`/`93`/`95` retry) |
| `1a 00` | finger present? (`97` = no finger) |
| `16 05` | is this finger already enrolled? (`04` = no; `00` + id = yes) |
| `16 03 <id>` | commit enrolment under `id = HMAC-SHA256(app_key0, "enroll\0" ‖ nonce)` |
| `16 04` | abort enrolment |
| `17 02` | wait for a finger for verify (falls back to `1a 00`) |
| `17 00 00 ‖ ids…` | match the finger against the given ids → matched id (`00`), or `0a` = matched and a template update is pending |
| `17 01` | commit the template update after a `0a` match |

## Session handshake (`APDU 00 57 01 00`, no CRC)

1. Host sends a 32-byte random and an uncompressed P-256 public key.
2. The chip answers with its own random, a certificate block (`N + 0x122` bytes,
   public key at offset `N + 0x41`) and a 32-byte tail.
3. `secret = ECDH(host_priv, chip_pub)`;
   `master = KDF(secret, "master secret", host_rand ‖ chip_rand)`;
   `app_keys = KDF(master, "application keys")` (64 bytes);
   KDF = NIST SP800-108 counter mode with HMAC-SHA256.
4. The host verifies `tail == HMAC-SHA256(app_keys[0..32], "connect\0" ‖ SHA256(cert block))`.
   `app_keys[0..32]` is `app_key0`, used for the enrolment id above.

A chip that answers `0xe0` has lost the session; the driver re-runs the handshake.

## Pitfalls

- Never pass a `GCancellable` to a USB transfer — aborting an in-flight URB can
  wedge the sensor until power is cut. The driver checks for cancellation between
  command sequences instead.
- Do not send `EGIS 99 01 00` (device reset): it re-enumerates the USB device.

