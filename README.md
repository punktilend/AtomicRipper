# AtomicRipper

A modern, open-source CD ripper for Windows — built to replace EAC.

Secure multi-pass ripping, AccurateRip verification, MusicBrainz metadata, FLAC/WAV output, cover art, cue sheets. Pure C++20, no legacy cruft.

---

## Features

| Feature | Status |
|---|---|
| Secure multi-pass ripping with configurable retry | ✅ |
| C2 error pointer detection | ✅ |
| AccurateRip V1 + V2 verification | ✅ |
| Drive read-offset correction | ✅ |
| Automatic drive offset detection (AccurateRip sweep) | ✅ |
| MusicBrainz metadata lookup | ✅ |
| Cover art download + FLAC embedding (Cover Art Archive) | ✅ |
| FLAC encoding (libFLAC, compression 0–8) | ✅ |
| WAV encoding | ✅ |
| TagLib tagging (Vorbis Comment) | ✅ |
| Cue sheet generation (.cue, CRLF, per-track and single-file) | ✅ |
| Single-file FLAC with embedded CUESHEET block | ✅ |
| Disc eject on completion | ✅ |
| Qt 6 GUI | 🔜 |
| Re-rip suspect sectors | 🔜 |
| Linux / macOS support | 🔜 |

---

## Requirements

- Windows 10 or later (x64)
- [Visual Studio 2022 Community](https://visualstudio.microsoft.com/vs/community/) with the **Desktop development with C++** workload
- [vcpkg](https://github.com/microsoft/vcpkg) — clone anywhere, set `VCPKG_ROOT`
- CMake 3.25+ (bundled with VS 2022)

---

## Building

```powershell
# 1. Clone
git clone https://github.com/punktilend/AtomicRipper.git
cd AtomicRipper

# 2. Configure (vcpkg installs all dependencies automatically)
cmake --preset windows-msvc-x64

# 3. Build
cmake --build build --config Release

# Output:
#   build\bin\Release\atomicripper.exe
#   build\bin\Release\atomicripper-tests.exe
```

Dependencies installed automatically via vcpkg manifest (`vcpkg.json`):
`libflac` · `cpr` · `nlohmann-json` · `taglib` · `catch2`

---

## Usage

```
atomicripper.exe [options]
```

| Command | Description |
|---|---|
| *(no args)* | List all optical drives |
| `--list-drives` | Same as above |
| `--toc D:` | Read and display the disc TOC + MusicBrainz Disc ID |
| `--rip D:` | Rip all audio tracks to FLAC in the current directory |
| `--rip D: "C:\Music\MyDisc"` | Rip to a specific output directory |
| `--rip D: --wav` | Rip to WAV instead of FLAC |
| `--rip D: --offset 30` | Apply a +30 sample drive read offset |
| `--rip D: --detect-offset` | Auto-detect drive offset via AccurateRip sweep |
| `--rip D: --single-file` | Encode all tracks to one FLAC with embedded cue sheet |
| `--rip D: --eject` | Eject the disc when the rip completes |

### Example session

```
> atomicripper.exe --rip D: "C:\Music\PinkFloyd\DSOTM"

AtomicRipper v0.7.0
===================

Drive  : D:
Disc   : 10 tracks, 2626 sec
Format : FLAC
Output : C:\Music\PinkFloyd\DSOTM

MusicBrainz lookup found 3 release(s):

  [1] The Dark Side of the Moon — Pink Floyd (1973) [US]
  [2] The Dark Side of the Moon — Pink Floyd (1973) [UK]
  [3] The Dark Side of the Moon — Pink Floyd (2011) [EU]

  Select [1-3] (Enter = #1): 1

  Using: The Dark Side of the Moon — Pink Floyd

Ripping track 01/10  (17800 sectors, 237.3 sec)
  [============================] 100%  4.2x  0 retries
  CRC32: A1B2C3D4  |  rip: OK  |  C2: 0 sector(s)
  -> C:\Music\PinkFloyd\DSOTM\01 - Speak to Me.flac

...

AccurateRip verification...
  DB pressings found: 42
  Track 01: OK       conf=38  CRCv1=A1B2C3D4  CRCv2=E5F67890
  ...
  10/10 tracks verified accurately

  10 track(s) tagged.

Done.
```

---

## How it works

**Rip loop** — each track is read sector by sector using Windows SCSI pass-through (`IOCTL_SCSI_PASS_THROUGH_DIRECT`). In secure mode, each sector is re-read until at least `minMatches` consecutive identical reads agree, or `maxRetries` is exhausted. C2 error pointers from the drive are used as a tiebreaker.

**AccurateRip** — after ripping, AtomicRipper fetches the AccurateRip database entry for the disc and computes CRC V1 and V2 checksums against every pressing on record. A confident match (conf > 0) means your rip is bit-perfect.

**Offset detection** — if no tracks match at offset 0, AtomicRipper can sweep ±1176 samples (the maximum standard drive offset) using an O(N+K) sliding-window algorithm to find your drive's read offset automatically.

**Pipeline** — the full rip/encode/verify/tag sequence runs in a background thread. The `Pipeline` class exposes typed callbacks so that any UI (CLI today, Qt tomorrow) can observe every state transition without polling.

---

## Architecture

```
AtomicRipper/
├── core/
│   ├── drive/          # Drive enumeration, TOC reading (IOCTL, overlapped + timeouts)
│   ├── rip/            # RipEngine — SCSI passthrough, secure multi-pass
│   ├── encode/         # FlacEncoder, WavEncoder, IEncoder interface
│   ├── metadata/       # DiscId, MusicBrainz, TagWriter, CoverArt, CueSheet
│   ├── verify/         # AccurateRip checksums, HTTP fetch, offset detection
│   └── pipeline/       # Pipeline — async orchestration of all the above
├── cli/                # Console front-end (main.cpp)
├── tests/              # Catch2 unit tests (29 tests, all passing)
└── CMakeLists.txt
```

---

## Running tests

```powershell
cmake --build build --config Release --target atomicripper-tests
.\build\bin\Release\atomicripper-tests.exe
```

```
All tests passed (1301 assertions in 29 test cases)
```

Tests cover: TOC validity, MusicBrainz Disc ID calculation, AccurateRip checksum algorithms and disc ID construction, cue sheet generation (per-track and single-file modes), FLAC encoder encode/tag/cuesheet/error-handling, and `cdBytesToSamples` byte-order correctness.

---

## Roadmap

- [ ] **Re-rip suspect sectors** — targeted retry of sectors that couldn't reach read consensus
- [ ] **Qt 6 GUI** — drive selector, track list, real-time progress bars, release picker
- [ ] **Linux support** — libcdio drive layer, `/dev/sr*` enumeration
- [ ] **macOS support** — IOKit drive layer
- [ ] **HTOA** — hidden track one audio detection and optional rip as track 00
- [ ] **ReplayGain** — track + album gain calculation and tagging

---

## License

MIT
