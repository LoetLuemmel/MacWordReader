#!/bin/zsh
# deploy_duo.sh — fetch the built apps from dev2019 (MacBinary) and install them on the Duo
cd "${0:A:h}/.."
D='Duo 120 MB:WordReader:'
(( $# )) || set -- WordReader DuoShot
for a in "$@"; do
  uv run python tools/ab.py mac_get_file "{\"guest\":\"dev2019\",\"mac_path\":\"MeinMac:MPW:WordReader:$a\",\"host_path\":\"$PWD/build/$a.bin\",\"format\":\"macbinary\"}" | grep -q '"success": true' || { echo "fetch $a failed"; exit 1; }
  uv run python tools/ab.py mpw_execute "{\"guest\":\"duo\",\"command\":\"Set Exit 0; Delete -i '${D}${a}'\"}" >/dev/null
  uv run python tools/ab.py mac_put_file "{\"guest\":\"duo\",\"host_path\":\"$PWD/build/$a.bin\",\"mac_path\":\"${D}${a}\"}" | grep -q '"success": true' || { echo "put $a failed"; exit 1; }
  echo "ok   $a -> Duo"
done
uv run python tools/ab.py mpw_execute "{\"guest\":\"duo\",\"command\":\"SetFile -a B '${D}WordReader'; Files -x abkt '${D}'\"}" | python3 -c 'import json,sys;print(json.load(sys.stdin)["output"])'
