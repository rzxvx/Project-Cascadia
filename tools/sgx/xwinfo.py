#!/usr/bin/env python3
"""xwinfo.py WINDOW: a window and its parents up to the root -- size, depth,
override-redirect, map state, _NET_WM_BYPASS_COMPOSITOR -- through libX11
(the iPad has no xprop or xwininfo)."""
import ctypes
import sys

X = ctypes.CDLL("libX11.so.6")
X.XOpenDisplay.restype = ctypes.c_void_p
d = X.XOpenDisplay(None)


class Attr(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int),
                ("height", ctypes.c_int), ("border_width", ctypes.c_int), ("depth", ctypes.c_int),
                ("visual", ctypes.c_void_p), ("root", ctypes.c_ulong), ("klass", ctypes.c_int),
                ("bit_gravity", ctypes.c_int), ("win_gravity", ctypes.c_int),
                ("backing_store", ctypes.c_int), ("backing_planes", ctypes.c_ulong),
                ("backing_pixel", ctypes.c_ulong), ("save_under", ctypes.c_int),
                ("colormap", ctypes.c_ulong), ("map_installed", ctypes.c_int),
                ("map_state", ctypes.c_int), ("all_event_masks", ctypes.c_long),
                ("your_event_mask", ctypes.c_long), ("do_not_propagate_mask", ctypes.c_long),
                ("override_redirect", ctypes.c_int), ("screen", ctypes.c_void_p)]


X.XGetWindowAttributes.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(Attr)]
PUL = ctypes.POINTER(ctypes.c_ulong)
X.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, PUL, PUL,
                         ctypes.POINTER(PUL), ctypes.POINTER(ctypes.c_uint)]
X.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
X.XInternAtom.restype = ctypes.c_ulong
X.XGetWindowProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long,
                                 ctypes.c_long, ctypes.c_int, ctypes.c_ulong, PUL,
                                 ctypes.POINTER(ctypes.c_int), PUL, PUL,
                                 ctypes.POINTER(ctypes.c_void_p)]


def prop(win, name):
    a = X.XInternAtom(d, name.encode(), 1)
    if not a:
        return None
    t, f, n, b, p = ctypes.c_ulong(), ctypes.c_int(), ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_void_p()
    if X.XGetWindowProperty(d, win, a, 0, 16, 0, 0, ctypes.byref(t), ctypes.byref(f),
                            ctypes.byref(n), ctypes.byref(b), ctypes.byref(p)) or not n.value:
        return None
    if f.value == 32:
        return [ctypes.cast(p, ctypes.POINTER(ctypes.c_long))[i] for i in range(n.value)]
    return n.value


chain = [int(sys.argv[1], 0)]
root, parent = ctypes.c_ulong(), ctypes.c_ulong()
kids, nk = PUL(), ctypes.c_uint()
while True:
    X.XQueryTree(d, chain[-1], ctypes.byref(root), ctypes.byref(parent), ctypes.byref(kids),
                 ctypes.byref(nk))
    if not parent.value or parent.value == root.value:
        break
    chain.append(parent.value)
for depth, win in enumerate(reversed(chain)):
    a = Attr()
    X.XGetWindowAttributes(d, win, ctypes.byref(a))
    print("%s0x%x %dx%d%+d%+d depth %d override %d map %d bypass %s state %s" % (
        "  " * depth, win, a.width, a.height, a.x, a.y, a.depth, a.override_redirect,
        a.map_state, prop(win, "_NET_WM_BYPASS_COMPOSITOR"), prop(win, "_NET_WM_STATE")))
