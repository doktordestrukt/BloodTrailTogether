# Synchronisation audit and status (started 2026-11-26)

This is the living status of BloodTrail Together's multiplayer sync. "Verified" means checked in the
running game (single PC: one real game + `btt_simpeer` as the second player) and/or unit tests. Nothing
here has been verified in a real two-headset session unless it says so.

## 1. What the project is

| Part | What it does |
|---|---|
| Game | BloodTrail (UE 4.27.2 shipping, PCVR). Player = `BP_VRCharacter`/`WendigoVrChar` (MotionControllerPawn, VRExpansion grip system). NPCs = `BP_NPC_Man` (ALS v4 locomotion, `NPC_AnimBP` with TwoBoneIK hand/foot nodes, DragonIK look-at, game gore via `EventReceiveDamageNPC` + damage-causer data table). |
| `btt_core.dll` (C++) | Unity Relay + Lobby transport, session layer (handshake, reliable ordered events, unreliable pose stream with interpolation, **new:** unreliable latest-wins state channel), F9 window, installer. Talks to Lua over a named pipe once per frame. |
| `main.lua` (UE4SS) | Reads the local VR pose and held items, drives the other player's body, replays hits/gore, NPC host/client sync, corpses, grabs, props, sounds, effects, character look. |
| `btt_simpeer` | Headless second player for tests (scripted pose, held gun, scripted NPC, sounds). |

No game files are modified. The mod runs entirely through UE4SS and the game's own Blueprint functions.
That is still the right choice: everything needed so far is reachable through reflection. Changing the
game's paks would make installs fragile and break with every game update.

## 2. Architecture assessment

**Sound:**
- **Transport:** pose stream (30 Hz, 100 ms interpolation buffer, extrapolation cap) plus a reliable ordered event channel with selective resend. Tested under 0-25 % loss and 0-250 ms latency.
- **Authority:** the host's game owns NPC AI, NPC health and deaths. Each player owns their own body and health; hits on a player are decided by the shooter's game and applied by the victim's game.
- **Hits:** NPC hits are replayed relative to the bone that was hit, so wounds land on the same spot.

**Weak points found (and fixed in this round unless marked):**

1. **High-rate data on the reliable channel.** NPC positions (~10 Hz per NPC), corpse positions, dragged bodies and thrown props were all reliable events. With packet loss, everything queued behind a lost packet: head-of-line blocking delayed hits and deaths and caused visible desync. **Fixed:** protocol 4 adds an unreliable latest-wins state channel; these streams moved there, and hits, deaths and drops stay reliable.
2. **NPC state was position + yaw only.**
   - Crouching NPCs stood up in the other game.
   - An NPC knocked down by a hit (alive, ragdolling) stood upright in the other game: this is the "guy on the floor for you but standing for me".
   - **Fixed:** stance and knocked-down state are synced, and a knocked-down NPC's ragdoll pose follows the host.
3. **Corpses followed one point (the pelvis).** The limbs settled differently on each screen. **Fixed:** the pose of 7 bodies (pelvis, chest, head, hands, feet) is synced, and the copy is velocity-driven so it keeps real physics.
4. **Remote player body** (fixed in 0.8.5):
   - The pose-copy mesh crashed the game. It is gone, and arms now use the NPC AnimBP's own hand IK.
   - The game's ragdoll and death state on the body caused fast spinning. It is now undone immediately.
5. **Held items** (fixed in 0.8.5): items whose model the other game had never loaded were invisible. The class is now sent and loaded, with an inert blueprint copy as a fallback.
6. **Third-party gore mods** (not fixable in code): the test PC runs `RealisticBlood` and `RealisticGore`. If only one player has them, gore will look different. **Both players need the same gore mods.**

## 3. Status

### Verified in game (simulated second player)
- Connection over Unity Relay and direct UDP (dev), map follow, version check.
- Remote player body: head look direction, both arms reaching the controllers via AnimBP IK (hands typically 0-15 cm from target), walking/strafing, crouch from head height. No crash in 200 s joined or hosted with AI NPCs (0.8.5).
- Held gun visible on both sides (mesh or blueprint copy).
- Shots on the other player: damage, no spin and no local death of the body (72 hits in 150 s).
- State channel (unit test): 60 keys at 30 Hz, final values delivered, no out-of-order values, up to 25 % loss.

- NPC sync over the state channel, joining side, scripted host NPC: walking copy 0-4 cm from the host position; crouch applied both ways; knocked-down copy ragdolls (pelvis 9-11 cm) and gets up again; death -> ragdoll.
- Corpse pose with a scripted (not real) pose: all 11 bones within 6-27 cm. (Correction: the 'recorded real pose' run actually used the scripted pose - the test tool split the argument.)
- **Two real game clients on one PC** (copy 1 hosts, copy 2 joins, real session code): both see each other's body with arms 2 cm from target; a real NPC corpse followed with every one of 11 bones within 2-5 cm.
- Host side: real AI NPC killed by a headshot -> corpse pose streamed (about 70 updates/s while moving, none at rest).

- Weapon moving parts, receiving side: a two-part held item whose second part moves like a slide shows at exactly the rest (10.0 cm) and back (11.7 cm) offsets as updates arrive.
- Desktop player sending: knife and bat equipped with the desktop mod are detected and sent (class, item and parts); gunshots and shell-casing sounds arrive with positions.
- Late join: held state, corpses and moved props are re-sent when a player connects.

- World blood: the host detected 4 blood decals from headshots and sent them (class, transform, random roll and scale).
- Dropped item: the other player's dropped Glock appears, falls to the floor (z 5 cm) and is removed when picked up.
- Held item on the hand: 0 cm off its grip while the hand misses an out-of-reach controller by up to 23 cm.

### Implemented, still to verify in game
- Blood on the joining side (spawning the host's decal and removing its own): the receive test did not run because of the tool bug above.
- Dragged bodies and thrown props over the state channel: the code path is unchanged apart from the channel. A VR grab is needed to test it.
- The other player's body was once destroyed by the game while an NPC copy got up (seen 1 of 4 runs). It respawned the next frame; cause unknown.

### Known gaps / outstanding
- **Fingers:** fingers don't curl on the other player's hands. The head follows the look direction, not exact headset tilt.
- **Weapon animations:** slide, magazine and reload animations of the other player's gun are not synced, only the gun's position. Firing is synced as sound, muzzle flash and impacts.
- **Spawned objects:** objects spawned at runtime (dropped magazines, casings, spawned guns) are not synced. Only map objects are.
- **Blood on walls/floor:** bullet impacts are synced (decal + surface effect). Blood decals that gore spawns are reproduced only where the hit is replayed, never transmitted on their own.
- **Two-player verification:** a real two-headset test of everything above is still outstanding.
- **Late join:** NPCs are recreated from the state stream, but corpses that settled before the join only appear when they next move or within 3 s of the last change. A full resync on join is still to do.

## 4. Approach for the next stages
1. Verify the NPC/corpse state sync in game from both sides (in progress).
2. Weapon state: sync the gun's own animation state (slide/mag/firing) through the gun blueprint's variables rather than mesh poses.
3. Snapshot on join: send every corpse pose and prop state once when a player joins.
4. Spawned-object sync for magazines and dropped guns (class + transform over the state channel, owner-authoritative).
5. Performance: measure Lua frame cost per system with the dev overlay and cut redundant per-frame UObject calls.
