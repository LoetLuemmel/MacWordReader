"""duoshot.py <guest> <mac folder with DuoShot> <label> — one frame via the DuoShot helper.
Launches DuoShot on the guest, waits for DuoShot.raw, fetches it, writes gif/frames/<guest>/<time>_<label>.png"""
import os, sys, time, json, tempfile
sys.path.insert(0, os.path.expanduser("~/Documents/Dev/AppleBridge_Working"))
from mcp import tools
from PIL import Image

guest, folder, label = sys.argv[1], sys.argv[2].rstrip(":") + ":", sys.argv[3]
raw = folder + "DuoShot.raw"
call = lambda n, a: tools.call_tool(n, dict(a, guest=guest))
t0 = time.time()
call("mpw_execute", {"command": f"Set Exit 0; Delete -i '{raw}'", "timeout": 30})
r = call("launch_app", {"path": folder + "DuoShot"})
if not r.get("success"):
    print("launch failed", r); sys.exit(1)
for _ in range(60):
    out = call("mpw_execute", {"command": f"Set Exit 0; Exists '{raw}'", "timeout": 30}).get("output", "")
    if "DuoShot.raw" in out:
        break
    time.sleep(2)
else:
    print("no DuoShot.raw"); sys.exit(1)
time.sleep(1)
tmp = os.path.join(tempfile.mkdtemp(), "shot.raw")
r = call("mac_get_file", {"mac_path": raw, "host_path": tmp, "format": "data"})
if not r.get("success"):
    print("fetch failed", r); sys.exit(1)
b = open(tmp, "rb").read()
assert b[:4] == b"DSHT", b[:4]
w, h = int.from_bytes(b[4:6], "big"), int.from_bytes(b[6:8], "big")
pal = bytes(b[8 + i * 6 + k] for i in range(256) for k in (0, 2, 4))
img = Image.frombytes("P", (w, h), b[8 + 1536: 8 + 1536 + w * h])
img.putpalette(pal)
d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "gif", "frames", guest)
os.makedirs(d, exist_ok=True)
name = time.strftime("%H%M%S") + "_" + label + ".png"
img.convert("RGB").save(os.path.join(d, name))
print(name, f"{w}x{h}", f"{len(b)//1024} KB", f"{time.time()-t0:.0f}s")
