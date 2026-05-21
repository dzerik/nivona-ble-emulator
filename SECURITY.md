# Security Policy

## Supported Versions

Only the latest `emu-v*` release is supported. Issues against older
tags may still be triaged but the fix will only ship in the latest
line.

| Version | Supported |
|---------|-----------|
| Latest release | Yes |
| Older releases | No |

## Reporting a Vulnerability

If you discover a security vulnerability, please report it privately
— **do not open a public issue**.

Preferred path: open a private advisory via GitHub Security Advisories:
<https://github.com/dzerik/nivona-ble-emulator/security/advisories/new>

Include:

- A description of the vulnerability and its scope.
- Steps to reproduce — ideally a CLI / packet trace against a running
  emulator instance.
- Affected version (`VERSION` file or `emu-v*` tag).
- Your suggested mitigation, if any.

You will receive an initial response within 7 days. Fixes will ship
as a patch release (`emu-vX.Y.Z+1`) with a CVE assignment if the
issue warrants one.

## Scope

This repository ships **emulator** firmware that impersonates a
Nivona coffee machine over BLE. The threat model is:

- **In scope:** anything that lets a network-adjacent attacker
  bypass the BLE handshake / RC4 session-key gate, exfiltrate the
  session key, crash the device remotely, or escalate from the
  unauthenticated `/diag` HTTP endpoint to remote code execution.
- **Also in scope:** any credential leak via the BLE advertising
  data, Device-Information-Service, or log output.
- **Out of scope:** physical access to the device, vulnerabilities
  in upstream ESP-IDF / NimBLE / lwIP (report those directly to
  Espressif / Apache NimBLE / lwIP).

## BLE Security Note

This emulator implements the Nivona BLE protocol verbatim, including
its 2-byte session-key prefix and RC4 frame encryption. The encryption
is **not** designed to resist a determined attacker — RC4 with a
hardcoded master key is the protocol's own design and we mirror it
faithfully so the real Android app can talk to the emulator. **Treat
the emulator's BLE traffic as authenticated-but-not-confidential.**

The companion `wifi_secrets.h` file is `.gitignored`; never commit
your real Wi-Fi credentials. The CI build uses the template values
only.

## Dev-only services

The emulator exposes an unauthenticated HTTP `/diag` endpoint and a
plain-text telnet CLI on port 23. Both are intended for development
on a trusted LAN. **Do not deploy the emulator on an untrusted
network.** Future releases may gate `/diag` behind a build-time flag
— track that in the FUNCTIONAL_COVERAGE residuals list.
