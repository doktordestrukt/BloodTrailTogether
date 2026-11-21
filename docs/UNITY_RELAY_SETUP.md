# Unity Relay: one shared project for every player

## Design
Players never need a Unity account. The release ships one **public Unity project id** and every copy
of the mod uses that project:

1. The mod signs in with **Unity Authentication – anonymous sign-in**
   (`POST https://player-auth.services.api.unity.com/v1/authentication/anonymous`, headers `ProjectId`,
   `UnityEnvironment`). This creates a throw-away player identity and returns a short-lived ID token.
2. With that token it calls the **Relay Allocations API**
   (`https://relay-allocations.services.api.unity.com`): `/v1/allocate` + `/v1/joincode` (host) or
   `/v1/join` (joiner).
3. Game traffic then flows over UDP to Unity's relay server using the Relay message protocol.

**No secret is shipped.** The project id is an identifier, like a website address — Unity's own games
embed it in every build. Service-account keys / secret keys are never used by the client and must
never be put in `config.ini`, `release.json` or the repository (`.gitignore` blocks `*.secret`, `.env`, `dev.ini`).

### Is a server of our own needed?
Not for correctness or security of player data: the only thing a stranger can do with the public
project id is what any player can do — create relay sessions on our project. The risk is **cost
abuse**. Mitigations, in order:
1. Unity Dashboard → billing alerts / spending cap (do this first).
2. Keep `maxConnections = 1` (enforced in the client) so one allocation = one pair of players.
3. If abuse ever happens: put a tiny token broker in front (for example Unity Cloud Code or a
   Cloudflare Worker) that hands out allocations only to clients that pass a check (e.g. Steam session
   ticket validation). The transport is modular (`RelayApi` base URLs are configurable), so this is a
   change to the control plane only. It is deliberately **not** built now because it adds a server
   that must be paid for and maintained, for a problem that has not happened.

## One-time setup (project owner — that's you)
1. Sign in at https://cloud.unity.com (free Unity ID).
2. Create a project, e.g. "BloodTrail Together" (no Unity Editor needed).
3. In the project: **Relay** → enable / "Get started". **Authentication** → *Identity providers* →
   ensure **Anonymous** sign-in is enabled.
4. Note the **Project ID** (Project settings → General; a UUID) and the environment name (`production`).
5. Billing → set alerts and a spending limit.
6. Put the Project ID into `release.json` → `unity_project_id` (it is public) and build a release.

## Pricing (check the live page before relying on it)
Unity Gaming Services pricing (unity.com/products/gaming-services/pricing) lists a free tier for Relay
of 50 average monthly concurrent users and 3 GiB bandwidth per CCU (up to 150 GiB/month). One
BloodTrail Together pair uses roughly 15–30 kbit/s per direction (poses at 30 Hz + events), i.e.
about 10–25 MB per hour of play for both players together.

## Protocol notes (what we verified and where)
- Message layout, type and error codes, 10 s inactivity timeout, 1400-byte payload limit:
  https://docs.unity.com/en-us/relay/relay-message-protocol
- Byte order details and the exact BIND HMAC input were taken from Unity Transport's reference
  implementation (`com.unity.transport`, `Runtime/Relay`), because the written docs say "big-endian"
  for everything while Unity's own client writes the BIND nonce and PING sequence little-endian and only
  the RELAY length big-endian. We follow the reference client. If the live service rejects our BIND,
  this is the first place to look (`src/relay/relay_messages.cpp`).
- REST paths and JSON field names come from Unity's SDK sources (`com.unity.services.relay`,
  `com.unity.services.authentication`).
- We use the plain `udp` endpoint. DTLS is optional per allocation endpoint and is not implemented.
