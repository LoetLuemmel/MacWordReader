"""build_mac.py — build WordReader on the dev2019 guest (MPW via ToolServer).

1. sources -> MacRoman/CR into ~/Desktop/Share/WordReader (seen as Unix:WordReader:)
2. Duplicate into MeinMac:MPW:WordReader:, SetFile TEXT/MPS
3. SC each .c, Link, Rez, SetFile — every step checked by its artefact and its
   ≥ error file (a long Link can answer -1712 and still have succeeded).
"""
import json, os, subprocess, sys
sys.path.insert(0, os.path.expanduser("~/Documents/Dev/AppleBridge_Working"))
from mcp import tools

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHARE = os.path.expanduser("~/Desktop/Share/WordReader")
CONVERT = os.path.expanduser("~/Documents/Dev/AppleBridge_Working/host/encoding_convert.py")
PROJ = "MeinMac:MPW:WordReader:"
SOURCES = ["macroman.c", "inflate.c", "zipdir.c", "docx.c", "image.c", "main.c"]
TEXTS = SOURCES + ["wrplat.h", "macroman.h", "inflate.h", "zipdir.h", "docx.h", "image.h",
                   "WordReader.r", "icons.r", "DuoShot.c", "DuoShot.r"]
GUEST = "dev2019"


def mpw(cmd, timeout=300):
    r = tools.call_tool("mpw_execute", {"command": cmd, "timeout": timeout, "guest": GUEST})
    return (r.get("output") or "").replace("(no output)", "").strip()


def exists(path):
    out = mpw(f"Exists '{path}'", 30)
    return bool(out) and "NoDir" not in out


def step(label, cmd, artefact, timeout=300):
    mpw(f"Delete -i '{artefact}'", 30) if artefact else None
    mpw(f"{cmd} ≥ '{PROJ}build.err'", timeout)
    err = mpw(f"Catenate '{PROJ}build.err'", 30)
    ok = exists(artefact) if artefact else True
    if not ok or ("Error" in err and "Warning" not in err.split("Error")[0][-20:]):
        print(f"FAIL {label}\n{err}")
        if not ok:
            sys.exit(1)
    else:
        print(f"ok   {label}" + (f"  ({err.count(chr(10)) + 1} lines of notes)" if err else ""))
    return err


def main():
    os.makedirs(SHARE, exist_ok=True)
    for f in TEXTS:
        subprocess.run(["uv", "run", "python", CONVERT, "to-mac", os.path.join(ROOT, "src", f),
                        os.path.join(SHARE, f)], check=True, capture_output=True)
    print("ok   converted", len(TEXTS), "files to MacRoman/CR")

    # a running WordReader holds its file open: ask it to quit (aevt/quit)
    tools.call_tool("mac_send_apple_event", {"target_creator": "WRdr", "event_class": "aevt",
                                             "event_id": "quit", "expect_reply": False, "guest": GUEST})
    mpw(f"Set Exit 0; NewFolder '{PROJ}'", 30)
    copy = "; ".join(f"Duplicate -y 'Unix:WordReader:{f}' '{PROJ}{f}'; "
                     f"SetFile -t TEXT -c 'MPS ' '{PROJ}{f}'" for f in TEXTS)
    mpw("Set Exit 0; " + copy, 120)
    missing = [f for f in TEXTS if not exists(PROJ + f)]
    if missing:
        print("FAIL copy, missing:", missing); sys.exit(1)
    print("ok   copied to", PROJ)

    if "duoshot" in sys.argv:
        o = f"{PROJ}DuoShot.o"
        step("SC DuoShot.c", f"SC -model far '{PROJ}DuoShot.c' -o '{o}'", o)
        app = f"{PROJ}DuoShot"
        step("Link DuoShot", f"Link -model far -t APPL -c DSht '{o}' "
             '"{Libraries}Interface.o" "{Libraries}MacRuntime.o"' f" -o '{app}'", app)
        step("Rez DuoShot", f"Rez -a '{PROJ}DuoShot.r' -o '{app}'", None)
        print(mpw(f"Files -x abkt '{app}'", 30))
        return

    objs = []
    for c in SOURCES:
        o = f"{PROJ}{c[:-2]}.o"
        notes = step(f"SC {c}", f"Directory '{PROJ}'; SC -model far -opt speed -i '{PROJ}' "
                     f"'{PROJ}{c}' -o '{o}'", o)
        if notes:
            print("     " + notes.replace("\n", "\n     ")[:3000])
        objs.append(f"'{o}'")

    app = f"{PROJ}WordReader"
    libs = ('"{Libraries}Interface.o" "{Libraries}MacRuntime.o"'
            )
    notes = step("Link", f"Link -model far -t APPL -c WRdr {' '.join(objs)} {libs} -o '{app}'",
                 app, timeout=400)
    if notes:
        print("     " + notes[:2000])
    notes = step("Rez", f"Directory '{PROJ}'; Rez -a -i '{PROJ}' '{PROJ}WordReader.r' -o '{app}'", None)
    print("     " + notes[:2000]) if notes else None
    mpw(f"SetFile -t APPL -c WRdr -a B '{app}'", 30)
    print(mpw(f"Files -x abkt '{app}'", 30))


if __name__ == "__main__":
    main()
