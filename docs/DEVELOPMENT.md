# Development

## Build
Requirements: Visual Studio 2022 with "Desktop development with C++", Python 3.
```
build.cmd            developer build  -> %LOCALAPPDATA%\BTTBuild\dev
build.cmd release    public build (developer UI compiled out) -> %LOCALAPPDATA%\BTTBuild\release
```
Build trees live under `%LOCALAPPDATA%\BTTBuild` because MSVC still trips over the 260-character path limit.

## Tests (no game, no Unity account needed)
| Command | Covers |
|---|---|
| `btt_tests.exe` | base64/JSON/HMAC (RFC 4231), Relay wire format incl. an independently computed BIND HMAC, allocation JSON parsing, pose codec error, interpolation error, sessions under 0–25 % loss and 0–250 ms latency, interruption/recovery, mock Unity Relay server end-to-end incl. re-join, engine + simulated player in real time |
| `btt_bridge_probe.exe <btt_core.dll> [s]` | loads the DLL like the Lua mod, talks over the real named pipes with C stdio, checks handshake, simulated player pose stream and sound echo, measures round-trip time |
| `python -I tests/installer_test.py <setup.exe> <workdir> [<real Win64 to copy UE4SS from>]` | installer on fake game folders: refusal without UE4SS, existing UE4SS + other mods untouched, repair keeps settings, uninstall, installing/removing UE4SS, path-traversal rejection |
| `btt_simpeer.exe relay-host` / `relay-join CODE` | a real second player over Unity Relay (needs `--config` with a project id) |
| `btt_simpeer.exe udp-host` + game DEV "UDP join" | game ↔ SimPeer on one PC without Relay (developer builds) |

## Developer mode
Create `ue4ss\Mods\BloodTrailTogether\dev.ini` (never shipped, git-ignored):
```
[Dev]
Enabled=1
SimLatencyMs=80
SimJitterMs=15
SimLossPct=5
```
The window then shows latency/jitter/loss controls and direct-UDP buttons; logging becomes verbose.

## Release
1. Bump the version in `CMakeLists.txt`, `release.json`, `Scripts/main.lua`; update `CHANGELOG.md`.
2. Put the UE4SS zip named in `release.json` into `third_party/ue4ss/` and record its SHA-256.
3. `python -I tools/make_release.py` → `dist/<version>/` (Setup exe, manual zip, SHA256SUMS.txt).
4. Run `tests/installer_test.py` against the new Setup exe.
5. Tag `vX.Y.Z` and push; `.github/workflows/release.yml` rebuilds and attaches the files to a draft release.

Release builds contain only: `Scripts/main.lua`, `bin/btt_core.dll`, `config.ini` (public project id),
`enabled.txt`, licence files, and optionally UE4SS. `make_release.py` refuses PDBs, logs, `dev.ini`,
test tools and anything with a secret-like name.
