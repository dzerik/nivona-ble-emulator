# Legal Notice and Position Statement

This document explains the legal basis on which this project operates,
its intent, and the maintainer's willingness to engage constructively
with any legitimate concerns from rights-holders.

## Purpose of the project

`nivona-ble-emulator` is a **research and interoperability tool**.
It impersonates the Bluetooth Low Energy protocol of a coffee machine
of the Nivona NICR / NIVO product family — manufactured by Eugster /
Frismag AG — so that the **official, vendor-released Android
application** can talk to the emulator instead of (or in addition to)
a physical machine.

Its concrete uses are:

- Developing and testing third-party automation integrations
  (notably the `melitta-barista-ha` Home Assistant integration) for
  owners of these machines, without needing a physical device on the
  bench during every test cycle.
- Understanding the BLE protocol surface for *interoperability* —
  bringing first-party device features into open ecosystems that the
  vendor's own application does not support (e.g. local automation
  via Home Assistant on the LAN).
- Education and research on BLE-based device protocols, reverse-
  engineering methodology, and FreeRTOS / ESP-IDF firmware patterns.

It is explicitly **not**:

- A counterfeit coffee machine. The emulator runs on a generic
  ESP32 development board and bears no resemblance to a Nivona
  product, in appearance or function.
- A commercial product. The maintainer derives no revenue from
  this project. It is released under the MIT licence (see
  [`LICENSE`](LICENSE)) at no cost.
- A device intended to bypass any DRM, license enforcement, paid
  service, or anti-tampering measure. The protocol it implements is
  exposed by the machine in normal use to the vendor's own freely
  available Android application.

## Trademarks

"Nivona", "NICR", "NIVO" and any logo, product name, or trade dress
of Nivona Apparate GmbH / Eugster / Frismag AG are the property of
their respective owners. **This project is not affiliated with,
endorsed by, sponsored by, or otherwise connected to those entities
in any way.**

The names appear in this repository only:

- Descriptively, to identify the family of machines the emulator
  targets — nominative fair use under EU and US trademark law.
- In technical documentation that explains protocol details
  observed on the wire.

If any reference would otherwise create user confusion as to the
source of this software, please raise it via the channels below
and the wording will be adjusted.

## Legal basis for the work

The maintainer believes this project sits comfortably within the
boundaries laid out by the following frameworks:

- **EU**: Article 6 of the Software Directive (2009/24/EC), which
  permits decompilation **strictly for the purpose of achieving
  interoperability** between independently created programs and
  the original program, where the necessary information is not
  otherwise readily available. The project's scope is exactly
  this. Member-state implementations (notably **German UrhG §69e**,
  **French CPI L.122-6-1 IV**) follow the same template.
- **United States**: well-established case-law on reverse
  engineering for interoperability — notably
  *Sega Enterprises Ltd. v. Accolade, Inc.* (9th Cir. 1992) and
  *Sony Computer Entertainment v. Connectix Corp.* (9th Cir. 2000)
  — together with the **DMCA §1201(f) interoperability exception**.
- **Russia / Eurasian Economic Union**: Article 1280 of the Russian
  Civil Code, which permits the lawful owner of a software copy to
  study the underlying ideas and principles, including by
  observation of operation.

No proprietary firmware, signed binary, or copyrighted code from
the vendor is redistributed here. Protocol observations are
recorded in textual notes (`docs/*.md`) and re-implemented from
scratch in this project's own source code, under the MIT licence.

## Cooperation with rights-holders

If you represent Nivona Apparate GmbH, Eugster / Frismag AG, or any
other entity with a legitimate interest in this project, **please
reach out before any escalation**. The maintainer is happy to:

- Discuss any specific concern about wording, scope, or naming.
- Clarify the protocol-observation methodology and the sources
  used (we will gladly point out which parts came from the
  vendor's own freely distributed Android application, which from
  on-wire packet capture of *the maintainer's own* hardware, etc.).
- Adjust documentation, code comments, or repository metadata if
  any specific item creates an ambiguity that you would like
  removed.
- Where appropriate, link from this repository to vendor-provided
  resources so users can compare and choose authoritatively.

Preferred contact:

- A GitHub issue tagged `legal` (public, transparent — preferred
  for non-sensitive matters that can benefit the community).
- A private GitHub Security Advisory
  ([new advisory](https://github.com/dzerik/nivona-ble-emulator/security/advisories/new))
  if confidentiality is required.

Good-faith requests will receive a substantive reply within 14 days.

## What this notice is not

This document is **not** legal advice and does not create an
attorney–client relationship. It states the maintainer's
understanding and position; it does not bind any third party. If
you have a legal question about your own use of this software,
consult counsel in your own jurisdiction.

## Changes

This notice may be updated. The full revision history is preserved
in the repository's git log; material changes will also be called
out in [`CHANGELOG.md`](CHANGELOG.md).
