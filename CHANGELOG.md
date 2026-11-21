# Changelog

Versioning: [Semantic Versioning](https://semver.org). The version lives in `CMakeLists.txt`,
`release.json` and `Scripts/main.lua` (the release script refuses to build if they differ).
The network protocol version (`kProtocolVersion`) changes only when old and new clients can no longer talk.

## [0.6.1] - 2026-11-22
### Fixed
- Remote body: ALS velocity-direction mode while walking forward/sideways (legs follow movement, aim offset follows the look); looking-direction mode when standing or walking backwards.
- No hit-reaction physics on a living remote body (SetAllBodiesSimulatePhysics(false) + blend weight 0 each frame).
- Subtitles: bracketed line-type descriptions instead of invented sentences; filename words kept.
- Watch: wider tap and look tolerances, hologram closer to the wrist.
### Simulated player
- Mirrors held items (held/heldpart echoed with sides swapped) and grip flags; takes hits, dies, respawns after 6 s; echoed sounds come from the mirrored position.

## [0.6.0] - 2026-11-21
### Fixed
- Host crash with live AI NPCs (0.3.0-0.5.1): the pose-copy body is not used on the host while AI NPCs are present (game animation + look-at instead); pose mesh moved to its own holder actor.
- NPC fire detection by ammo polling instead of hooking Fire_pistol (hook crashed inside UE4SS).
### Added
- AI: hearing reports for the other player's sounds, ammo scavenging from bodies, flee/surrender when losing; also in single player.
- NPC subtitles (category captions, shadowed text, animated), NPC voice lines replayed for the joining player; watch toggle.
- Animated in-headset notes.

## [0.5.1] - 2026-11-21
### Fixed
- Untracked hands hang at the sides (IK to a relaxed target) instead of the NPC's clasped pose.
- Held-item meshes are never loaded from disk at runtime (UE4SS LoadAsset of meshes crashed the game); asset type checked before use; failed loads retried every 5 s, not every frame.
- Sound sharing forced on at start; window label "IN A GAME" for clients.
- SimPeer: --at X Y, --yaw, --no-hands; static meshes sent as S.

## [0.5.0] - 2026-11-21
### Added
- Finger poses on the other player's hands: wrapped round held items (index on the trigger), fist while gripping, relaxed otherwise. Grip-button state travels in pose flags 16/32.
- Muzzle flashes (game's BP_MuzzleFlash_Generic / _12ga) for every shared gunshot, including NPC shots aimed at you.
- Bullet impacts: the shooter's own bullets report SpawnHitEffect point/normal/surface; the other game spawns the matching BP_BHole_* effect.
- Watch hologram menu: tap your left wrist with your right hand; host, quick join, leave, name tags, health bars, bullet FX, share sounds. Settings saved in watch_settings.txt.

## [0.4.0] - 2026-11-21
### Added
- Server list (Unity Lobby): hosting lists your game publicly; others join with a click. Listings carry the Relay join code, map, mod version and protocol; heartbeat every 10 s, removed on join/close.
- Redesigned F9 window: custom-drawn with GDI+, animated (hover glow, pulsing status light, ping meter, sliding server rows, toasts).
- Players on different mod versions with the same network protocol can join each other; otherwise the row offers UPDATE (opens the download page) and a joiner gets a clear "update needed" message. A mismatched joiner no longer ends the host's session.
- NPCs attack both players: when an NPC on the host fires and the joining player is the closer target, the shot is replayed at them (gunshot from that NPC, NPC turns to them, distance-based hit chance).
- Health bar over the other player, wrist status panel (look at your raised left hand), weak-connection warning.
- Installer icon.

## [0.3.0] - 2026-11-21
### Fixed
- Remote body legs crossing/spinning: bodies get an AIController carrying the look rotation (ALS looking-direction mode) and the movement component's last-update transform is kept current (foot locking).
- Head + neck follow the headset; two-bone arm IK to each controller via a PoseableMesh copy.
- NPC hits replayed relative to the hit bone (no more floating blood); remote body never ragdolls while alive.
- Frame rate: cached player controller, NPC registry via NotifyOnNewObject; assets that fail to load are not retried every frame.
- Relay region chosen from the time zone (europe-west4 for Europe: ~60 ms instead of ~220 ms).
- Remote gunshots use ATT_gunshot attenuation (3D, distance falloff); other remote sounds without 3D settings use a short range.
### Added
- Protocol v3 (pose carries both held items). Held items are sent as multi-part meshes (`held` / `heldpart`), refreshed while held.
- Head lamp / night vision sync, NPC body drag + throw sync, map prop grab/throw sync.

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
