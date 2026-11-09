"""The setup run from a release package as a player would, the proof a release needs before it is published: on Linux
from the AppImage (its own Python, zig, glslc and runtime, PATH emptied), on the Windows PC from the zip
(dev/tools/win.sh). Prints the setup's events and ends with OK or the failure.

Why: v0.1.0 and v0.1.1 went out without the game ever compiled from their packages (issue #1), and on 09/11/2026 the
same check for v0.1.3 was put together by hand three times (mount, links, unpack, a detached process on Windows):
over an hour for a fix release.

--from warm: keeps the folder's finished steps (generate, compile, shaders) and runs the setup from its warm step, the
only one a runtime change touches; a new package with the same setup code needs only that. Without it, a fresh folder
and the whole setup (Linux ~20 min on 12 threads; Windows ~1.5 h on the 6-thread PC, mostly shaders).
Folders: scratchpad/prep-<target> here, C:\\mkw-rel on the Windows PC; the game copy: game/ here, C:\\mkw there.

Usage: uv run tools/build/prep_check.py --target linux|windows --version 0.1.3 [--from warm]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REL = ROOT / "build/release"
WIN = [str(ROOT / "dev/tools/win.sh")]
WIN_GAME, WIN_SRC, WIN_EVENTS = r"C:\mkw-rel", r"C:\mkw", r"C:\mkw-rel-events.txt"


def drop_steps(state: dict, keep_until_warm: bool) -> dict:
    """The setup state with the warm step and those after it undone (prepare.py resumes there), or all of it."""
    done = state.get("done", {})
    state["done"] = {k: v for k, v in done.items() if keep_until_warm and k not in ("warm", "cleanup")}
    return state


def linux(version: str, from_warm: bool) -> bool:
    app = REL / f"mkw-recomp-{version}-x86_64.AppImage"
    game = ROOT / "scratchpad/prep-linux"
    if not from_warm:
        shutil.rmtree(game, ignore_errors=True)
    game.mkdir(parents=True, exist_ok=True)
    st = game / "setup-state.json"
    if st.is_file():
        st.write_text(json.dumps(drop_steps(json.loads(st.read_text()), from_warm), indent=1))
    # --appimage-mount stays up until killed; the mount path is its first line
    mnt = subprocess.Popen([str(app), "--appimage-mount"], stdout=subprocess.PIPE, text=True)
    try:
        m = Path(mnt.stdout.readline().strip())
        shutil.copy2(m / "mkw", game / "mkw")
        link = game / "setup"
        link.unlink(missing_ok=True)
        link.symlink_to(m / "setup")
        env = {
            "PATH": "/usr/bin:/bin",
            "HOME": os.environ["HOME"],
            "LD_LIBRARY_PATH": str(m / "lib"),
            "MKW_BACKGROUND_WINDOW": "1",
            **{k: os.environ[k] for k in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR") if k in os.environ},
        }
        p = subprocess.run([str(m / "setup/python/bin/python3"), "-I", str(m / "setup/src/tools/prepare/prepare.py"),
                            "--source", str((ROOT / "game").resolve()), "--game", str(game), "--romfs", "link"],
                           env=env, capture_output=True, text=True, check=False)  # fmt: skip
    finally:
        mnt.terminate()
        mnt.wait()
    events = [ln for ln in p.stdout.splitlines() if not ln.startswith("progress")]
    print("\n".join(events))
    return p.returncode == 0 and events[-1:] == ["done"]


def win_ps(cmd: str) -> str:
    return subprocess.run([*WIN, "ps", cmd], capture_output=True, text=True, check=True).stdout


def windows(version: str, from_warm: bool) -> bool:
    z = REL / f"mkw-recomp-{version}-windows-x64.zip"
    host = os.environ.get("WIN_HOST", "win-pc")   # the ssh alias win.sh uses
    subprocess.run(["scp", "-q", str(z), f"{host}:C:/mkw-rel.zip"], check=True)
    reset = "" if from_warm else f"Remove-Item -Recurse -Force {WIN_GAME} -EA 0;"
    undo = (
        "$s = Get-Content setup-state.json -Raw | ConvertFrom-Json; foreach ($k in 'warm','cleanup') "
        "{ $s.done.PSObject.Properties.Remove($k) }; $s | ConvertTo-Json -Depth 5 | Set-Content setup-state.json"
        if from_warm
        else ""
    )
    # unpack and run in one detached cmd: ssh's own job would end both, and the unpack alone outlives its timeouts
    script = (f"{reset} Remove-Item -Recurse -Force C:\\mkw-rel-unz -EA 0; Expand-Archive C:\\mkw-rel.zip C:\\mkw-rel-unz;"
              f" New-Item -ItemType Directory -Force {WIN_GAME} | Out-Null;"
              f" Copy-Item -Recurse -Force C:\\mkw-rel-unz\\mkw-recomp\\* {WIN_GAME}; Remove-Item -Recurse -Force C:\\mkw-rel-unz;"
              f" Set-Location {WIN_GAME}; if (Test-Path setup-state.json) {{ {undo} }};"
              f" & {WIN_GAME}\\setup\\python\\python.exe -I {WIN_GAME}\\setup\\src\\tools\\prepare\\prepare.py"
              f" --source {WIN_SRC} --game {WIN_GAME} --romfs link")  # fmt: skip
    win_ps(f"Set-Content C:\\mkw-prep.ps1 @'\n{script}\n'@; Remove-Item {WIN_EVENTS} -EA 0")
    subprocess.run([*WIN, "detach", "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\mkw-prep.ps1", WIN_EVENTS],
                   check=True, stdout=subprocess.DEVNULL)  # fmt: skip
    seen = 0
    while True:
        time.sleep(30)
        lines = [ln for ln in win_ps(f"Get-Content {WIN_EVENTS} -EA 0").splitlines() if not ln.startswith("progress")]
        print("\n".join(lines[seen:]), flush=True) if lines[seen:] else None
        seen = len(lines)
        if lines and (lines[-1] == "done" or lines[-1].startswith("error")):
            return lines[-1] == "done"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--target", choices=("linux", "windows"), required=True)
    ap.add_argument("--version", required=True, help="the package's version in build/release, e.g. 0.1.3")
    ap.add_argument("--from", dest="start", choices=("warm",), help="keep the finished steps; run from warm")
    args = ap.parse_args()
    ok = (linux if args.target == "linux" else windows)(args.version, args.start == "warm")
    print(f"prep_check {args.target} {args.version}: {'OK' if ok else 'FAILED'}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
