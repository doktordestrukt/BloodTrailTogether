# BLOODTRAIL TOGETHER — Phase 1 Feasibility Report

Date: 2026-11-21
Scope: read-only inspection of `E:\SteamLibrary\steamapps\common\Blood Trail` (Steam app 1032430, build 18598125). **No game files were modified.** Assets were extracted read-only from the PAK into a temporary scratch folder for name-table analysis.

Legend: **[VERIFIED]** = seen directly in the files. **[INFERRED]** = strong evidence but not observed at runtime. **[UNVERIFIED]** = must be tested.

---

## 1. Engine version

| Item | Finding | Evidence |
|---|---|---|
| Engine | **Unreal Engine 4.27.2** (`++UE4+Release-4.27-CL-18319896`) [VERIFIED] | Branch string in `BTVR-Win64-Shipping.exe`; `BTVR.uproject` `"EngineAssociation": "4.27"`; UE4SS log `Using engine version: 4.27` |
| Binary | Win64, monolithic **Shipping** build, 85.9 MB, unsigned [VERIFIED] | PE header, UE4SS `Build configuration: shipping` |
| DRM | **SteamStub DRM wrapper** (`.bind` PE section) [VERIFIED] | Section table. This is why UE4SS needs several scan retries at startup; it does eventually succeed |
| Project | `BTVR` (internal path `D:/BTVR_July2024Project/BTVR_Lite`), Electrovore LLC [VERIFIED] | `DefaultGame.ini`, debug strings |
| Native module | `/Script/BTVR` exists, but gameplay is overwhelmingly **Blueprint** [VERIFIED] | Module list; all player/mode/weapon/NPC logic lives in `.uasset` Blueprints |
| PAK | One PAK, **v11, unencrypted index, no compression, no IoStore** (25.6 GB, 30,744 files) [VERIFIED] | PAK footer; `bUseIoStore=False`, `bCompressed=False` |
| Launchers | `BTVR.exe` (UE bootstrap) and `BloodTrail_419ai.exe` (small stub that just `ShellExecute`s `BTVR.exe`; "BloodTrail_419ai" is the old project name, see `ActiveGameNameRedirects`) [VERIFIED] | Strings / config |

Third-party plugins compiled in: VRExpansionPlugin + OpenVRExpansionPlugin, SteamVR, OculusHMD, OpenXR, DragonIK, AICoverSystem, ObjectPool, DonMeshPainting, SkinnedDecalComponent, LowEntryExtStdLib, RMPP, Niagara, Chaos/PhysX.

### Already installed on this machine (not part of the Steam depot)
- **UE4SS v3.0.1 Beta** via `dwmapi.dll` proxy — working. Lua mods `RealisticBlood`, `RealisticGore`, console/cheat enablers are running and hooking game functions successfully.
- **ReShade 6.8.0** via `dxgi.dll` (+ FakeRTX/UltraReal presets).
- Empty `BTVR/Content/Paks/LogicMods/` (UE4SS BPModLoader target).

Our mod must coexist with both injectors.

---

## 2. How the VR player works

```
SteamVR_GM (GameMode, GlobalDefaultGameMode)
 └─ PlayerControllerClass = Steam_VR_Player_Controller
Pawn (placed in level / spawned by mode):
 BP_SurvivalCharacter / BP_DefaultPlayerCharacter
   └─ BP_VRCharacter          (/Game/WeaponMaster/Blueprints/Basic, 114 KB BP — the real player)
        └─ MotionControllerPawn  (/Game/WeaponMaster/VirtualRealityBP — parent is plain engine APawn)
             components: VROrigin (scene root for tracking space), Camera (HMD),
                         MotionControllerComponent ×2 → child actors BP_MotionController (hands),
                         HeadCapsule, Capsule, BP_WristWatch, holsters, ALS body (ALS_Player_CharacterBPai)
```

