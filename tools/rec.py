"""rec.py dev2019 [interval]  — loop: fb-export frames, skip duplicates.
   rec.py duo <label>       — one milestone frame via the bridge (checks link first)."""
import base64, hashlib, json, os, sys, time
sys.path.insert(0, os.path.expanduser("~/Documents/Dev/AppleBridge_Working"))
from mcp import tools
ROOT = os.path.join(os.path.dirname(__file__), "..", "gif", "frames")

def save(guest, png, label):
    d = os.path.join(ROOT, guest)
    name = time.strftime("%H%M%S") + ("_" + label if label else "") + ".png"
    open(os.path.join(d, name), "wb").write(png)
    return name

if sys.argv[1] == "dev2019":
    interval = float(sys.argv[2]) if len(sys.argv) > 2 else 15
    last = None
    while not os.path.exists(os.path.join(ROOT, "STOP")):
        r = tools.call_tool("mac_fb_screenshot", {"guest": "dev2019", "fallback": False})
        if r.get("image"):
            png = base64.b64decode(r["image"])
            h = hashlib.md5(png).hexdigest()
            if h != last:
                print(save("dev2019", png, ""), flush=True); last = h
        else:
            print("skip:", r.get("error"), flush=True)
        time.sleep(interval)
else:
    label = sys.argv[2] if len(sys.argv) > 2 else ""
    st = tools.call_tool("mac_status", {"guest": "duo"})
    if not st.get("daemon_connected"):
        print("duo link down, no frame"); sys.exit(1)
    t0 = time.time()
    with tools.using_port(9041):
        status, out, err = tools.get_connection().send_command("SCREENSHOT", timeout=240.0)
    if status == 0 and out:
        print(save("duo", base64.b64decode(out), label), "%.0fs" % (time.time() - t0))
    else:
        print("fail:", status, err); sys.exit(1)
