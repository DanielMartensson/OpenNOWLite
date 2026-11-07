#!/bin/sh
# opennowlite - launch OpenNOWLite on this seat.
#
# Keep whatever runtime dir and display the session provides (a shell started
# from weston already has the right ones) and only fall back the way the
# opennow launcher does: this image's weston listens on wayland-1 under
# /home/weston, while /run/user/1000 has no wayland socket of its own.
if [ -z "${XDG_RUNTIME_DIR}" ]; then
    if [ -S /home/weston/wayland-1 ]; then
        export XDG_RUNTIME_DIR=/home/weston
    elif [ -d /run/user/1000 ]; then
        export XDG_RUNTIME_DIR=/run/user/1000
    fi
fi
if [ -z "${WAYLAND_DISPLAY}" ]; then
    for _socket in wayland-1 wayland-0; do
        if [ -S "${XDG_RUNTIME_DIR}/${_socket}" ]; then
            export WAYLAND_DISPLAY="${_socket}"
            break
        fi
    done
fi
exec /usr/bin/opennow-lite "$@"
