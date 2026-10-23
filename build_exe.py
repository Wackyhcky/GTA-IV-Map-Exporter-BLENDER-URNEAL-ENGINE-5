"""Builds "GTA IV Exporter.exe" and the Blender add-on zip.

Usage: double-click build_exe.bat (or run: python build_exe.py)

Everything (virtual env, temp files, pip downloads) stays inside the .build
folder next to this script, so your main Python install is not touched.
Output:
    dist/GTA IV Exporter/GTA IV Exporter.exe   (keep with its _internal folder)
    dist/gtaiv_exporter_addon.zip                   (Blender: Install from Disk)
"""

import glob
import json
import os
import shutil
import subprocess
import sys
import venv
import zipfile

SRC = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(SRC, ".build")
VENV = os.path.join(BUILD, "venv")
DIST = os.path.join(SRC, "dist")
APP = "GTA IV Exporter"
PACKAGES = ["pyinstaller", "numpy", "pycryptodome", "sv-ttk", "pillow"]  # sv-ttk: Windows 11 look for Tk


def find_runuat():
    """RunUAT.bat of the newest Unreal Engine 5 install (Epic launcher list + common folders)."""
    roots = []
    try:
        dat = r"C:\ProgramData\Epic\UnrealEngineLauncher\LauncherInstalled.dat"
        roots += [e["InstallLocation"] for e in json.load(open(dat, encoding="utf-8")).get("InstallationList", [])
                  if e.get("AppName", "").startswith("UE_5")]
    except Exception:
        pass
    roots += glob.glob(r"C:\Program Files\Epic Games\UE_5*") + glob.glob(r"?:\Epic Games\UE_5*")
    bats = {os.path.join(r, "Engine", "Build", "BatchFiles", "RunUAT.bat") for r in roots}
    bats = sorted(b for b in bats if os.path.isfile(b))
    return bats[-1] if bats else None


def traffic_plugin(env):
    """The compiled GTAIVTraffic plugin to bundle: UnrealPlugin/GTAIVTraffic_Built if present,
    otherwise built here (into .build) with Unreal's RunUAT BuildPlugin."""
    prebuilt = os.path.join(SRC, "UnrealPlugin", "GTAIVTraffic_Built")
    if os.path.isdir(os.path.join(prebuilt, "Binaries")):
        return prebuilt
    out = os.path.join(BUILD, "GTAIVTraffic")
    src_dir = os.path.join(SRC, "UnrealPlugin", "GTAIVTraffic")
    stamp = max(os.path.getmtime(os.path.join(d, f)) for d, _, fs in os.walk(src_dir) for f in fs)
    dll = os.path.join(out, "Binaries", "Win64", "UnrealEditor-GTAIVTraffic.dll")
    if os.path.isfile(dll) and os.path.getmtime(dll) > stamp:
        return out  # already built from the current source
    uat = find_runuat()
    if uat is None:
        print("Unreal Engine 5 not found: building without the traffic/pedestrian plugin", flush=True)
        return None
    print("Building the GTAIVTraffic Unreal plugin (a couple of minutes)...", flush=True)
    shutil.rmtree(out, ignore_errors=True)
    subprocess.run('"%s" BuildPlugin -Plugin="%s" -Package="%s" -TargetPlatforms=Win64 -Rocket'
                   % (uat, os.path.join(src_dir, "GTAIVTraffic.uplugin"), out), check=True, env=env, shell=True)
    return out


def run(cmd, env):
    print(">", " ".join('"%s"' % c if " " in c else c for c in cmd), flush=True)
    subprocess.run(cmd, check=True, env=env)


def main():
    tmp = os.path.join(BUILD, "tmp")
    os.makedirs(tmp, exist_ok=True)
    env = dict(os.environ, TEMP=tmp, TMP=tmp, PIP_NO_CACHE_DIR="1")

    py = os.path.join(VENV, "Scripts", "python.exe")
    if not os.path.isfile(py):
        print("Creating build environment...", flush=True)
        venv.create(VENV, with_pip=True)
        run([py, "-m", "pip", "install", "-q", "--upgrade", "pip"], env)
    run([py, "-m", "pip", "install", "-q"] + PACKAGES, env)

    for d in (os.path.join(SRC, "gtaiv_exporter", "__pycache__"),):
        shutil.rmtree(d, ignore_errors=True)
    plugin = traffic_plugin(env)
    sep = os.pathsep  # ';' on Windows
    run([py, "-m", "PyInstaller", "--noconfirm", "--clean", "--onedir", "--windowed",
         "--name", APP,
         "--collect-data", "sv_ttk",
         "--add-data", os.path.join(SRC, "gtaiv_exporter") + sep + "gtaiv_exporter",
         "--add-data", os.path.join(SRC, "blender_runner.py") + sep + ".",
         "--add-data", os.path.join(SRC, "unreal_runner.py") + sep + ".",
         "--add-data", os.path.join(SRC, "unreal_lights.py") + sep + ".",
         "--add-data", os.path.join(SRC, "unreal_peds.py") + sep + ".",
         "--add-data", os.path.join(SRC, "unreal_frontend.py") + sep + ".",
         "--add-data", os.path.join(SRC, "unreal_hud.py") + sep + ".",
         "--add-data", os.path.join(SRC, "unreal_player.py") + sep + ".",
         "--add-data", os.path.join(SRC, "unreal_story.py") + sep + ".",
         *(["--add-data", plugin + sep + os.path.join("UnrealPlugin", "GTAIVTraffic")] if plugin else []),
         "--distpath", DIST,
         "--workpath", os.path.join(BUILD, "work"),
         "--specpath", BUILD,
         os.path.join(SRC, "gtaiv_exporter_gui.py")], env)

    shutil.copy(os.path.join(SRC, "app_README.txt"), os.path.join(DIST, APP, "README.txt"))

    addon_zip = os.path.join(DIST, "gtaiv_exporter_addon.zip")
    pkg = os.path.join(SRC, "gtaiv_exporter")
    with zipfile.ZipFile(addon_zip, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(os.listdir(pkg)):
            p = os.path.join(pkg, f)
            if os.path.isfile(p) and not f.endswith((".pyc", ".bin")):
                z.write(p, "gtaiv_exporter/" + f)

    shutil.rmtree(tmp, ignore_errors=True)
    print("\nDone:")
    print("  " + os.path.join(DIST, APP, APP + ".exe"))
    print("  " + addon_zip)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as e:
        print("\nBuild failed (%s)" % e)
        sys.exit(1)
