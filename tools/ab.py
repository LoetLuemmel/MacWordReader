"""Call an AppleBridge MCP tool from the shell: ab.py <tool> '<json args>'"""
import os, json, sys
sys.path.insert(0, os.path.expanduser("~/Documents/Dev/AppleBridge_Working"))
from mcp import tools
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
res = tools.call_tool(sys.argv[1], args)
print(json.dumps(res, indent=1, default=str)[:20000])
