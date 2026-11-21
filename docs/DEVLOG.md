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
