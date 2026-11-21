"""Build a public BloodTrail Together release.

  python -I tools/make_release.py [--allow-missing-ue4ss] [--allow-empty-project]

Steps:
  1. builds the release configuration (build.cmd release) - developer features compiled out
  2. assembles the installer payload from an explicit allow-list of files
     (mod scripts, btt_core.dll, config with the PUBLIC Unity project id, licences, UE4SS)
  3. builds the installer with the payload embedded
  4. writes dist/<version>/: Setup exe, manual-install zip, SHA256SUMS.txt
Nothing from build trees, tests, SimPeer, dev.ini, logs or PDBs can end up in dist/.
"""
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(os.environ.get("BTT_BUILD_ROOT", os.path.join(os.environ["LOCALAPPDATA"], "BTTBuild")), "release")
MOD = os.path.join(ROOT, "mod", "BloodTrailTogether")
FORBIDDEN = re.compile(r"(\.pdb|\.ilk|\.obj|\.log|dev\.ini|\.secret|\.env|simpeer|bridge_probe|btt_tests)$", re.I)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def pack(entries):
    """entries: list of (component, relpath, bytes). component 0 = meta, 1 = mod, 2 = UE4SS"""
    out = io.BytesIO()
    out.write(b"BTTPACK1")
    out.write(struct.pack("<I", len(entries)))
    for comp, rel, data in entries:
        p = rel.replace("\\", "/").encode("utf-8")
        out.write(struct.pack("<BH", comp, len(p)))
        out.write(p)
        out.write(struct.pack("<I", len(data)))
        out.write(data)
    return out.getvalue()


def fill_config(text, project_id, environment, repo):
    text = re.sub(r"(?m)^ProjectId=.*$", "ProjectId=" + project_id, text)
    text = re.sub(r"(?m)^Environment=.*$", "Environment=" + environment, text)
    text = re.sub(r"(?m)^GitHubRepo=.*$", "GitHubRepo=" + repo, text)
    return text


def run(cmd, **kw):
    print(">", " ".join(cmd))
    subprocess.check_call(cmd, **kw)


