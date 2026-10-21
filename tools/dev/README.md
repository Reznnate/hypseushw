# Developer test harnesses

Scripts used while developing the core.  They are not needed to build or use
it, and they assume a development machine: Windows with MSYS2 (bash, coreutils)
for the shell scripts, `adb` for the Android ones, and Python 3 with Pillow and
NumPy for the image tools.  Expect to read a script before running it.

None of them writes next to game data: frame dumps, logs and saves go under
`$OUT` (default: a `sweep-*` folder in the repository root, which is ignored).

| Script | What it does |
|---|---|
| `sweep-core.sh` | Runs Daphne games headless through the Windows core with `lr-host`, with scripted button presses; per game a log, frame dumps and a contact sheet. `DATA=` the Daphne folder. |
| `sweep-singe.sh` | The same for Singe games, in real time. `DATA=` the Daphne folder; titles are framefile names. |
| `shield-sweep.sh` | The same on an Android device over `adb`, with `lr-host-cli` and the Android core. `DATA=` the Daphne folder as the device sees it. |
| `sweep-hypseus.ps1` | Baseline sweep of standalone Hypseus (window screenshot, audio RMS, errors) to compare the core against. |
| `compare-dumps.py` | Mean pixel difference between the frame dumps of two runs. |
| `decoder-table.py` | Side-by-side table of two sweeps' decoder timings (e.g. hardware vs software). |
| `contact.py` | Contact sheet from a folder of frame dumps. |
| `hwseek/` | Android tool timing exact-frame searches with libmpeg2 vs MediaCodec. |
| `ra-launch.sh` | Starts RetroArch on Android with the core and a content path, the way a launcher does. |
| `shield.sh` | Sends Android key events over `adb` and saves a screenshot. |
