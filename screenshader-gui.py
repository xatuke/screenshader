#!/usr/bin/env python3
"""
screenshader-gui - Visual shader browser with live preview.

Displays a list of available shaders. Selecting a shader opens a live
preview window showing the effect applied to the desktop in real-time.
Uses screenshader-preview (C helper) for GPU-accelerated rendering.

Requires: Python 3, tkinter, screenshader-preview binary.
"""

import os
import subprocess
import signal
import threading
import tkinter as tk


class ShaderGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("screenshader")
        self.root.geometry("280x600")
        self.root.minsize(250, 400)

        # Paths
        self.script_dir = os.path.dirname(os.path.abspath(__file__))
        self.preview_bin = os.path.join(self.script_dir, "screenshader-preview")
        self.launcher = os.path.join(self.script_dir, "screenshader.sh")
        self.shaders_dir = os.path.join(self.script_dir, "shaders")

        if not os.path.isfile(self.preview_bin):
            self._fatal("screenshader-preview not found. Run 'make' first.")
            return

        # State
        self.live_process = None
        self.current_shader = None

        self._build_ui()
        self._load_shaders()

        # Keyboard shortcuts
        self.root.bind("<Return>", lambda e: self.apply_shader())
        self.root.bind("<Escape>", lambda e: self.stop_shader())

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _fatal(self, msg):
        frame = tk.Frame(self.root, padx=20, pady=20)
        frame.pack(fill=tk.BOTH, expand=True)
        tk.Label(frame, text=msg, fg="red", font=("monospace", 12)).pack()

    def _build_ui(self):
        main = tk.Frame(self.root, padx=8, pady=8)
        main.pack(fill=tk.BOTH, expand=True)

        tk.Label(main, text="Shaders:", anchor=tk.W,
                 font=("sans-serif", 10, "bold")).pack(fill=tk.X)

        list_frame = tk.Frame(main)
        list_frame.pack(fill=tk.BOTH, expand=True, pady=(4, 8))

        scrollbar = tk.Scrollbar(list_frame)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        self.shader_list = tk.Listbox(list_frame, exportselection=False,
                                      yscrollcommand=scrollbar.set,
                                      font=("monospace", 11),
                                      activestyle="none",
                                      selectbackground="#4a90d9",
                                      selectforeground="white")
        self.shader_list.pack(fill=tk.BOTH, expand=True)
        scrollbar.config(command=self.shader_list.yview)
        self.shader_list.bind("<<ListboxSelect>>", self._on_select)

        btn_frame = tk.Frame(main)
        btn_frame.pack(fill=tk.X)

        self.apply_btn = tk.Button(btn_frame, text="Apply",
                                   command=self.apply_shader,
                                   bg="#4a90d9", fg="white",
                                   font=("sans-serif", 10, "bold"))
        self.apply_btn.pack(fill=tk.X, pady=(0, 4))

        self.stop_btn = tk.Button(btn_frame, text="Stop",
                                  command=self.stop_shader)
        self.stop_btn.pack(fill=tk.X, pady=(0, 4))

        self.status_var = tk.StringVar(value="Select a shader to preview")
        tk.Label(main, textvariable=self.status_var,
                 fg="#666666", anchor=tk.W, wraplength=250,
                 font=("monospace", 9)).pack(fill=tk.X)

    def _load_shaders(self):
        names = []
        for f in sorted(os.listdir(self.shaders_dir)):
            if f.endswith(".frag"):
                name = f[:-5]
                if name != "composite":
                    names.append(name)
        self.shader_names = names
        for name in names:
            self.shader_list.insert(tk.END, name)

    # --- Live preview ---

    def _kill_preview(self):
        if self.live_process and self.live_process.poll() is None:
            self.live_process.terminate()
            try:
                self.live_process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.live_process.kill()
        self.live_process = None

    def _on_select(self, event):
        sel = self.shader_list.curselection()
        if not sel:
            return
        name = self.shader_names[sel[0]]
        if name == self.current_shader and self.live_process \
                and self.live_process.poll() is None:
            return  # already showing this shader
        self.current_shader = name
        self._start_live(name)

    def _start_live(self, shader_name):
        self._kill_preview()

        shader_path = os.path.join(self.shaders_dir, shader_name + ".frag")
        self.status_var.set(f"Preview: {shader_name}")

        self.live_process = subprocess.Popen(
            [self.preview_bin, shader_path, "--live", "--fps", "30"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )

    # --- Apply / Stop ---

    def apply_shader(self):
        if not self.current_shader:
            self.status_var.set("Select a shader first.")
            return

        # Kill preview window so it doesn't interfere
        self._kill_preview()

        shader_path = os.path.join(self.shaders_dir, self.current_shader + ".frag")
        self.status_var.set(f"Applying: {self.current_shader}...")

        def do_apply():
            try:
                proc = subprocess.Popen(
                    [self.launcher, shader_path],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE
                )
                # Auto-confirm after 2 seconds
                import time
                time.sleep(2)
                try:
                    proc.stdin.write(b"c")
                    proc.stdin.flush()
                except Exception:
                    pass
            except Exception as e:
                self.root.after(0, lambda: self.status_var.set(f"Error: {e}"))
                return
            self.root.after(0, lambda: self.status_var.set(
                f"Active: {self.current_shader}"))

        threading.Thread(target=do_apply, daemon=True).start()

    def stop_shader(self):
        self._kill_preview()
        self.status_var.set("Stopping...")
        try:
            subprocess.run([self.launcher, "--stop"],
                           capture_output=True, timeout=5)
            self.status_var.set("Stopped.")
        except Exception as e:
            self.status_var.set(f"Error stopping: {e}")

    # --- Cleanup ---

    def _on_close(self):
        self._kill_preview()
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    app = ShaderGUI(root)
    root.mainloop()
