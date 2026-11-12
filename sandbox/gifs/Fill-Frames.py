# Fills skipped capture slots so ffmpeg gets a gap-free sequence: Fill-Frames.py <scene> <results\gifs folder>
import sys, os, shutil, re
G = sys.argv[2]  # the guest results folder that holds gifs\<scene>
S = os.path.dirname(os.path.abspath(__file__))
scene = sys.argv[1]
src = os.path.join(G, scene); dst = os.path.join(S, "seq", scene)
shutil.rmtree(dst, ignore_errors=True); os.makedirs(dst)
slots = sorted(int(re.match(r"f(\d+)\.png", f).group(1)) for f in os.listdir(src) if re.match(r"f\d+\.png", f))
have = set(slots); last = None; n = 0
for s in range(slots[0], slots[-1] + 1):
    if s in have: last = s
    shutil.copyfile(os.path.join(src, "f%04d.png" % last), os.path.join(dst, "%04d.png" % n)); n += 1
print(scene, "frames", n, "captured", len(slots))
