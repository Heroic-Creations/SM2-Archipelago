"""The Archipelago connect window for Marvel's Spider-Man 2.

A small always-on-top window, separate from the game. Type the connection in,
hit Connect, watch the dot.

It does not talk to Archipelago itself -- `client.py` owns the socket and the
save watcher. This window only writes what you typed and reads back the status:

    connect_ui  --(ap-connect.json)-->  client.py  --(websocket)-->  AP server
    connect_ui  <--(ap-status.txt)----  client.py

That split is deliberate. The first version put these fields *inside* the game
as an overlay panel, but giving an overlay keyboard focus drops the game out of
exclusive fullscreen -- the whole game window shrank. A separate window never
touches the game's focus at all.
"""
import json
import os
import subprocess
import sys
import time
import tkinter as tk

import paths

CAP = paths.RUNTIME
CONNECT = paths.runtime("ap-connect.json")
STATUS = paths.runtime("ap-status.txt")
REMEMBER = paths.runtime("ap-last-connection.json")
ALIVE = paths.runtime("ap-client-alive.txt")
CLIENT = os.path.join(paths.HERE, "client.py")

# Suit-AI palette, same family as the in-game feed.
BG      = "#0b1016"
PANEL   = "#131c26"
EDGE    = "#22303e"
TEXT    = "#c9dbe8"
DIM     = "#6d8598"
CYAN    = "#5ae2ff"
GREEN   = "#5aeb82"
RED     = "#eb5a5a"
AMBER   = "#e8b455"


def load_remembered():
    try:
        with open(REMEMBER) as f:
            return json.load(f)
    except Exception:
        return {"host": "localhost:38281", "slot": "", "password": ""}


class App:
    def __init__(self, root):
        self.root = root
        self.last_status = None
        root.title("Spider-Man 2 - Archipelago")
        root.configure(bg=BG)
        root.attributes("-topmost", True)
        root.resizable(False, False)

        saved = load_remembered()
        self.vars = {}

        wrap = tk.Frame(root, bg=BG, padx=18, pady=16)
        wrap.pack(fill="both", expand=True)

        tk.Label(wrap, text="ARCHIPELAGO", bg=BG, fg=CYAN,
                 font=("Consolas", 13, "bold")).pack(anchor="w")
        tk.Label(wrap, text="Marvel's Spider-Man 2", bg=BG, fg=DIM,
                 font=("Segoe UI", 9)).pack(anchor="w", pady=(0, 12))

        for key, label, secret in (("host", "Server", False),
                                   ("slot", "Player / slot", False),
                                   ("password", "Password", True)):
            tk.Label(wrap, text=label.upper(), bg=BG, fg=DIM,
                     font=("Segoe UI", 8, "bold")).pack(anchor="w")
            var = tk.StringVar(value=saved.get(key, ""))
            e = tk.Entry(wrap, textvariable=var, width=32, bg=PANEL, fg=TEXT,
                         insertbackground=CYAN, relief="flat", highlightthickness=1,
                         highlightbackground=EDGE, highlightcolor=CYAN,
                         font=("Consolas", 11), show="*" if secret else "")
            e.pack(fill="x", ipady=5, pady=(2, 10))
            e.bind("<Return>", lambda _e: self.connect())
            self.vars[key] = var

        self.button = tk.Button(wrap, text="CONNECT", command=self.connect,
                                bg=CYAN, fg="#06121a", activebackground="#8fecff",
                                activeforeground="#06121a", relief="flat",
                                font=("Segoe UI", 10, "bold"), cursor="hand2")
        self.button.pack(fill="x", ipady=6, pady=(2, 14))

        row = tk.Frame(wrap, bg=BG)
        row.pack(fill="x")
        self.dot = tk.Canvas(row, width=16, height=16, bg=BG,
                             highlightthickness=0)
        self.dot.pack(side="left")
        self.blob = self.dot.create_oval(3, 3, 13, 13, fill=RED, outline="")
        self.status = tk.Label(row, text="starting...", bg=BG, fg=DIM,
                               font=("Segoe UI", 9), anchor="w",
                               wraplength=250, justify="left")
        self.status.pack(side="left", padx=(8, 0), fill="x", expand=True)

        self.ensure_client()
        self.poll()

    # --- the other half of the pipe -------------------------------------------

    def ensure_client(self):
        """Start client.py if it is not already up.

        Without it nothing reads ap-connect.json, so Connect would look like it
        did nothing at all -- the most confusing possible failure.
        """
        # The client touches ALIVE every two seconds. Checking a file's age is
        # instant and silent; the previous check shelled out to wmic, which
        # flashed a console window on screen and could not see Store Python
        # (it runs as python3.11.exe) -- so it started a second client every
        # time and the extra one fought over the same output files.
        try:
            if time.time() - os.path.getmtime(ALIVE) < 10:
                return
        except OSError:
            pass
        try:
            subprocess.Popen([sys.executable, "-u", CLIENT], cwd=paths.HERE,
                             stdout=open(paths.runtime("client-out.txt"), "w"),
                             stderr=open(paths.runtime("client-err.txt"), "w"),
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except Exception as e:
            self.set_dot(RED, "could not start client.py: %s" % e)

    def connect(self):
        cfg = {k: v.get().strip() for k, v in self.vars.items()}
        if not cfg["host"] or not cfg["slot"]:
            self.set_dot(AMBER, "server and player are both required")
            return
        with open(CONNECT, "w") as f:
            json.dump(cfg, f)
        with open(REMEMBER, "w") as f:
            json.dump(cfg, f)
        self.ensure_client()
        self.set_dot(AMBER, "connecting to %s..." % cfg["host"])

    # --- status ---------------------------------------------------------------

    def set_dot(self, colour, text):
        self.dot.itemconfig(self.blob, fill=colour)
        self.status.config(text=text, fg=TEXT if colour == GREEN else DIM)

    def poll(self):
        try:
            with open(STATUS) as f:
                line = f.read().strip()
        except OSError:
            line = ""
        if line and line != self.last_status:
            self.last_status = line
            low = line.lower()
            if low.startswith("connected"):
                colour = GREEN
            elif low.startswith("connecting") or low.startswith("waiting"):
                colour = AMBER
            else:
                colour = RED
            self.set_dot(colour, line)
        self.root.after(500, self.poll)


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
