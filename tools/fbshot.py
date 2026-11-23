"""fbshot.py out.png [x y w h] — dev2019 framebuffer to PNG."""
import os, base64, sys
sys.path.insert(0, os.path.expanduser("~/Documents/Dev/AppleBridge_Working"))
from mcp import tools
a = {"guest": "dev2019", "fallback": False}
if len(sys.argv) > 2: a["region"] = [int(v) for v in sys.argv[2:6]]
r = tools.call_tool("mac_fb_screenshot", a)
open(sys.argv[1], "wb").write(base64.b64decode(r["image"]))
