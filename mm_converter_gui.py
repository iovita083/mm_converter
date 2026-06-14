"""
mm_converter_gui.py
GUI wrapper for mm_converter.exe  --  stdlib only (tkinter + threading)
"""

import os
import re
import subprocess
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

# ── helpers ──────────────────────────────────────────────────────────────────

def find_pv_db(mod_root: str) -> str | None:
    for rel in ("rom/mod_pv_db.txt", "mod_pv_db.txt", "rom/pv_db.txt"):
        p = os.path.join(mod_root, rel.replace("/", os.sep))
        if os.path.isfile(p):
            return p
    return None


def parse_songs(pv_db_path: str) -> list[tuple[int, str]]:
    """Return [(pv_id, display_name), ...] sorted by id."""
    entries: dict[int, dict] = {}
    with open(pv_db_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, _, v = line.partition("=")
            k, v = k.strip(), v.strip()
            if not k.startswith("pv_"):
                continue
            dot = k.find(".", 3)
            if dot == -1:
                continue
            try:
                pid = int(k[3:dot])
            except ValueError:
                continue
            field = k[dot + 1:]
            e = entries.setdefault(pid, {})
            if field == "song_name_en":
                e["en"] = v
            elif field == "song_name":
                e["jp"] = v
    result = []
    for pid, e in entries.items():
        name = e.get("en") or e.get("jp") or f"pv_{pid}"
        result.append((pid, name))
    result.sort(key=lambda x: x[0])
    return result


def collect_mod_roots(path: str) -> list[str]:
    """Return a list of mod roots under path (path itself, or subdirs that contain rom/)."""
    if find_pv_db(path):
        return [path]
    roots = []
    for entry in sorted(os.scandir(path), key=lambda e: e.name):
        if entry.is_dir() and find_pv_db(entry.path):
            roots.append(entry.path)
    return roots


# ── main window ──────────────────────────────────────────────────────────────

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("mm_converter GUI")
        self.minsize(720, 560)
        self.resizable(True, True)

        # State
        self._songs: list[tuple[int, str, str]] = []  # (pv_id, name, mod_root)
        self._vars:  list[tk.BooleanVar] = []
        self._running = False

        self._build_ui()

    # ── UI construction ───────────────────────────────────────────────────────

    def _build_ui(self):
        pad = dict(padx=8, pady=4)

        # ── Row 1: mods folder
        f1 = tk.Frame(self)
        f1.pack(fill="x", **pad)
        tk.Label(f1, text="mods folder:").pack(side="left")
        self._mod_var = tk.StringVar()
        tk.Entry(f1, textvariable=self._mod_var, width=40).pack(side="left", padx=4)
        tk.Button(f1, text="Browse…", command=self._browse_batch).pack(side="left")

        # ── Row 2: output folder
        f2 = tk.Frame(self)
        f2.pack(fill="x", **pad)
        tk.Label(f2, text="Output folder:").pack(side="left")
        self._out_var = tk.StringVar()
        tk.Entry(f2, textvariable=self._out_var, width=40).pack(side="left", padx=4)
        tk.Button(f2, text="Browse…", command=self._browse_out).pack(side="left")

        # ── Row 3: extra options
        f3 = tk.Frame(self)
        f3.pack(fill="x", **pad)
        self._skip_video = tk.BooleanVar(value=False)
        tk.Checkbutton(f3, text="Skip video", variable=self._skip_video).pack(side="left")

        # ── Row 4: search + song list
        f4 = tk.Frame(self)
        f4.pack(fill="both", expand=True, **pad)

        search_bar = tk.Frame(f4)
        search_bar.pack(fill="x")
        tk.Label(search_bar, text="Search:").pack(side="left")
        self._search_var = tk.StringVar()
        self._search_var.trace_add("write", lambda *_: self._apply_filter())
        tk.Entry(search_bar, textvariable=self._search_var, width=30).pack(side="left", padx=4)
        tk.Button(search_bar, text="Select all", command=self._select_all).pack(side="left", padx=2)
        tk.Button(search_bar, text="Deselect all", command=self._deselect_all).pack(side="left")

        # Scrollable checklist
        list_frame = tk.Frame(f4, relief="sunken", bd=1)
        list_frame.pack(fill="both", expand=True, pady=4)
        self._canvas = tk.Canvas(list_frame, highlightthickness=0)
        vsb = ttk.Scrollbar(list_frame, orient="vertical", command=self._canvas.yview)
        self._canvas.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right", fill="y")
        self._canvas.pack(side="left", fill="both", expand=True)
        self._inner = tk.Frame(self._canvas)
        self._canvas_win = self._canvas.create_window((0, 0), window=self._inner, anchor="nw")
        self._inner.bind("<Configure>", lambda e: self._canvas.configure(
            scrollregion=self._canvas.bbox("all")))
        self._canvas.bind("<Configure>", lambda e: self._canvas.itemconfig(
            self._canvas_win, width=e.width))
        # Mouse-wheel scroll
        self._canvas.bind_all("<MouseWheel>",
            lambda e: self._canvas.yview_scroll(int(-1 * (e.delta / 120)), "units"))

        # ── Row 5: run button
        f5 = tk.Frame(self)
        f5.pack(fill="x", **pad)
        self._run_btn = tk.Button(f5, text="Convert selected", command=self._run,
                                  bg="#2d7d46", fg="white", font=("", 10, "bold"))
        self._run_btn.pack(side="left")
        self._status_lbl = tk.Label(f5, text="", anchor="w")
        self._status_lbl.pack(side="left", padx=8)

        # ── Row 6: log
        log_frame = tk.Frame(self)
        log_frame.pack(fill="both", expand=False, **pad)
        tk.Label(log_frame, text="Log:", anchor="w").pack(fill="x")
        self._log = tk.Text(log_frame, height=8, state="disabled",
                            font=("Consolas", 9), wrap="word")
        log_sb = ttk.Scrollbar(log_frame, command=self._log.yview)
        self._log.configure(yscrollcommand=log_sb.set)
        log_sb.pack(side="right", fill="y")
        self._log.pack(fill="both", expand=True)

    # ── Browse callbacks ──────────────────────────────────────────────────────

    def _browse_out(self):
        p = filedialog.askdirectory(title="Select output folder")
        if p:
            self._out_var.set(p)

    def _browse_batch(self):
        p = filedialog.askdirectory(title="Select parent folder containing multiple mods")
        if p:
            self._mod_var.set(p)
            self._load_songs(p)

    # ── Song list management ──────────────────────────────────────────────────

    def _load_songs(self, path: str):
        self._songs.clear()
        self._vars.clear()

        roots = collect_mod_roots(path)
        if not roots:
            messagebox.showwarning("No mods found",
                "Could not find any mod_pv_db.txt under the selected path.")
            return

        for mod_root in roots:
            db = find_pv_db(mod_root)
            if not db:
                continue
            try:
                songs = parse_songs(db)
            except Exception as exc:
                self._log_write(f"WARN: failed parsing {db}: {exc}\n")
                continue
            for pid, name in songs:
                self._songs.append((pid, name, mod_root))
                self._vars.append(tk.BooleanVar(value=True))

        self._apply_filter()
        self._log_write(f"Loaded {len(self._songs)} songs from {len(roots)} mod(s).\n")

    def _apply_filter(self):
        q = self._search_var.get().lower()
        for w in self._inner.winfo_children():
            w.destroy()

        for i, (pid, name, mod_root) in enumerate(self._songs):
            label = f"[pv_{pid:04d}]  {name}"
            if mod_root != self._mod_var.get():
                label += f"  ({os.path.basename(mod_root)})"
            if q and q not in label.lower():
                continue
            cb = tk.Checkbutton(self._inner, text=label, variable=self._vars[i],
                               anchor="w", justify="left")
            cb.pack(fill="x", padx=4)

    def _select_all(self):
        q = self._search_var.get().lower()
        for i, (pid, name, mod_root) in enumerate(self._songs):
            label = f"[pv_{pid:04d}]  {name}"
            if q and q not in label.lower():
                continue
            self._vars[i].set(True)

    def _deselect_all(self):
        q = self._search_var.get().lower()
        for i, (pid, name, mod_root) in enumerate(self._songs):
            label = f"[pv_{pid:04d}]  {name}"
            if q and q not in label.lower():
                continue
            self._vars[i].set(False)

    # ── Conversion ────────────────────────────────────────────────────────────

    def _run(self):
        if self._running:
            return

        exe = "mm_converter.exe"
        out = self._out_var.get().strip()
        
        if not out:
            messagebox.showerror("Error", "No output folder specified.")
            return

        # Group selected songs by mod_root
        jobs: dict[str, list[int]] = {}
        for i, (pid, name, mod_root) in enumerate(self._songs):
            if self._vars[i].get():
                jobs.setdefault(mod_root, []).append(pid)

        if not jobs:
            messagebox.showinfo("Nothing selected", "Select at least one song.")
            return

        self._running = True
        self._run_btn.config(state="disabled")
        self._status_lbl.config(text="Running…")
        threading.Thread(target=self._run_thread,
                         args=(exe, out, jobs), daemon=True).start()

    def _run_thread(self, exe: str, out_root: str, jobs: dict):
        try:
            total = sum(len(v) for v in jobs.values())
            done = 0
            for mod_root, pv_ids in jobs.items():
                cmd = [exe, mod_root, out_root]
                if self._skip_video.get():
                    cmd.append("--skip-video")
                for pid in pv_ids:
                    cmd += ["--pv", str(pid)]

                self._log_write(f"\n$ {' '.join(cmd)}\n")
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT,
                                        text=True, encoding="utf-8", errors="replace")
                for line in proc.stdout:
                    self._log_write(line)
                proc.wait()
                done += len(pv_ids)
                self.after(0, self._status_lbl.config,
                            {"text": f"{done}/{total} songs processed"})

            self._log_write("\nAll done.\n")
        except Exception as exc:
            self._log_write(f"\nERROR: {exc}\n")
        finally:
            self._running = False
            self.after(0, self._run_btn.config, {"state": "normal"})

    # ── Logging ───────────────────────────────────────────────────────────────

    def _log_write(self, text: str):
        def _do():
            self._log.config(state="normal")
            self._log.insert("end", text)
            self._log.see("end")
            self._log.config(state="disabled")
        self.after(0, _do)


# ── entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    app = App()
    app.mainloop()