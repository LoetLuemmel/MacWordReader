"""shot.py <guest> <out.png> — save a guest screenshot as PNG."""
import os, base64, json, sys
sys.path.insert(0, os.path.expanduser("~/Documents/Dev/AppleBridge_Working"))
from mcp import tools
r = tools.call_tool("mac_screenshot", {"guest": sys.argv[1]})
if not r.get("image"):
    print(json.dumps({k: v for k, v in r.items() if k != "image"}, default=str)); sys.exit(1)
open(sys.argv[2], "wb").write(base64.b64decode(r["image"]))
print("saved", sys.argv[2], {k: v for k, v in r.items() if k not in ("image",)})
