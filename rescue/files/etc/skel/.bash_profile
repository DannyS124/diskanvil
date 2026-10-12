# DiskAnvil Live: the first screen opens the desktop, unless the boot menu asked for text mode.
[ -f ~/.bashrc ] && . ~/.bashrc
if [ "$(tty)" = /dev/tty1 ] && [ -z "${DISPLAY:-}" ]; then
    if grep -qw diskanvil-live.text /proc/cmdline; then
        diskanvil-live-welcome
    else
        diskanvil-live-session
    fi
fi
