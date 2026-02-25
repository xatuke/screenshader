#!/bin/bash
#
# screenshader.sh - Toggle screenshader compositor
#
# Supports X11 (compositor), Hyprland (decoration:screen_shader), and macOS (Metal overlay).
#
# Usage:
#   screenshader.sh [shader.frag]   Start with a shader (default: shaders/crt.frag)
#   screenshader.sh --stop          Stop the running compositor
#   screenshader.sh --reload        Hot-reload the current shader
#   screenshader.sh --list          List available shaders
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIDFILE="/tmp/screenshader.pid"
PARAM_FILE="/tmp/screenshader.params"

# ---------------------------------------------------------------------------
# Platform detection
# ---------------------------------------------------------------------------
detect_platform() {
    if [ "$(uname)" = "Darwin" ]; then
        echo "macos"
    elif [ -n "$HYPRLAND_INSTANCE_SIGNATURE" ]; then
        echo "hyprland"
    elif [ -n "$DISPLAY" ]; then
        echo "x11"
    else
        echo "unsupported"
    fi
}

PLATFORM="$(detect_platform)"

# ---------------------------------------------------------------------------
# Hyprland: convert GLSL 330 core → Hyprland's GLSL ES dialect
# ---------------------------------------------------------------------------
convert_hyprland() {
    sed -e '/#version/d' \
        -e 's/^in vec2 v_texcoord;/varying vec2 v_texcoord;/' \
        -e 's/^out vec4 frag_color;//' \
        -e 's/uniform sampler2D u_screen;/uniform sampler2D tex;/' \
        -e 's/uniform float u_time;/uniform float time;/' \
        -e 's/uniform vec2 u_resolution;//' \
        -e 's/texture(/texture2D(/g' \
        -e 's/frag_color/gl_FragColor/g' \
        -e 's/u_screen/tex/g' \
        -e 's/u_time/time/g' \
        -e 's/u_resolution/screenSize/g' \
        "$1" | sed '1i precision highp float;'
}

# ---------------------------------------------------------------------------
# X11 backend
# ---------------------------------------------------------------------------
BINARY_X11="$SCRIPT_DIR/screenshader"

stop_x11() {
    if [ -f "$PIDFILE" ]; then
        pid=$(cat "$PIDFILE")
        if kill -0 "$pid" 2>/dev/null; then
            echo "Stopping screenshader (PID $pid)..."
            kill "$pid"
            # Wait for graceful exit
            for _ in $(seq 1 30); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.1
            done
            # Force kill if still running
            if kill -0 "$pid" 2>/dev/null; then
                echo "Force killing..."
                kill -9 "$pid"
            fi
        fi
        rm -f "$PIDFILE"
    fi

    # Clean up params file
    rm -f "$PARAM_FILE"

    # Restore xfwm4 compositing
    if command -v xfconf-query &>/dev/null; then
        xfconf-query -c xfwm4 -p /general/use_compositing -s true 2>/dev/null
        echo "Restored xfwm4 compositing"
    fi
}

start_x11() {
    local shader="${1:-shaders/crt.frag}"

    if [ ! -x "$BINARY_X11" ]; then
        echo "Error: $BINARY_X11 not found. Run 'make' first."
        exit 1
    fi

    # Stop any running instance
    stop_x11

    # Disable xfwm4 compositing
    if command -v xfconf-query &>/dev/null; then
        xfconf-query -c xfwm4 -p /general/use_compositing -s false 2>/dev/null
        echo "Disabled xfwm4 compositing"
    fi

    # Wait for xfwm4 to release the composite extension
    sleep 0.5

    echo "Starting screenshader with: $shader"
    "$BINARY_X11" "$shader" &
    local pid=$!
    echo "$pid" > "$PIDFILE"

    # Clean up on exit
    trap stop_x11 EXIT INT TERM

    # Safety timeout: user must press 'c' within 10 seconds to confirm
    echo ""
    echo "============================================"
    echo "  Shader is active. Press 'c' within 10s"
    echo "  to confirm, or it will revert automatically."
    echo "============================================"
    echo ""

    local confirmed=false
    local timeout=10
    while [ "$timeout" -gt 0 ]; do
        printf "\r  Time remaining: %2ds  [press 'c' to confirm] " "$timeout"
        if read -r -t 1 -n 1 key 2>/dev/null; then
            if [ "$key" = "c" ] || [ "$key" = "C" ]; then
                confirmed=true
                break
            fi
        fi
        timeout=$((timeout - 1))

        # Check if compositor is still running
        if ! kill -0 "$pid" 2>/dev/null; then
            echo ""
            echo "Compositor exited unexpectedly."
            stop_x11
            exit 1
        fi
    done

    echo ""
    if [ "$confirmed" = true ]; then
        echo "Confirmed! Shader will stay active."
        echo "Run '$(basename "$0") --stop' or press Ctrl+C to stop."
        wait "$pid" 2>/dev/null
    else
        echo "No confirmation received. Reverting..."
        stop_x11
        exit 0
    fi
}

