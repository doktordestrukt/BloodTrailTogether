# Changelog

Versioning: [Semantic Versioning](https://semver.org). The version lives in `CMakeLists.txt`,
`release.json` and `Scripts/main.lua` (the release script refuses to build if they differ).
The network protocol version (`kProtocolVersion`) changes only when old and new clients can no longer talk.

## [0.1.0] - 2026-11-21 (preview)
### Tested
- Live Unity Relay session (sign-in, allocation, join code, BIND, connect, data both ways).
- In game: remote avatar, map following into Sandbox and Raid, shared sound playback; avatar seen in VR.
### Known limitations
- UE4SS is not bundled: install it first.
- Remote player is shown with simple shapes; no player-vs-player damage, NPC/gore/ragdoll sync yet.
### Added
- Unity Relay transport implemented from the published wire protocol (BIND/HMAC-SHA256, PING,
  CONNECT_REQUEST/ACCEPTED, RELAY, DISCONNECT, CLOSE, error handling) plus anonymous Unity
  Authentication and the Relay allocation / join-code REST calls.
- Two-player session layer: handshake with version check, unreliable pose stream, reliable ordered
  events, RTT/loss/jitter statistics, interruption detection, host stays open for re-joins.
- Snapshot interpolation of head + both controllers (100 ms buffer, bounded extrapolation).
- Game bridge `btt_core.dll` + UE4SS Lua script: VR pose capture, remote avatar, name tag with ping,
  VR notifications, map following (host leads), shared positional sounds.
- Desktop multiplayer window (F9): create / join / disconnect, join code with copy, status, update notice.
- Simulated player (mirror / walk-around) for testing alone; headless `btt_simpeer` tool.
- Installer (GUI + command line): Steam library detection, build compatibility check, UE4SS
  detection/installation, settings-preserving repair, backups, uninstall.
- Update check against GitHub Releases (notification only, never installs by itself).