Key points:
- **The player pawn is NOT a VRExpansion `AVRCharacter`.** It is the classic UE4 VR template rig (`APawn` + `MotionControllerComponent`, `/Script/HeadMountedDisplay`, `/Script/SteamVR`). VRExpansion is used for menus, buttons, grips and the ALS full-body/NPC side. [VERIFIED by import tables]
- Tracking runtime: SteamVR plugin (OpenVR 1.5.17) with SteamVR Input bindings for Index/Touch/Vive/WMR; OpenXR loader is present but SteamVR is the configured path (`vr.SteamVR.EnableVRInput=1`). [VERIFIED]
- Tracking space: `VROrigin` scene component; HMD and controllers are relative to it. Remote avatars therefore must be built from **world-space** transforms `VROrigin * local pose`, never raw camera data. [INFERRED — confirm offsets at runtime with UE4SS live view]
- Height/turn/movement settings exist in the menu (HeightPlus/Minus/Auto, TurnMethod, MovementDirection, HandType). Player height scaling affects VROrigin → must be captured. [VERIFIED names]
- Hands: `BP_VRCharacterHand`, `BP_MotionController`; grips use both the WeaponMaster pickup interface and VRE grip components. [VERIFIED names]

---

## 3. The game modes (names taken from the game's menu Blueprint)

The hub map is **`Map_Beta_Basement`** (`GameDefaultMap`). The menu actor `MainMenuBt` has two top-level play buttons, **`Btnsandbox`** and **`Btnarcade`**, plus Story (landing page only), How-To-Play and Settings pages. [VERIFIED names]

| Mode (menu) | Maps | Logic |
|---|---|---|
| **Sandbox** | `Map_Sandbox`, `Map_Sandbox_Nu_OPT`, `Map_FuturisticSandbox`, `Map_Fy_World`, `Map_Doppleganger` | Free play, `BP_DugVrMenu` spawn menu, `Bp_NpcSpawner` |
| **Arcade** | Raid: `Map_Raid_lvl1`–`lvl9`; Arenas: `Map_CompoundArena`, `Map_SlumArena`, `Map_Homewrecker_Arena`, `Map_Arena_Jumper`, `Map_Beta_WoodsArena`, `Map_Beta_Apartment` | Raid buildings/outposts/compass waypoints (`BP_RaidBLDG_Base`, `BP_RaidOutpost`), wave spawning (`BP_WaveSpawner`, `BP_SurvivalGameMode` with `CurrentZombiesInLevel/MaxZombiesInLevel`), `BP_AiCoordinator`, `Bp_NpcSpawner` |

- No map overrides the GameMode by import, so all maps appear to run under `SteamVR_GM`; per-mode logic lives in level Blueprints and spawner/manager actors rather than in distinct GameMode classes. [INFERRED — WorldSettings properties still need to be read at runtime]
- Transitions: hub uses overlap triggers (`Level_Trigger_SANDBOX`, `Level_Trigger_Raid_Lvl_1…10`, `…_Compound`, `…_FOREST`, `…_HOMEWRECKER` …) and `OpenLevel`; the wristwatch returns to `Map_Beta_Basement`. All transitions are **hard `OpenLevel` loads**. [VERIFIED names]
- Whether "Arcade" is one mode containing both Raid and Arenas, or whether the arenas are a sub-page, must be confirmed in-headset. That is the first thing to check in Phase 3.

---

## 4. Suitable mod loader / extension method

**Recommended: UE4SS (already installed and confirmed working) with three layers:**

1. **UE4SS C++ mod (`Mods/BloodTrailTogether/dlls/main.dll`)** — transport, Relay client, HTTPS auth, threading, diagnostics. Lua cannot open sockets, and C++ gives us a background network thread so the game thread never blocks.
2. **UE4SS Lua (or C++) gameplay glue** — hooks via `RegisterHook`/`NotifyOnNewObject` on `BP_VRCharacter`, `OpenLevel`, weapon fire, NPC damage, etc. Same technique your `RealisticGore` mod already uses.
3. **Logic-mod PAK (`Content/Paks/LogicMods/BloodTrailTogether.pak`)** built in a **UE 4.27 editor** — the remote-avatar Blueprint, avatar meshes/materials and the UMG/3D-widget menu. Loaded by UE4SS BPModLoader. No game files replaced.

