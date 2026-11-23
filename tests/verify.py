"""verify.py files... — inflate must be byte-exact; text must match an independent extraction."""
import re, subprocess, sys, zipfile, unicodedata
WRCAT = __file__.rsplit("/", 1)[0] + "/wrcat"
bad = 0
for path in sys.argv[1:]:
    try:
        z = zipfile.ZipFile(path)
    except zipfile.BadZipFile:
        continue
    for info in z.infolist():                      # every member, not just document.xml
        if info.file_size > 3_000_000: continue
        got = subprocess.run([WRCAT, "-member", info.filename, path], capture_output=True).stdout
        if got != z.read(info):
            print("INFLATE MISMATCH", path, info.filename, len(got), info.file_size); bad += 1
    xml = z.read("word/document.xml").decode("utf-8")
    xml = re.sub(r"<mc:Fallback>.*?</mc:Fallback>", "", xml, flags=re.S)
    ref = "".join(re.findall(r"<w:t(?:\s[^>]*)?>([^<]*)</w:t>", xml))
    ref = ref.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", '"').replace("&apos;", "'")
    out = subprocess.run([WRCAT, path], capture_output=True).stdout.decode("utf-8")
    key = lambda s: re.sub(r"[^A-Za-z0-9]", "", unicodedata.normalize("NFC", s))
    a, b = key(ref), key(out.replace("[Picture]", ""))
    if a != b:
        i = next((k for k in range(min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
        print("TEXT MISMATCH", path, len(a), len(b), repr(a[i-30:i+30]), repr(b[i-30:i+30])); bad += 1
    print("ok" if not bad else "", path.rsplit("/",1)[1], len(z.infolist()), "members")
sys.exit(1 if bad else 0)
