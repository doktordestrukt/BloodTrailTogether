# BLOODTRAIL TOGETHER — Development Log

## 2026-11-21 — Phase 1 investigation (read-only)
- Inventoried the Steam install (app 1032430, build 18598125). Nothing modified.
- Engine: UE 4.27.2 (CL-18319896), monolithic Win64 Shipping, SteamStub-wrapped (`.bind`).
- PAK v11, unencrypted, uncompressed; parsed the directory index (30,744 files) and extracted maps and core BPs to scratch for name-table analysis (custom read-only Python tools).
- UE4SS 3.0.1 (dwmapi proxy) already installed and working. ReShade 6.8 (dxgi) present.
- Player rig: BP_VRCharacter → MotionControllerPawn (APawn, UE VR template), SteamVR/OpenVR input. GameMode SteamVR_GM + Steam_VR_Player_Controller. Hub map Map_Beta_Basement.
- Menu modes: Sandbox and Arcade (Raid lvl1–9 + arenas).
- Gameplay BPs have no replication; OnlineSubsystemSteam not compiled in, no steam_api64.dll.
- Unity Relay: wire protocol is documented for non-Unity engines → feasible via C++ UE4SS mod. Open: REST endpoint details, HMAC algorithm, DTLS.
- Report: docs/PHASE1_FEASIBILITY.md

## 2026-11-21 — Build day 1: networking, Relay, game bridge, installer
### Decisions
- **No UE4SS C++ mod.** RE-UE4SS's C++ SDK needs the private `Re-UE4SS/UEPseudo` repo (GitHub
  account linked to Epic); this account has no access (verified: 404 on UEPseudo and EpicGames/UnrealEngine).
  Instead: a dependency-free native DLL (`btt_core.dll`) loaded from Lua with `package.loadlib(path, "*")`
  (UE4SS.dll contains `loadlib`), talking to Lua over two named pipes.
- The installed UE4SS is the experimental build `v3.0.1-1181-g83330635` (2026-10-17, 50 commits before
  v4.0.0). It is the version proven on BloodTrail, so it is the one pinned in `release.json`.
- Native C++ installer (no .NET / VC++ redistributable needed; static CRT).
- Build trees moved to `%LOCALAPPDATA%\BTTBuild` (MSVC failed on >260-char paths in the workspace).

### Unity Relay protocol verification
Docs page (relay-message-protocol) vs Unity Transport source (`needle-mirror/com.unity.transport`):
- docs say all fields big-endian; Unity's client writes BIND nonce and PING sequence little-endian,
  RELAY length big-endian. **Following the reference client.**
- BIND HMAC = HMAC-SHA256(64-byte key, `DA 72 00 00 00 nonce_lo nonce_hi FF` + 255-byte connection data).
- REST: `relay-allocations.services.api.unity.com/v1/{allocate,joincode,join}`; auth
  `player-auth.services.api.unity.com/v1/authentication/anonymous` with `ProjectId`, `UnityEnvironment` headers
  (from `com.unity.services.relay` / `.authentication` sources). JSON byte fields are base64.
- Not yet tested against the live service (needs the project id).

### Bugs found by tests and fixed
- Host session state was overwritten after the peer left (now: host stays open, same code, re-join works).
- RelayTransport re-opened its socket every tick until bound (5 s bind delay) -> now binds in ~15 ms.
- Session timestamps used wall clock while tests used simulated time -> session uses its update clock.
- Config loader ignored the first section when the file had a UTF-8 BOM.
- Bridge probe misread negative coordinates as "no pose" (probe bug, Lua unaffected).
- Themed checkboxes drew dark text on dark background.

### Test status (all automated, all passing)
- `btt_tests`: 337 checks (incl. sessions at 25 % loss / 250 ms, mock Relay end-to-end + re-join,
  engine with simulated player in real time: ~87 interpolated remote poses/s at 90 Hz requests).
- `btt_bridge_probe`: DLL over real pipes, ~50 µs average round trip, worst ~0.3 ms.
- `installer_test.py`: 25 checks on fake game folders (real install never touched).
- Installer `--detect` on this PC: found 5 Steam libraries, BloodTrail on E:, build 18598125 supported,
  existing UE4SS kept.

### NOT yet verified
- Anything inside the running game (main.lua): pose capture, avatar spawning, sound hooks, OpenLevel follow.
- Live Unity Relay (no project id yet).
- Installing UE4SS from the pinned zip (zip not downloaded yet; extraction logic tested with a fake UE4SS).

## 2026-11-21 — First in-game tests (real BloodTrail install, no headset, `-nohmd` desktop mode)
Installed with the real installer (`--install`), UE4SS kept. Verified afterwards that all 955 pre-existing
files in `BTVR\Binaries\Win64` were byte-identical (only ReShade later rewrote its own log).

### Crash found and fixed
- First run: game crashed ~1.3 s after the avatar spawned. UE4SS logged `Ref was not function`, crash
  report: EXCEPTION_ACCESS_VIOLATION inside UE4SS. Cause: the frame loop created a new
  `ExecuteInGameThread` closure from `LoopAsync` ~90x/s. Fix: one persistent
  `LoopInGameThreadAfterFrames(1, ...)` callback (available in this UE4SS build; old path kept as fallback).
  After the fix: several runs of 2-5 minutes without problems.

### Verified in the running game
- Lua loads `btt_core.dll` via `package.loadlib`, all three sound hooks register, bridge connects.
- Simulated player (mirror) joins; remote avatar (head sphere, visor, hand cubes, "SimBot 3ms" tag)
  visible in the hub (`Map_Beta_Basement`) - screenshot taken.
- Multiplayer window works inside the game process (status, ping, same-map indicator).
- **Map following, both modes**: SimPeer hosting on `Map_Sandbox` (Sandbox) and on `Map_Raid_lvl1`
  (Arcade/Raid) -> the game (client) loaded the host's map by itself within ~2 s, avatar re-created on the new map.
- Bridge at ~130 frames/s (desktop frame rate), remote pose available every frame, 6-12 ms RTT, 0 % loss
  over localhost UDP for 80+ s.
- **Shared audio playback**: 19/19 remote sound events (`Cue_ZombieMoan`) loaded and played at the
  sender's position. The native GameplayStatics hooks fire, and our own playback is not echoed back.

### Still unverified (needs the headset and/or a Unity project)
- Motion controller tracking (`hands=0` without a headset), avatar orientation/scale in VR.
- Capturing the local player's own gunshots (needs VR input).
- Unity Relay against the live service.

### Other fixes
- Log file is now opened with shared read access (could not be read while the game ran).
- Dev UDP transport binds to 127.0.0.1 (no Windows Firewall prompt).
- `dev.ini` `AutoStart=` (sim_mirror | sim_orbit | udp_host | udp_join) + 5-second diagnostic stats lines.
- SimPeer `--map` and `--sound` options; installer no longer logs each line twice.

## 2026-11-21 — Live Unity Relay verified
- Project `3a2b7ffb-8a1a-40da-be13-571dfac0220a` (public id) added to `release.json`.
- SimPeer host + SimPeer joiner through the real service: anonymous sign-in OK, allocation
  (us-central1), BIND accepted (confirms HMAC-SHA256 input + little-endian nonce from Unity's reference
  client), join code `77GDFN`, ACCEPTED, handshake < 1 s, maps/notices/sounds exchanged both ways.
- Found: two SimPeers echoed each other's sounds endlessly (test-tool bug); echoes are now tagged and never re-echoed.
- The user tested in VR: the simulated player's head, visor, hands and name tag render correctly in the headset.
