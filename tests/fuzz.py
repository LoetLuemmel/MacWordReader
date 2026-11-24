"""fuzz.py N files... — damage documents and pictures at random; the reader must never crash.
Pictures (.png .jpg .jpeg) go through the decoder (wrcat -img) at a random size."""
import os, random, subprocess, sys, tempfile
n = int(sys.argv[1]); files = sys.argv[2:]; crashes = 0; codes = {}
exe = os.path.join(os.path.dirname(__file__), "wrcat_asan")
tmp = tempfile.mkdtemp()
random.seed(1992)
for i in range(n):
    b = bytearray(open(random.choice(files), "rb").read())
    for _ in range(random.choice([1, 2, 8, 64])):
        if len(b) < 2: break
        k = random.randrange(len(b))
        if random.random() < 0.8: b[k] = random.randrange(256)
        else: del b[k:k + random.randrange(1, 200)]
    if random.random() < 0.1 and len(b) > 1: b = b[:random.randrange(len(b))]
    p = os.path.join(tmp, "f.docx"); open(p, "wb").write(b)
    if b[:4] in (b"\x89PNG",) or b[:2] == b"\xff\xd8":
        args = ["-img", os.path.join(tmp, "o.pgm"), str(random.randrange(1, 900)), str(random.randrange(1, 700)), p]
    else:
        args = [p]
    r = subprocess.run([exe] + args, capture_output=True)
    codes[r.returncode] = codes.get(r.returncode, 0) + 1
    if r.returncode < 0 or b"ERROR: AddressSanitizer" in r.stderr or b"runtime error" in r.stderr:
        crashes += 1; open(os.path.join(tmp, f"crash{crashes}.docx"), "wb").write(b)
        print("CRASH", r.returncode, r.stderr.decode()[-600:])
print("runs", n, "crashes", crashes, "exit codes", dict(sorted(codes.items())), "kept in", tmp)
