# Provenance & Licensing

An honest account of where the code in this repository comes from.

## What this is

A [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) driver for the
**LighTuning Technology Inc. EgisTec EH57A** fingerprint sensor (USB ID
`1c7a:057a`), which ships in laptops but has **no vendor-provided Linux driver**.
The EH57A protocol was worked out by analysing Egis' Windows driver
(`EgisTouchFP057A.dll` / `EgisTouchFPAdapter057A.dll`, v4.1.0.17, obtained from the
Microsoft Update Catalog) and by probing the sensor, for the sole purpose of
**interoperability** — making hardware that people already own usable on Linux.

## Where the code comes from

File paths are relative to `driver/`; `install.sh` copies them into
`libfprint/drivers/` of the libfprint tree it builds.

### 1. Foundation — © Philipp Oster and contributors, LGPL-2.1-or-later

This repository was started from
[`PHILIPPDEV5396/libfprint-egis0576`](https://github.com/PHILIPPDEV5396/libfprint-egis0576),
a driver for the neighbouring EgisTec EH576. These parts of that work are carried
over (adapted, renamed, and in places reduced) and keep their original credit in
the file headers:

| What | Where |
|---|---|
| libfprint `FpDevice` driver skeleton, cancellation rules, suspend / resume parking | `egis057a.c` |
| asynchronous `FpiSsm` / `FpiUsbTransfer` transport (`EGIS` command / reply helpers, endpoint drain, cancel-at-boundary checks) | `egis057a/egis057a_proto.c` |
| meson / hwdb integration patch and `install.sh` structure | `patches/`, `install.sh` |
| sleep hook | `integration/50-egis057a-fp-resume.sh` |
| `fprintd` start gate (design by [sam-dant](https://github.com/sam-dant/egis0576-resume-workaround)) | `integration/egis057a-fp-wait`, `integration/egis057a-fprintd-wait.conf` |

Everything specific to the EH576 in that project — its image sensor bring-up, the
host-side fingerprint matchers (including the matcher translated from Egis'
EH576 driver and Thaddeus Stepanovich's clean-room matcher), the accuracy kit,
the distro packaging — is **not** part of this repository. The git history still
contains it, with its authors.

### 2. EH57A match-on-chip protocol — © Milepower, LGPL-2.1-or-later

Written for this repository, from a description of the sensor's behaviour:

| What | Where |
|---|---|
| `EGIS a0` / `a1` APDU transport, `55 AA` framing, CRC-16 | `egis057a/egis057a_proto.c` |
| ECDH P-256 + SP800-108 KDF session handshake, enrolment id derivation | `egis057a/egis057a_proto.c` |
| enrol / verify / identify state machine | `egis057a.c` |
| protocol notes | `../docs/protocol.md` |

The command set, status codes and cryptographic framing are facts about the
device's interface. **No Egis code, tables, keys or binary blobs are included**:
the session keys are generated per connection (ephemeral ECDH), and the KDF labels
(`"master secret"`, `"application keys"`, `"connect"`, `"enroll"`) are protocol
constants.

## Legal basis and honest risk disclosure

*This is not legal advice.*

- **Reverse-engineering for interoperability is legal.** In the EU it is explicitly
  permitted by [Directive 2009/24/EC, Article 6](https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:32009L0024);
  in the US it is broadly protected under the interoperability line of fair-use
  cases.
- **The original DLLs and any decompilation of them are deliberately excluded**
  (see `.gitignore`) and must never be committed. Only the protocol knowledge
  needed for the device to work is present here, written in original code.
- **Realistic worst case:** a takedown request, not prosecution of users.

## Prior art

- [`PHILIPPDEV5396/libfprint-egis0576`](https://github.com/PHILIPPDEV5396/libfprint-egis0576) — the foundation above.
- [`Pengu601/EgisTec-EH576`](https://github.com/Pengu601/EgisTec-EH576) — the EgisTec
  EH576 protocol notes that project started from.
- [`tsteppy/egistec-eh576-libfprint`](https://github.com/tsteppy/egistec-eh576-libfprint) —
  a clean-room EH576 matcher (not used here).
- [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) and the other
  `egis*` drivers in its tree (`egismoc`, `egis0570`), whose conventions this driver follows.