reload_x11() {
    if [ -f "$PIDFILE" ]; then
        pid=$(cat "$PIDFILE")
        if kill -0 "$pid" 2>/dev/null; then
            kill -USR1 "$pid"
            echo "Sent reload signal to PID $pid"
        else
            echo "screenshader is not running"
            rm -f "$PIDFILE"
        fi
    else
        echo "screenshader is not running"
    fi
}

# ---------------------------------------------------------------------------
# Hyprland backend
# ---------------------------------------------------------------------------
HYPR_SHADER="/tmp/screenshader_hypr.glsl"

stop_hyprland() {
    hyprctl keyword decoration:screen_shader "" >/dev/null 2>&1
    rm -f "$HYPR_SHADER"
    echo "Removed screen shader"
}

start_hyprland() {
    local shader="${1:-shaders/crt.frag}"

    if [ ! -f "$shader" ]; then
        echo "Error: shader not found: $shader"
        exit 1
    fi

    # Convert and write to temp file
    convert_hyprland "$shader" > "$HYPR_SHADER"

    # Apply via hyprctl
    hyprctl keyword decoration:screen_shader "$HYPR_SHADER" >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        echo "Applied shader: $shader (Hyprland)"
    else
        echo "Error: hyprctl failed. Is Hyprland running?"
        rm -f "$HYPR_SHADER"
        exit 1
    fi
}

reload_hyprland() {
    if [ -f "$HYPR_SHADER" ]; then
        hyprctl keyword decoration:screen_shader "$HYPR_SHADER" >/dev/null 2>&1
        echo "Reloaded shader (Hyprland re-reads file)"
    else
        echo "No shader is active"
    fi
}

# ---------------------------------------------------------------------------
# macOS backend
# ---------------------------------------------------------------------------
BINARY_MACOS="$SCRIPT_DIR/macos/screenshader-macos"

stop_macos() {
    if [ -f "$PIDFILE" ]; then
        pid=$(cat "$PIDFILE")
        if kill -0 "$pid" 2>/dev/null; then
            echo "Stopping screenshader (PID $pid)..."
            kill "$pid"
            for _ in $(seq 1 30); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.1
            done
            if kill -0 "$pid" 2>/dev/null; then
                kill -9 "$pid"
            fi
        fi
        rm -f "$PIDFILE"
    fi
    rm -f "$PARAM_FILE"
}

start_macos() {
    local shader="${1:-shaders/crt.frag}"

    if [ ! -x "$BINARY_MACOS" ]; then
        echo "Error: $BINARY_MACOS not found. Run 'make' first."
        exit 1
    fi

    # Stop any running instance
    stop_macos

    echo "Starting screenshader with: $shader"
    "$BINARY_MACOS" "$shader" &
    local pid=$!
    echo "$pid" > "$PIDFILE"

    trap stop_macos EXIT INT TERM

    # Give the overlay a moment to appear
    sleep 1

    if ! kill -0 "$pid" 2>/dev/null; then
        echo "screenshader-macos exited unexpectedly."
        echo "Check that Screen Recording permission is granted in System Settings."
        rm -f "$PIDFILE"
        exit 1
    fi

    echo "Shader is active."
    echo "Run '$(basename "$0") --stop' or press Ctrl+C to stop."
    wait "$pid" 2>/dev/null
}

reload_macos() {
    if [ -f "$PIDFILE" ]; then
        pid=$(cat "$PIDFILE")
        if kill -0 "$pid" 2>/dev/null; then
            kill -USR1 "$pid"
            echo "Sent reload signal to PID $pid"
        else
            echo "screenshader is not running"
            rm -f "$PIDFILE"
        fi
    else
        echo "screenshader is not running"
    fi
}