Rejected:
- Patching the exe / Steamless — forbidden and unnecessary.
- **UE's built-in replication (NetDriver)** — not usable: (a) the game's Blueprints contain no replication or RPC logic (single-player authority everywhere); (b) adding a custom Relay NetDriver would require compiling engine modules into a monolithic shipping exe, which is not possible from outside. → We need our **own lightweight state-sync protocol**.
- Replacing game assets in the base PAK — unnecessary and breaks on updates.

---

## 5. Unity Relay feasibility

**Verdict: technically feasible from a C++ UE4SS mod.** Unity documents the Relay wire protocol specifically for non-Unity engines and says they must implement it themselves.

What has to be implemented (from Unity's Relay message-protocol page):
- UDP messages, big-endian, header `0xDA72` + version `0` + type byte. Types: BIND 0, BIND_RECEIVED 1, PING 2, CONNECT_REQUEST 3, ACCEPTED 6, DISCONNECT 9, RELAY 10, CLOSE 11, ERROR 12.
- BIND carries the encrypted connection data (≤255 B) and a 32-byte HMAC over the preceding bytes, signed with the allocation's secret key. *The page does not name the hash algorithm — HMAC-SHA256 is the obvious candidate given 32 bytes; must be confirmed against Unity Transport's source or by testing.*
- RELAY payload ≤ **1400 bytes** → our packets must fit (they easily do).
- Relay disconnects after **10 s** of inactivity; PING every 1–2 s. Host must BIND within **60 s** of allocating.
- Errors include ErrTimeout, ErrUnauthorized, ErrNotConnected, etc. → maps directly onto the required connection-status UI.

Control plane (HTTPS, from the mod via WinHTTP):
- Unity Authentication **anonymous sign-in** using only the public **Project ID** → JWT. No secret ships in the mod.
- Relay Allocations REST: create allocation (max_players = 1 joiner), create **join code**, join with join code. *Exact endpoint paths/response fields are on Unity's external API reference and still need confirming in Phase 2.*
- Players never see each other's IPs; no port forwarding/VPN (outbound UDP to Relay only).

Open items / risks:
- DTLS: whether Relay accepts plain UDP for third-party clients or requires DTLS was **not confirmed** — if DTLS is mandatory, we need a DTLS library (e.g. mbedTLS), which is more work but still feasible.
- You need a **Unity account + Unity Cloud project with Relay enabled** (I can't create that for you).
- Pricing (check the live page): free tier listed as 50 average monthly CCU and 3 GiB Relay bandwidth per CCU, up to 150 GiB/month. Two players at ~20–40 kbit/s each is far below that.
- Both players are bound to the same Unity project/environment — fine, since the mod ships one Project ID.

**Fallback — not implemented:** Steam Datagram Relay / Steam Networking Messages. Note: the game's config lists a Steam NetDriver, but **OnlineSubsystemSteam is not compiled into the exe and no `steam_api64.dll` ships**, so this would also need us to bring our own Steamworks integration. It would work without a third-party account and with Steam friend invites instead of codes. Your approval is needed before any work on this.

The transport sits behind an `ITransport` interface (Loopback / UnityRelay / future SDR), so changing it later won't touch gameplay code.

---

## 6. Can two PCVR players share the same world?

**Yes, with an important qualification.** Each player runs their own full copy of the game. What we can realistically make shared:

| Tier | What | Feasibility |
|---|---|---|
| 1 | Remote avatar (head + both hands, name tag), same map, host-driven map/mode changes | **High** |
| 2 | Shared events: gunshots/impacts/explosions with positional audio, doors/buttons, spawned items, raid objective progress | **Medium** — per-event hooks |
| 3 | NPCs consistent on both machines (host simulates AI, client shows puppets; client hits forwarded to host) | **Hard** — the largest risk in the project |
| — | Full physics/ragdoll/gore replication | **Not planned** (unreliable and too expensive) |

Without Tier 3, each player fights their own NPCs — that isn't real co-op. Tier 3 is achievable in principle (suppress AI controllers on the client, drive NPC transforms/anim state from host snapshots, forward damage to host), but it depends on how `BP_NPC`/`BP_AiCoordinator` behave once we can observe them live.

---

## 7. Systems that will be difficult to synchronise

1. **NPC AI** (`BP_NPC`, `BP_AiCoordinator`, AICoverSystem, EQS spawn points, ALS + DragonIK + physical animation): non-deterministic, heavy, not built for replication.
2. **Randomness**: weapons and spawners use `RandomFloat`, `RandomInteger`, `RandomBool`, unseeded streams → host must decide and broadcast outcomes.
3. **Physics weapons/melee and grips** (WeaponMaster + VRE grips): local-only; we only share "held by remote player" visuals and resulting events.
4. **Gore/decals/mesh painting** (DonMeshPainting, SkinnedDecal, pooled decals): replicate the hit event, let each side render it cosmetically.
5. **Level transitions**: hard `OpenLevel`; the network layer has to survive the load (it lives in the DLL, outside UObjects) and resync after load.
6. **Raid/wave state**: kept in Blueprint variables (`CurrentZombiesInLevel`, `WaveScoreDeath`, `UpdateRaidBldgsRemaining`) → host authoritative, mirrored to client.
7. **Audio**: ~2,100 audio assets; sounds fire via `PlaySoundAtLocation`/`SpawnSoundAttached` in many BPs → hook at the call sites we care about and send `(soundAssetId, location, volume, pitch)`. Prevent duplicates by tagging locally-originated events.
8. **Game updates**: UE4SS signatures/Blueprint names may change with patches.

---

## 8. Dependencies

| Dependency | Purpose | Status |
|---|---|---|
| UE4SS 3.0.x | Loader, Lua, C++ mod API, BPModLoader | Installed |
| RE-UE4SS C++ source/SDK matching the installed version + Visual Studio 2022 (MSVC, C++20) | Build `main.dll` | Needed |
| Unreal Engine **4.27** editor (Epic launcher) | Build the LogicMod PAK (avatar BP, UMG) | Needed |
| UE4SS UHT/CXX header dump of the game | Exact class layouts / BP function signatures | Generate in-game (UE4SS keybind), no install changes |
| Unity account + project with Relay + Anonymous Auth enabled | Relay service | **Your action** |
| WinHTTP + BCrypt (Windows built-in) | HTTPS + HMAC-SHA256 | Built-in |
| Small JSON lib (e.g. nlohmann/json, header-only) | REST responses | Vendored |
| mbedTLS (only if DTLS is required) | DTLS to Relay | Conditional |
| FModel / UAssetGUI (optional) | Deeper Blueprint inspection | Optional |

---

## 9. What can be tested without a second VR player

- **Loopback transport** inside one game: the mod sends its own pose to itself through the full serialize → jitter buffer → interpolate path, and the "remote" avatar mirrors you with configurable offset, latency and loss. This tests ~80% of avatar sync with no network.
- **Headless simulated peer** (`BTT_SimPeer.exe`, console app): uses the **same protocol and real Unity Relay**, creates/joins sessions with codes, sends scripted VR tracking (walk loop, waving hands, head look) and shared events, and logs what it receives. This is a real Relay client, not a fake — it exercises auth, allocation, join code, BIND, keep-alive, disconnect and reconnect.
- **Network impairment layer** (latency, jitter, loss, duplication, reordering) between transport and session, toggled from config / UE4SS console.
- **Packet inspector**: binary log of every packet + a decoder tool; live overlay in UE4SS's GUI.
- **Two game instances on one PC: [UNVERIFIED, likely limited]**. SteamVR allows only one scene application at a time, so the second instance would have to run flat with `-nohmd`. The BP rig may misbehave without an HMD, and SteamStub/Steam may refuse a second instance. Worth a single test, but not something to rely on.
- **Not testable alone**: real two-person comfort, avatar scale perception, two-headset performance. These need one real session before release.

---

## 10. Recommended architecture

```
 ┌────────────────────────── Game process (BTVR-Win64-Shipping.exe, unmodified) ──────────────────────────┐
 │  LogicMods/BloodTrailTogether.pak      (UE 4.27-built)                                                  │
 │    BP_BTT_RemoteAvatar (head/hands meshes, name tag, quality icon)   WBP_BTT_Menu / 3D wrist panel      │
 │                ▲ spawn/update via UE4SS reflection                                                       │
 │  UE4SS C++ mod  BloodTrailTogether.dll                                                                   │
 │   Game thread ───────────────────────────────────────────────────────────────────────────────────────   │
 │   • VrTrackingSampler   reads VROrigin/Camera/MotionControllers → world poses (90 Hz local)             │
 │   • AvatarSync          send 30 Hz, jitter buffer ~100 ms, Hermite interp + short extrapolation         │
 │   • ModeSync            host OpenLevel → client follows; load barrier/resync                             │
 │   • EventSync           audio / interaction / damage events (reliable or unreliable channel, IDs, dedupe)│
 │   • WorldStateSync      host-authoritative registry of chosen actors (doors, pickups, raid state, NPCs) │
 │   • UI bridge           menu, status, notifications                                                      │
 │   ─────────── lock-free queues ───────────                                                               │
 │   Net thread                                                                                            │
 │   • Session (handshake, versions, heartbeats, RTT/loss stats, reconnect)                                │
 │   • Channels: unreliable-sequenced (poses) / reliable-ordered (events) — small custom ARQ               │
 │   • ITransport ── LoopbackTransport | UnityRelayTransport | (future SdrTransport)                        │
 │                     └ RelayAuth (anon sign-in, token refresh) + RelayAllocator (alloc/joincode/join)     │
 │   • Impairment layer, PacketLog, Diagnostics                                                             │
 └──────────────────────────────────────────────────────────────────────────────────────────────────────────┘
                     UDP ⇄ Unity Relay server ⇄ UDP          HTTPS → Unity Auth / Relay Allocations
```

Host-authoritative model: host owns the map/mode, NPCs, raid/wave state and world objects; each player owns their own body pose and local grabs. Bandwidth estimate for poses: 3 transforms × (quantised pos 6 B + smallest-three quat 4–7 B) + header ≈ 50–60 B/packet × 30 Hz ≈ 15 kbit/s per direction.

Project layout (to be created in Phase 3):
```
BloodTrailTogether/
  docs/            PHASE1_FEASIBILITY.md, DEVLOG.md, ARCHITECTURE.md
  src/net/         Transport, Session, Channels, Serialization
  src/relay/       RelayAuth, RelayAllocator, RelayProtocol, UnityRelayTransport
  src/vr/          VrTrackingSampler, PoseCodec
  src/sync/        AvatarSync, ModeSync, EventSync, WorldStateSync, AudioEvents
  src/ui/          UiBridge
  src/core/        Config, Log, Diagnostics, Impairment
  ue4ss_mod/       UE4SS mod wrapper + Lua glue
  unreal/          UE 4.27 LogicMod project (avatar, UI)
  tools/           SimPeer, PacketDecoder, install/backup scripts
  tests/           unit tests (codec, ARQ, interp, relay message encode/decode)
```

---

## Major limitations (summary)

- Real co-op against the **same** enemies (Tier 3) is the hardest part and isn't guaranteed. Avatars, map sync and shared audio/events are realistic.
- The Unity Relay control plane endpoints, HMAC algorithm and DTLS requirement still need confirming. The protocol itself is documented for non-Unity engines.
- A Unity account/project is required and is your step.
- Game patches can break hooks. Everything is keyed on Blueprint names, with version checks.

## Next step proposal (Phase 2 + start of Phase 3)

1. In-headset runtime pass with UE4SS: dump the object/UHT headers, confirm the two menu modes, VROrigin/camera/hand transforms, and spawn flow per map. (Read-only; files written under `ue4ss/` only.)
2. Confirm Relay REST endpoints, HMAC algorithm and DTLS requirement; you create the Unity project.
3. Write the Phase 2 design docs, then build the loopback avatar prototype.
