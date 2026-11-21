"""Automated installer tests on FAKE BloodTrail folders (the real game is never touched).

  python -I tests/installer_test.py <installer.exe> <workdir> [<real Win64 dir to copy UE4SS from>]

Scenarios
  A  no UE4SS + payload without UE4SS        -> install refused with a clear error, nothing written
  B  existing UE4SS + other mods             -> mod installed, other mods / mods.txt / exe untouched
  C  repair after the user changed settings  -> name kept, ProjectId refreshed, backup made
  D  uninstall                               -> mod removed, UE4SS and other mods untouched
  E  clean folder + payload WITH UE4SS       -> UE4SS installed, recorded, removed again with --remove-ue4ss
  F  game running guard is exercised indirectly (process name check) - not simulated here
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))

INSTALLER, WORK = sys.argv[1], sys.argv[2]
REAL_WIN64 = sys.argv[3] if len(sys.argv) > 3 else None
passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
        print("  ok  ", what)
    else:
        failed += 1
        print("  FAIL", what)


def h(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def tree_hashes(root, skip=None):
    out = {}
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            if skip and skip in p:
                continue
            out[os.path.relpath(p, root)] = h(p)
    return out


def make_game(name, build="18598125"):
    lib = os.path.join(WORK, name)
    shutil.rmtree(lib, ignore_errors=True)
    game = os.path.join(lib, "steamapps", "common", "Blood Trail")
    win64 = os.path.join(game, "BTVR", "Binaries", "Win64")
    os.makedirs(win64)
    open(os.path.join(win64, "BTVR-Win64-Shipping.exe"), "wb").write(b"MZ fake shipping exe for tests")
    open(os.path.join(game, "BTVR.exe"), "wb").write(b"MZ fake launcher")
    open(os.path.join(lib, "steamapps", "appmanifest_1032430.acf"), "w").write(
        '"AppState"\n{\n\t"appid"\t\t"1032430"\n\t"installdir"\t\t"Blood Trail"\n\t"buildid"\t\t"%s"\n}\n' % build)
    return game, win64


def run(*args):
    r = subprocess.run([INSTALLER, *args], capture_output=True, text=True, timeout=120)
    return r.returncode


def pack(entries):
    out = bytearray(b"BTTPACK1" + struct.pack("<I", len(entries)))
    for comp, rel, data in entries:
        p = rel.encode()
        out += struct.pack("<BH", comp, len(p)) + p + struct.pack("<I", len(data)) + data
    return bytes(out)


print("[A] no UE4SS, payload without UE4SS")
game, win64 = make_game("libA")
noue = os.path.join(WORK, "payload-no-ue4ss.bin")
open(noue, "wb").write(pack([(0, "meta.ini", b"version=test\n"), (1, "ue4ss/Mods/BloodTrailTogether/Scripts/main.lua", b"-- test")]))
check(run("--install", game, "--payload", noue) != 0, "install refused")
check(not os.path.exists(os.path.join(win64, "ue4ss")), "nothing written")

if REAL_WIN64:
    print("[B] existing UE4SS with other mods")
    game, win64 = make_game("libB")
    shutil.copy(os.path.join(REAL_WIN64, "dwmapi.dll"), win64)
    shutil.copytree(os.path.join(REAL_WIN64, "ue4ss"), os.path.join(win64, "ue4ss"),
                    ignore=shutil.ignore_patterns("BloodTrailTogether", "*.log"))
    before = tree_hashes(win64)
    exe_before = h(os.path.join(win64, "BTVR-Win64-Shipping.exe"))
    check(run("--install", game) == 0, "install succeeded")
    mod = os.path.join(win64, "ue4ss", "Mods", "BloodTrailTogether")
    check(os.path.exists(os.path.join(mod, "Scripts", "main.lua")), "main.lua installed")
    check(os.path.exists(os.path.join(mod, "bin", "btt_core.dll")), "btt_core.dll installed")
    check(os.path.exists(os.path.join(mod, "enabled.txt")), "enabled.txt installed")
    after = tree_hashes(win64, skip="BloodTrailTogether")
    check(before == after, "every pre-existing file unchanged (other mods, mods.txt, UE4SS, settings)")
    check(h(os.path.join(win64, "BTVR-Win64-Shipping.exe")) == exe_before, "game executable untouched")
    manifest = open(os.path.join(mod, "install-manifest.ini")).read()
    check("UE4SSInstalledByUs=0" in manifest, "manifest: UE4SS not ours")

    print("[C] repair keeps user settings")
    cfg = os.path.join(mod, "config.ini")
    text = open(cfg).read().replace("Name=Player", "Name=Tester").replace("ShareRadiusCm=180", "ShareRadiusCm=99")
    text = text.replace("ProjectId=", "ProjectId=stale-value")
    open(cfg, "w").write(text)
    check(run("--install", game) == 0, "repair succeeded")
    text = open(cfg).read()
    check("Name=Tester" in text, "player name kept")
    check("ShareRadiusCm=99" in text, "audio setting kept")
    check("stale-value" not in text, "ProjectId refreshed from release")
    backups = os.path.join(os.environ["LOCALAPPDATA"], "BloodTrailTogether", "backups")
    check(os.path.isdir(backups) and len(os.listdir(backups)) > 0, "backup of previous version created")

    print("[D] uninstall")
    before = tree_hashes(win64, skip="BloodTrailTogether")
    check(run("--uninstall", game, "--remove-ue4ss") == 0, "uninstall succeeded")
    check(not os.path.exists(mod), "mod folder removed")
    check(tree_hashes(win64) == before, "UE4SS (not ours) and other mods left exactly as they were")
else:
    print("[B-D] skipped (no real UE4SS folder given to copy from)")

print("[E] clean folder, payload WITH UE4SS")
game, win64 = make_game("libE", build="99999999")
fake_proxy = os.path.join(REAL_WIN64, "dwmapi.dll") if REAL_WIN64 else None
ue_files = [(2, "ue4ss/UE4SS.dll", b"fake core"), (2, "ue4ss/UE4SS-settings.ini", b"[General]\n"),
            (2, "ue4ss/Mods/shared/UEHelpers/UEHelpers.lua", b"return {}")]
if fake_proxy:
    ue_files.append((2, "dwmapi.dll", open(fake_proxy, "rb").read()))
payload = pack([(0, "meta.ini", b"version=0.1.0\nsupported_builds=18598125\n"),
                (1, "ue4ss/Mods/BloodTrailTogether/Scripts/main.lua", b"-- test"),
                (1, "ue4ss/Mods/BloodTrailTogether/config.ini", b"[Player]\r\nName=Player\r\n"),
                (1, "ue4ss/Mods/BloodTrailTogether/enabled.txt", b"")] + ue_files)
pfile = os.path.join(WORK, "test-payload.bin")
open(pfile, "wb").write(payload)
check(run("--install", game, "--payload", pfile) == 0, "install with UE4SS succeeded (untested game build)")
check(os.path.exists(os.path.join(win64, "ue4ss", "UE4SS.dll")), "UE4SS installed")
mod = os.path.join(win64, "ue4ss", "Mods", "BloodTrailTogether")
check("UE4SSInstalledByUs=1" in open(os.path.join(mod, "install-manifest.ini")).read(), "manifest records UE4SS as ours")
check(run("--uninstall", game, "--remove-ue4ss") == 0, "uninstall succeeded")
check(not os.path.exists(os.path.join(win64, "ue4ss")), "UE4SS removed because we installed it")
check(os.path.exists(os.path.join(win64, "BTVR-Win64-Shipping.exe")), "game executable still there")

print("[G] unsafe payload path rejected")
game, win64 = make_game("libG")
os.makedirs(os.path.join(win64, "ue4ss"))
evil = pack([(0, "meta.ini", b"version=x\n"), (1, "../../../evil.txt", b"x"), (2, "ue4ss/UE4SS.dll", b"x")])
open(pfile, "wb").write(evil)
check(run("--install", game, "--payload", pfile) != 0, "path traversal refused")
check(not os.path.exists(os.path.join(game, "..", "evil.txt")), "nothing written outside")

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
