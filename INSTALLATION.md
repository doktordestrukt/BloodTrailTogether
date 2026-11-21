# Installing BloodTrail Together

## Requirements
- BloodTrail from Steam (tested build: **18598125**), a PCVR headset, Windows 10/11 64-bit.
- Internet access (UDP to Unity's relay servers must not be blocked).
- Both players need the **same version** of BloodTrail Together.

## Standard installation
1. Download `BloodTrailTogether-Setup-vX.Y.Z.exe` from the Releases page.
   Windows SmartScreen may warn because the installer is not code-signed: click *More info → Run anyway*.
   You can compare the file with `SHA256SUMS.txt` from the same release.
2. Run the installer. It looks through all your Steam library folders (including ones on other drives)
   and selects BloodTrail automatically. If not, click **Browse…** and select the folder that contains `BTVR.exe`.
3. Check the information lines:
   - *Game build … (supported)* — or a warning if your BloodTrail version is newer than this release was tested with.
   - *UE4SS: installed* — your existing UE4SS is kept. If it is missing, the installer adds it.
4. Click **Install**. The log shows every file written.
5. Click **Launch BloodTrail** (or start it from Steam).
6. In game press **F9** to show the BloodTrail Together window on your desktop:
   - Host: **Create Session** → tell your friend the code.
   - Friend: type the code → **Join Session**.
   The friend automatically follows the host to the same map.

## What the installer changes
Only inside `BloodTrail\BTVR\Binaries\Win64\`:
- adds `ue4ss\Mods\BloodTrailTogether\` (the mod), and
- **only if UE4SS is missing**: adds `dwmapi.dll` and the `ue4ss\` folder.

It never modifies `BTVR-Win64-Shipping.exe`, `BTVR.exe`, the `.pak` files, Steam files, your other
mods, `mods.txt` or `UE4SS-settings.ini`. A previous BloodTrail Together version is backed up to
`%LOCALAPPDATA%\BloodTrailTogether\backups\` before it is replaced. Your name and settings are kept on update.

## Repair / update
Run the newer installer and click **Repair / Update**. Settings are preserved.

## Uninstall
Run the installer → **Uninstall**. Tick *also remove UE4SS* only if you want UE4SS gone too; it is
removed only if this installer added it and no other UE4SS mods are installed.
Verifying the game files in Steam does **not** remove mods (they are extra files); use Uninstall.

## Manual install (advanced)
`BloodTrailTogether-vX.Y.Z-manual.zip` contains the mod folder. Copy its `BTVR` folder into your
BloodTrail folder (merge). UE4SS must already be installed.

## Command line (for testing / scripts)
```
BloodTrailTogether-Setup.exe --detect
BloodTrailTogether-Setup.exe --install "E:\SteamLibrary\steamapps\common\Blood Trail"
BloodTrailTogether-Setup.exe --uninstall "<folder>" [--remove-ue4ss]
```
Output is also written to `%TEMP%\BloodTrailTogether-installer.log`.
