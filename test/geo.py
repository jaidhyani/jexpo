"""Where jexpo puts things on the nested 1280x720 output (the same arithmetic as expo.cpp), for
stress: geo.py center|badge|corner <workspace 1-9> prints x y."""
import sys
MX, MY, MARGIN, GAP, HINT = 1280, 720, 56, 30, 40
W, H = MX - 2*MARGIN, MY - 2*MARGIN - HINT
tw = min((W - 2*GAP)/3, (H - 2*GAP)/3 * MX/MY); th = tw*MY/MX
x0 = (MX - (3*tw + 2*GAP))/2; y0 = (MY - HINT - (3*th + 2*GAP))/2
def slot(s): return (x0 + (s%3)*(tw+GAP), y0 + (s//3)*(th+GAP))
what, s = sys.argv[1], int(sys.argv[2]) - 1
x, y = slot(s)
if what == "center": print(int(x + tw/2), int(y + th/2))
elif what == "badge": print(int(x + 28), int(y + 28))
elif what == "corner": print(int(x + tw - 10), int(y + th - 10))
