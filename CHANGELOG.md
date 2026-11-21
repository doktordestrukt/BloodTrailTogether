# Changelog

Versioning: [Semantic Versioning](https://semver.org). The version lives in `CMakeLists.txt`,
`release.json` and `Scripts/main.lua` (the release script refuses to build if they differ).
The network protocol version (`kProtocolVersion`) changes only when old and new clients can no longer talk.

## [0.2.1] - 2026-11-21
### Fixed (from the first real two-player session)
- Remote body sank ~1 m: VRExpansion pawns have their actor at the floor (measured), not at capsule centre.
- NPC pistol always visible: hidden unless the player grips something (grip flags in pose); ALS overlay 0 when unarmed.
- PvP damage now a fraction of the victim's `playerhealthmax` (250): head 100 %, torso 50 %, limbs 34 %.
- Head look-at via the AnimBP's DragonIK `Look At Location` (an FTransform) + crouch via ALS Stance.
- Damage-table rows re-fetched each hit (stale after level reload).

## [0.2.0] - 2026-11-21
### Added
- Remote players appear as the game's full-body human NPC (AI disabled), walking with the player.
- Player-vs-player damage through the game's own bullet/gore pipeline; victim's game applies the damage;
  death ragdoll + respawn; melee counted only when your hand connects; knock-down recovery.
- Protocol v2 (body transform, generic game events). Not compatible with 0.1.x.
- Host-authoritative NPC sync (on by default, `[Gameplay] NpcSync`): host streams NPC position/yaw/speed/death
  ~10x/s; client removes its own NPCs and shows mirrors; hits from either side replayed via EventReceiveDamageNPC.

## [0.1.0] - 2026-11-21 (preview)
### Tested
- Live Unity Relay session (sign-in, allocation, join code, BIND, connect, data both ways).
- In game: remote avatar, map following into Sandbox and Raid, shared sound playback; avatar seen in VR.
### Known limitations
- Remote player is shown with simple shapes; no player-vs-player damage, NPC/gore/ragdoll sync yet.
### Added
- Installer bundles UE4SS v3.0.1-1181-g83330635 (the build proven on BloodTrail, SHA-256 pinned) and installs it only when missing.
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