def main():
    args = set(sys.argv[1:])
    rel = json.load(open(os.path.join(ROOT, "release.json"), encoding="utf-8"))
    version = rel["version"]
    project_id = rel.get("unity_project_id", "").strip()
    if not project_id and "--allow-empty-project" not in args:
        sys.exit("release.json: unity_project_id is empty - online play would not work. "
                 "Fill it in (it is a public id, not a secret) or pass --allow-empty-project for a test build.")
    if not re.fullmatch(r"[0-9a-fA-F-]{0,64}", project_id):
        sys.exit("unity_project_id does not look like a Unity project id (UUID)")

    cmake_version = re.search(r"project\(BloodTrailTogether VERSION ([0-9.]+)", open(os.path.join(ROOT, "CMakeLists.txt")).read()).group(1)
    lua_version = re.search(r'local VERSION = "([0-9.]+)"', open(os.path.join(MOD, "Scripts", "main.lua")).read()).group(1)
    if not (version == cmake_version == lua_version):
        sys.exit(f"version mismatch: release.json {version}, CMakeLists {cmake_version}, main.lua {lua_version}")

    # 1. release build (no payload yet; the DLL is what we need first)
    run(["cmd", "/c", os.path.join(ROOT, "build.cmd"), "release", "--target", "btt_core"])
    dll = open(os.path.join(BUILD, "btt_core.dll"), "rb").read()

    # 2. payload from an explicit allow-list
    entries = []
    modrel = "ue4ss/Mods/BloodTrailTogether/"
    cfg = fill_config(open(os.path.join(MOD, "config.ini"), encoding="utf-8").read(), project_id,
                      rel.get("unity_environment", "production"), rel.get("github_repo", ""))
    entries.append((1, modrel + "Scripts/main.lua", open(os.path.join(MOD, "Scripts", "main.lua"), "rb").read()))
    entries.append((1, modrel + "enabled.txt", b""))
    entries.append((1, modrel + "config.ini", cfg.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")))
    entries.append((1, modrel + "bin/btt_core.dll", dll))
    entries.append((1, modrel + "LICENSE.txt", open(os.path.join(ROOT, "LICENSE"), "rb").read()))
    entries.append((1, modrel + "THIRD_PARTY_NOTICES.txt", open(os.path.join(ROOT, "THIRD_PARTY_NOTICES.md"), "rb").read()))

    ue = rel.get("ue4ss", {})
    ue_zip = os.path.join(ROOT, ue.get("zip", ""))
    if ue.get("zip") and os.path.exists(ue_zip):
        data = open(ue_zip, "rb").read()
        if ue.get("sha256") and sha256(data) != ue["sha256"].lower():
            sys.exit("UE4SS zip checksum mismatch - refusing to package it")
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                name = info.filename
                if name.lower().endswith(".exe"):
                    continue  # never ship executables into the game folder
                data = z.read(info)
                if name == "ue4ss/UE4SS-settings.ini":
                    # BloodTrail is UE 4.27.2; pin it like the known-good install instead of auto-detecting.
                    text = data.decode("utf-8")
                    text = re.sub(r"(?m)^MajorVersion =\s*$", "MajorVersion = 4", text)
                    text = re.sub(r"(?m)^MinorVersion =\s*$", "MinorVersion = 27", text)
                    data = text.encode("utf-8")
                entries.append((2, name, data))
        print(f"UE4SS bundled: {ue.get('name')} ({len([e for e in entries if e[0] == 2])} files)")
    elif "--allow-missing-ue4ss" not in args:
        sys.exit(f"UE4SS zip not found at {ue_zip}. Download it (see release.json ue4ss.url) or pass --allow-missing-ue4ss")
    else:
        print("WARNING: building WITHOUT UE4SS - installer will require an existing UE4SS")

    for comp, relp, _ in entries:
        if FORBIDDEN.search(relp):
            sys.exit("forbidden file in payload: " + relp)
    meta = (f"version={version}\nsupported_builds={','.join(rel.get('supported_builds', []))}\n"
            f"github_repo={rel.get('github_repo', '')}\nue4ss_version={ue.get('name', '')}\n").encode()
    payload = pack([(0, "meta.ini", meta)] + entries)
    payload_path = os.path.join(BUILD, "payload.bin")
    open(payload_path, "wb").write(payload)
    print(f"payload: {len(entries)} files, {len(payload) / 1e6:.1f} MB")

    # 3. installer with embedded payload
    run(["cmd", "/c", os.path.join(ROOT, "build.cmd"), "release", "--target", "btt_installer"],
        env=dict(os.environ, BTT_PAYLOAD=payload_path))

    # 4. dist
    dist = os.path.join(ROOT, "dist", version)
    shutil.rmtree(dist, ignore_errors=True)
    os.makedirs(dist)
    setup = os.path.join(dist, f"BloodTrailTogether-Setup-v{version}.exe")
    shutil.copy(os.path.join(BUILD, "btt_installer.exe"), setup)
    manual = os.path.join(dist, f"BloodTrailTogether-v{version}-manual.zip")
    with zipfile.ZipFile(manual, "w", zipfile.ZIP_DEFLATED) as z:
        for comp, relp, data in entries:
            if comp == 1:
                z.writestr("BTVR/Binaries/Win64/" + relp, data)
        z.writestr("README-MANUAL-INSTALL.txt",
                   "Copy the BTVR folder into your BloodTrail folder (merge). Requires UE4SS.\r\n"
                   "The Setup .exe does this for you and is the recommended way to install.\r\n")
    with open(os.path.join(dist, "SHA256SUMS.txt"), "w") as f:
        for p in (setup, manual):
            f.write(f"{sha256(open(p, 'rb').read())}  {os.path.basename(p)}\n")
    print("release ready in", dist)


if __name__ == "__main__":
    main()
