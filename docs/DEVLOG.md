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
