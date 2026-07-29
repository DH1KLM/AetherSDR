#!/usr/bin/env python3
"""Minimal X automation via python-xlib + XTEST, for driving VARA headlessly.

Exists because xdotool is not installed and installing it needs a password.
Only what is needed: enumerate windows, move/click, type, screenshot trigger.

Usage:
  xdrive.py list                      list windows on the display
  xdrive.py click X Y                 click at root coordinates
  xdrive.py key KEYSYM                press a key (e.g. Escape, Return)
  xdrive.py tree                      dump window geometry tree
"""
import sys
import time

from Xlib import X, display, XK
from Xlib.ext import xtest

DISPLAY = ":99"


def get_display():
    return display.Display(DISPLAY)


def walk(win, depth=0, out=None):
    if out is None:
        out = []
    try:
        name = win.get_wm_name()
        geom = win.get_geometry()
        attrs = win.get_attributes()
        if name and attrs.map_state == X.IsViewable:
            abs_x, abs_y = absolute_position(win)
            out.append((depth, name, abs_x, abs_y, geom.width, geom.height))
        for child in win.query_tree().children:
            walk(child, depth + 1, out)
    except Exception:
        pass
    return out


def absolute_position(win):
    x = y = 0
    try:
        while True:
            geom = win.get_geometry()
            x += geom.x
            y += geom.y
            parent = win.query_tree().parent
            if parent is None or parent == win:
                break
            win = parent
    except Exception:
        pass
    return x, y


def cmd_list(d):
    root = d.screen().root
    for depth, name, x, y, w, h in walk(root):
        print(f"{'  ' * depth}{name!r}  @{x},{y} {w}x{h}")


def cmd_click(d, x, y, button=1):
    xtest.fake_input(d, X.MotionNotify, x=int(x), y=int(y))
    d.sync()
    time.sleep(0.25)
    xtest.fake_input(d, X.ButtonPress, button)
    d.sync()
    time.sleep(0.08)
    xtest.fake_input(d, X.ButtonRelease, button)
    d.sync()
    time.sleep(0.4)
    print(f"clicked {x},{y}")


def cmd_key(d, keyname):
    keysym = XK.string_to_keysym(keyname)
    if keysym == 0:
        print(f"unknown keysym {keyname}", file=sys.stderr)
        return
    keycode = d.keysym_to_keycode(keysym)
    xtest.fake_input(d, X.KeyPress, keycode)
    d.sync()
    time.sleep(0.05)
    xtest.fake_input(d, X.KeyRelease, keycode)
    d.sync()
    time.sleep(0.3)
    print(f"pressed {keyname}")


def cmd_type(d, text):
    """Type an ASCII string. Keysym names for printable chars are the chars
    themselves; uppercase needs an explicit Shift because the keymap entry is
    the lowercase keycode."""
    for ch in text:
        if ch.isupper():
            shift = d.keysym_to_keycode(XK.string_to_keysym("Shift_L"))
            keysym = XK.string_to_keysym(ch.lower())
            keycode = d.keysym_to_keycode(keysym)
            xtest.fake_input(d, X.KeyPress, shift)
            xtest.fake_input(d, X.KeyPress, keycode)
            xtest.fake_input(d, X.KeyRelease, keycode)
            xtest.fake_input(d, X.KeyRelease, shift)
        else:
            name = {"-": "minus", "_": "underscore", " ": "space",
                    "/": "slash", ".": "period", "@": "at"}.get(ch, ch)
            keysym = XK.string_to_keysym(name)
            if keysym == 0:
                continue
            keycode = d.keysym_to_keycode(keysym)
            xtest.fake_input(d, X.KeyPress, keycode)
            xtest.fake_input(d, X.KeyRelease, keycode)
        d.sync()
        time.sleep(0.05)
    print(f"typed {text!r}")


def main():
    global DISPLAY
    args = sys.argv[1:]
    if args and args[0].startswith(":"):
        DISPLAY = args.pop(0)
    if not args:
        print(__doc__)
        return 1
    d = get_display()
    cmd = args[0]
    if cmd == "list" or cmd == "tree":
        cmd_list(d)
    elif cmd == "click":
        cmd_click(d, int(args[1]), int(args[2]),
                  int(args[3]) if len(args) > 3 else 1)
    elif cmd == "key":
        cmd_key(d, args[1])
    elif cmd == "type":
        cmd_type(d, " ".join(args[1:]))
    else:
        print(f"unknown command {cmd}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
