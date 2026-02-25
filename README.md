# screenshader

Real-time screen shader overlay for your desktop. Applies GLSL post-processing effects (CRT, glitch, thermal, VHS, etc.) to your entire display.

## Platforms

- **Linux/X11** — C + XComposite + OpenGL
- **macOS** — Swift + ScreenCaptureKit + Metal (auto GLSL→MSL conversion)
- **Hyprland** — native `decoration:screen_shader` integration

## Shaders

20 built-in effects in `shaders/`:

`crt` `amber` `green` `glitch` `vhs` `thermal` `matrix` `sketch` `dreamy` `oilpaint` `underwater` `anime` `nightlight` `neon` `halftone` `filmgrain` `pixelate` `marvel` `lcd` `tilt_shift`

## Build

```
make          # auto-detects platform
make macos    # macOS only
make x11      # Linux/X11 only
```

**Dependencies:**
- macOS: Xcode command line tools (swiftc)
- X11: `libx11 libxcomposite libxdamage libxfixes libxrender libgl pkg-config gcc`

## Usage

```
./screenshader.sh                        # start with default CRT shader
./screenshader.sh shaders/amber.frag     # pick a shader
./screenshader.sh --list                 # list available shaders
./screenshader.sh --stop                 # stop the overlay
./screenshader.sh --reload               # hot-reload current shader
./screenshader.sh --set u_curvature 0.1  # tweak a shader parameter
./screenshader.sh --get                  # show current parameters
```

A GUI shader browser is also available:

```
./screenshader-gui.py
```

## Writing Custom Shaders

Drop a `.frag` file in `shaders/`. Available uniforms:

```glsl
uniform sampler2D u_screen;    // captured screen texture
uniform vec2      u_resolution; // screen resolution
uniform float     u_time;       // elapsed time (for animation)
```

Input: `vec2 v_texcoord` — Output: `vec4 frag_color`

## Notes

- macOS requires Screen Recording permission (System Settings → Privacy & Security)
- Shaders hot-reload on file save (macOS) or via `--reload` / `SIGUSR1`
- Runtime parameters are stored in `/tmp/screenshader.params`
