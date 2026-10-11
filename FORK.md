# About this fork

This is a fork of [Forbes-Automotive/OpenHaldex-C6](https://github.com/Forbes-Automotive/OpenHaldex-C6),
MIT-licensed, maintained by [louij2](https://github.com/louij2). Upstream's
copyright is preserved in `LICENSE.md`; the hardware and the great majority of
the firmware are Forbes Automotive's work.

It exists because four fixes and features sat as open pull requests upstream
with no response, and because two release-process faults were shipping to users.
Everything here is offered back upstream — if upstream merges it, this fork has
done its job.

## What differs from upstream

- **The OTA updater points at this repo** (`UPD_REPO` in `data/app.js`). A
  device running this firmware is offered this fork's releases. Point
  `UPD_REPO` back at `Forbes-Automotive/OpenHaldex-C6` to track upstream.
- **Versions use the middle field**: `9.10.0`, `9.11.0` for features, `9.10.1`
  for fixes. Upstream numbers releases `X.00.Y`, so ours sort above the
  upstream cut they branched from and cannot collide. Digits and dots only —
  the updater's folder regex rejects any suffix.
- **Releases are built in CI from a tag**, so the binaries and the index that
  describes them come out of one run.
- The four carried-over PRs: CLI legacy backup key, favicon + PWA branding,
  vehicle geometry settings, and the OTA "Latest build" channel.

## Release process

Releases are never built by hand. To cut one:

1. Bump `FW_VERSION` in `include/OpenHaldexC6_ver.h` **and** `"fs"` in
   `data/version.json` to the same value. Add a changelog entry in
   `OpenHaldexC6_ver.h` below the fork-point marker.
2. Commit and push to `main`.
3. Tag it: `git tag v9.10.0 && git push origin v9.10.0`

`.github/workflows/release.yml` then builds the firmware and filesystem,
generates `Releases/releases.json` from the binaries it just built, verifies
the two agree, commits `Releases/` back to `main` and publishes a GitHub
release. The tag, `FW_VERSION` and `data/version.json` must all match or the
run fails deliberately — a mismatch there is how a release gets mislabelled.

To build a release locally instead (needs PlatformIO):

```sh
pio run -e esp32c6
pio run -e esp32c6 -t buildfs
python3 tools/make_release.py add 9.10.0 \
    --firmware   .pio/build/esp32c6/firmware.bin \
    --filesystem .pio/build/esp32c6/littlefs.bin \
    --bootloader .pio/build/esp32c6/bootloader.bin \
    --partitions .pio/build/esp32c6/partitions.bin \
    --notes "..."
python3 tools/make_release.py verify
```

## The release-index problem this fixes

The web updater lists releases from `Releases/releases.json` and reports the
indexed size while downloading. Upstream had no committed release tooling — the
index was produced by a local script (`make_release.py`, referenced in the
9.00.0 changelog but never committed) that was not always re-run after a
rebuild. Verified against upstream `main` on 2026-10-01:

| Release | Indexed | Actual | Verdict |
|---|---|---|---|
| 8.00.1 | matches | matches | fine |
| 8.00.2 | matches | matches | fine |
| 8.00.3 | 1 258 688 B | 1 258 880 B | index wrong by 192 B |
| 9.00.0 | 1 337 792 B | 1 346 368 B | firmware re-uploaded after indexing |

The two cases are different and worth keeping straight:

- **8.00.3 was never touched.** Its `firmware.bin` has exactly one commit
  (`ee1c2f1`, 2026-07-09) and that blob appears in no other commit.
  `releases.json` was first written on 2026-09-20, two months later, and
  back-filled the older releases — getting 8.00.1 and 8.00.2 right and 8.00.3
  wrong. The binary is sound; the index entry was never correct.
- **9.00.0 genuinely was rebuilt in place.** Its `firmware.bin` has nine
  commits, eight on 2026-09-20 and one on **2026-09-30** — after the index was
  last written. So "V9.0.0" names more than one binary, and you cannot tell
  which one a unit is running without reading `/ota/info`.

Upstream's response to the symptom was to remove the OTA upload-stream SHA-256
gate in 9.00.0 ("the gate only ever failed updates when a .bin was rebuilt in
place without re-running make_release.py"), which removed the detection rather
than the drift.

Here, `tools/make_release.py verify` runs in CI on every push and fails the
build if the index and the binaries disagree. Both inherited entries have been
corrected with `make_release.py reindex`, which records what the files *are* —
it cannot recover what a stale entry was meant to describe.

## Open work

Gen5 0AY (generation 51) lock behaviour is under investigation — a car reported
staying FWD in every forced mode. Candidate causes are tracked on a branch and
deliberately **not** in a release: they change how a real car's AWD engages, so
they need a road test before they ship to anyone. See `docs/gen51-notes.md`.
