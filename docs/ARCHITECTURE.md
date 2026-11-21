# Architecture

```
 BloodTrail process (BTVR-Win64-Shipping.exe, unmodified, UE 4.27.2)
 ┌───────────────────────────────────────────────────────────────────────────────────────┐
 │ UE4SS (dwmapi.dll proxy) ── Lua: ue4ss/Mods/BloodTrailTogether/Scripts/main.lua         │
 │   every frame (game thread, ExecuteInGameThread):                                       │
 │     read HMD (PlayerCameraManager) + MotionControllerComponents (Left/Right)            │
 │     read current map (world package path), captured sounds (GameplayStatics hooks)      │
 │     ── one request/reply over named pipes \\.\pipe\BloodTrailTogether_<n>_up/_down ──     │
 │     draw remote avatar (StaticMeshActors + TextRenderActor), play remote sounds,        │
 │     OpenLevel to follow the host, VR notifications                                       │
 │                                                                                           │
 │ bin/btt_core.dll  (loaded by package.loadlib(path,"*"), no Unreal/UE4SS headers)          │
 │   bridge/   pipe server, text protocol, desktop window (F9), startup                       │
 │   engine/   NetEngine: session lifecycle, map-follow logic, simulated player, update check │
 │   net/      Session (handshake, reliable+unreliable channels, stats), ITransport,          │
 │             LoopbackTransport, UdpTransport (dev), ImpairedTransport (latency/loss)        │
 │   relay/    RelayTransport (UDP data plane), RelayApi (HTTPS: auth, allocate, join codes), │
 │             relay_messages (wire format), HMAC-SHA256 (CNG), JSON, WinHTTP                  │
 │   sync/     pose codec (49 B for 3 devices), snapshot interpolation                         │
 └───────────────────────────────────────────────────────────────────────────────────────┘
                 UDP ⇄ Unity Relay server ⇄ UDP          HTTPS → Unity Auth / Relay Allocations
```

## Why this shape
- **No UE4SS C++ mod.** Building one needs UE4SS's private `UEPseudo` headers (GitHub account linked
  to Epic). The DLL instead has no Unreal dependency at all and is loaded with plain `LoadLibrary`
  from Lua, so it also does not depend on a specific UE4SS build.
- **Pipe bridge.** UE4SS Lua has no sockets. A synchronous request/reply over a local named pipe costs
  ~50 µs per frame (measured with `btt_bridge_probe`), and replies come from cached state, so the game
  thread never waits on the network.
- **Own sync protocol, not Unreal replication.** BloodTrail's Blueprints contain no replication logic and
  a monolithic shipping build cannot load a custom NetDriver. Each player runs a full game; we sync
  poses and selected events.
- **Transport is replaceable.** `ITransport` hides Unity Relay; loopback/UDP/impairment wrappers are used for tests.

## Data on the wire (inside a Relay RELAY message)
Header 16 B: `'B' 'T' ver flags | session-id u32 | seq u16 | reliable-ack u16 | send-time-ms u32`,
then chunks `type u8, len u16, body`:
- `POSE` (unreliable, 30 Hz): flags + 3× (position int32 mm ×3, rotation smallest-three 32 bit) = 49 B
- `REL` (reliable, ordered): HELLO/WELCOME (protocol version, role, name, mod version), EVENT
  (Map, Sound, Notice, World, Request)
- `PING`/`PONG` (1 Hz RTT), `BYE`

Typical packet: 16 + 52 + overhead ≈ 75 B → about 20 kbit/s per direction including Relay headers.

## Authority
- Host decides the map; the client follows (`TR` command → `GameplayStatics::OpenLevel`).
- Each player owns their own body pose and their own sounds.
- World state (`World` events) is host-only by design; nothing is synchronised there yet.

## Threads
| Thread | Work |
|---|---|
| Game thread (Lua) | pose read, one pipe exchange, avatar/sound/travel |
| Pipe thread | answers Lua from engine state |
| Net thread (500 Hz loop) | transport IO, session, simulated player |
| Relay worker | HTTPS sign-in / allocation (one-off) |
| UI thread | desktop window |
| Update thread | one GitHub request 5 s after start |