# ---------------------------------------------------------------------------
# Shared commands
# ---------------------------------------------------------------------------
list_shaders() {
    echo "Available shaders:"
    for f in "$SCRIPT_DIR"/shaders/*.frag; do
        [ -f "$f" ] || continue
        name=$(basename "$f" .frag)
        [ "$name" = "composite" ] && continue
        echo "  $name  ($f)"
    done
}

set_param() {
    local name="$1"
    local value="$2"

    if [ -z "$name" ] || [ -z "$value" ]; then
        echo "Usage: $(basename "$0") --set NAME VALUE"
        echo "  e.g.: $(basename "$0") --set u_curvature 0.1"
        exit 1
    fi

    if [ "$PLATFORM" = "hyprland" ]; then
        echo "Note: runtime parameters are not supported on Hyprland."
        echo "Edit the shader file and use --reload instead."
        return
    fi

    # Update or add the param in the file
    if [ -f "$PARAM_FILE" ] && grep -q "^$name " "$PARAM_FILE" 2>/dev/null; then
        sed -i "s/^$name .*/$name $value/" "$PARAM_FILE"
    else
        echo "$name $value" >> "$PARAM_FILE"
    fi

    echo "Set $name = $value"
}

get_params() {
    if [ "$PLATFORM" = "hyprland" ]; then
        echo "Note: runtime parameters are not supported on Hyprland."
        return
    fi

    if [ -f "$PARAM_FILE" ]; then
        echo "Current parameters:"
        while IFS=' ' read -r name value; do
            echo "  $name = $value"
        done < "$PARAM_FILE"
    else
        echo "No parameters set"
    fi
}

# ---------------------------------------------------------------------------
# Dispatch: route commands to the right backend
# ---------------------------------------------------------------------------
do_stop() {
    case "$PLATFORM" in
        x11)       stop_x11 ;;
        hyprland)  stop_hyprland ;;
        macos)     stop_macos ;;
        *)         echo "Unsupported platform"; exit 1 ;;
    esac
}

do_start() {
    case "$PLATFORM" in
        x11)       start_x11 "$1" ;;
        hyprland)  start_hyprland "$1" ;;
        macos)     start_macos "$1" ;;
        *)
            echo "Error: unsupported platform."
            echo "Supported: X11, Hyprland, macOS"
            exit 1
            ;;
    esac
}

do_reload() {
    case "$PLATFORM" in
        x11)       reload_x11 ;;
        hyprland)  reload_hyprland ;;
        macos)     reload_macos ;;
        *)         echo "Unsupported platform"; exit 1 ;;
    esac
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
case "${1:-}" in
    --stop|-s)
        do_stop
        rm -f "$PARAM_FILE"
        ;;
    --reload|-r)
        do_reload
        ;;
    --list|-l)
        list_shaders
        ;;
    --set)
        set_param "$2" "$3"
        ;;
    --get)
        get_params
        ;;
    --help|-h)
        echo "Usage: $(basename "$0") [OPTIONS] [shader.frag]"
        echo ""
        echo "Multi-platform screen shader tool (detected: $PLATFORM)"
        echo ""
        echo "Options:"
        echo "  (no args)       Start with CRT shader"
        echo "  NAME            Start with a built-in shader by name (e.g. crt, amber)"
        echo "  shader.frag     Start with a shader file path"
        echo "  --stop, -s      Stop the running compositor"
        echo "  --reload, -r    Hot-reload the current shader"
        echo "  --list, -l      List available shaders"
        echo "  --set NAME VAL  Set a shader parameter at runtime"
        echo "  --get           Show current parameters"
        echo "  --help, -h      Show this help"
        echo ""
        echo "Supported platforms:"
        echo "  X11        Linux (XComposite compositor)"
        echo "  Hyprland   Linux Wayland (decoration:screen_shader)"
        echo "  macOS      Metal overlay (ScreenCaptureKit)"
        echo ""
        echo "Shader presets: crt, amber, green, nightlight, vhs"
        echo ""
        echo "CRT parameters:"
        echo "  u_curvature     Barrel distortion (0=flat, 0.08=default, 0.3=heavy)"
        echo ""
        echo "Examples:"
        echo "  $(basename "$0")                           # CRT effect"
        echo "  $(basename "$0") amber                     # Amber terminal (by name)"
        echo "  $(basename "$0") shaders/amber.frag        # Amber terminal (by path)"
        echo "  $(basename "$0") --set u_curvature 0.15    # Crank up curvature"
        echo "  $(basename "$0") --set u_curvature 0       # Flat (no curve)"
        echo "  $(basename "$0") --stop                    # Restore normal desktop"
        ;;
    *)
        shader="$1"
        if [ -n "$shader" ] && [ ! -f "$shader" ]; then
            # Try resolving as a bare shader name (e.g. "crt" → "shaders/crt.frag")
            if [ -f "$SCRIPT_DIR/shaders/${shader}.frag" ]; then
                shader="$SCRIPT_DIR/shaders/${shader}.frag"
            else
                echo "Error: shader not found: $shader"
                echo "Run '$(basename "$0") --list' to see available shaders."
                exit 1
            fi
        fi
        do_start "$shader"
        ;;
esac
