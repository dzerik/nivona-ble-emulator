# Contributing

Thanks for considering a contribution. This is a small,
single-maintainer ESP-IDF project; the workflow is intentionally
lightweight.

## Quickstart

```sh
# 1. Clone
git clone git@github.com:dzerik/nivona-ble-emulator.git
cd nivona-ble-emulator

# 2. Provide WiFi credentials (file is gitignored)
cp main/wifi_secrets.h.template main/wifi_secrets.h
# edit main/wifi_secrets.h with your SSID / password / hostname

# 3. Build for your target (default board is xiao_c6)
. $IDF_PATH/export.sh
idf.py -DBOARD=xiao_c6 build   # or xiao_s3 / waveshare_c6_lcd_1_47

# 4. Flash + monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

`idf.py` requires ESP-IDF **v5.3+** to be installed and `IDF_PATH` to
be set. The CI uses Espressif's official Docker image at v5.3 — see
`.github/workflows/build.yml` for the canonical version.

## Repository layout

| Path | What |
|------|------|
| `main/` | All C sources for the emulator firmware |
| `boards/` | Per-board profiles: `board.c` (HAL impl), `sdkconfig.board`, `partitions.csv` |
| `tests/` | Python BLE integration tests (require real hardware) |
| `docs/AUDIT_V3.md` | Reconstructed table of every audit finding, mapped to its fix commit |
| `docs/FUNCTIONAL_COVERAGE.md` | Emulator vs real Nivona app — gap list (G1–G6) |
| `CHANGELOG.md` | Per-release notes, tagged `emu-vX.Y.Z` |
| `VERSION` | Single source of truth for the firmware version |

## Branching & commits

- Branch from `main`. Naming: `feat/<short-name>`, `fix/<short-name>`,
  `chore/<short-name>`, `docs/<short-name>`, `ci/<short-name>`.
- Conventional-style commit subjects (`feat:`, `fix:`, `refactor:`,
  `docs:`, `chore:`, `ci:`, `perf:`, `test:`).
- One logical change per commit. Squash noise locally before pushing.
- Reference audit findings or `FUNCTIONAL_COVERAGE` gap IDs in commit
  messages when applicable.

## Pull request checklist

- [ ] Builds clean under CI (`.github/workflows/build.yml` green).
- [ ] If the change is observable on the BLE wire, it is reflected
      in `docs/FUNCTIONAL_COVERAGE.md` (status flipped if a G-item
      is closed, or new row + G7+ if a new gap is recognised).
- [ ] `CHANGELOG.md` updated with a one-line entry under the next
      pending release section.
- [ ] No `wifi_secrets.h` or real credentials in the diff.
- [ ] No decompile-source citations in code or comments (per parent
      project's CLAUDE.md convention — keep those in `docs/*.md`
      where they're explicit reference material).

## Audit-trail expectations

Every meaningful behaviour change is expected to land alongside a
small, factual code comment explaining *why* (not what) when the
reason is non-obvious — a hidden invariant, a real-machine quirk,
or a fix for a specific failure mode. Style guide: see existing
comments in `main/nivona_frame.c` and `main/nivona_brew.c` for the
flavour.

## Releasing

Maintainer-only. Outline:

1. Bump `VERSION` and add a `CHANGELOG.md` entry on a `feat/...`
   branch.
2. Merge → tag `emu-vX.Y.Z` on the merge commit → push tag.
3. `gh release create emu-vX.Y.Z` with the changelog entry as the
   release body.

## Reporting issues

- Bugs / feature requests: use the templates in
  `.github/ISSUE_TEMPLATE/` (the "New issue" picker on GitHub).
- Security vulnerabilities: private advisory only — see
  `SECURITY.md`.
