# Troubleshooting

Logs to attach to bug reports:
- `BTVR\Binaries\Win64\ue4ss\Mods\BloodTrailTogether\BloodTrailTogether.log` (networking, sessions)
- `BTVR\Binaries\Win64\ue4ss\UE4SS.log` (lines starting with `[BloodTrailTogether]` are the in-game script)
- `%TEMP%\BloodTrailTogether-installer.log` (installer)

| Symptom | Cause / fix |
|---|---|
| F9 does nothing, no window | The mod did not load. Check `UE4SS.log` for `[BloodTrailTogether]`. If there is no UE4SS.log at all, UE4SS is not running: repair with the installer. |
| Window says *waiting for the in-game Lua mod* | The DLL loaded but the script cannot reach it. Restart the game; check UE4SS.log for errors. |
| *No Unity project id configured* | You are running a developer build without `Relay.ProjectId`. Use an official release. |
| *Unity sign-in failed (HTTP 401/403)* | The shared Unity project is misconfigured or Relay is disabled; report it. |
| *Relay server did not answer (UDP may be blocked)* | A firewall blocks outgoing UDP. Allow `BTVR-Win64-Shipping.exe` in Windows Firewall / your antivirus, or try another network. No port forwarding is needed. |
| *Joining with code … failed (HTTP 404)* | Wrong or expired code. Codes only work while the host's session is open. Ask the host for the current code. |
| *Rate limit reached* | Too many attempts in a short time; wait a minute. |
| *version mismatch* | Both players must run the same BloodTrail Together version. |
| Joiner is not moved to the host's map | Map following needs the host to be in a map (not loading). Both players should start from the hub. Some game menu state may not carry over when following by map load; report which mode/map. |
| You cannot see the other player | The avatar is only shown when both are on the same map (window shows "same map"). |
| *Connection interrupted* | Packets stopped for 2 s. It recovers on its own; after 20 s the session ends. |
| Echoes / doubled gunshots | Lower `ShareRadiusCm` in `config.ini` or untick *Share my gun / hand sounds*. |
| Antivirus flags the installer or `btt_core.dll` | Unsigned new files often get flagged. The source code is public; you can build it yourself. |

## Testing alone
In the multiplayer window click **Simulated player: mirror** (stands in front of you and copies your
movements) or **walk around**. Shared sounds are echoed back by the simulated player after 0.75 s.
