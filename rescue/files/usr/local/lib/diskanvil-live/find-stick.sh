# Sourced by the helpers that write to the stick (diskanvil-live-logs, diskanvil-live-virus-list).
#
# find_stick prints where the stick this system belongs to is mounted: the one it started from,
# or, when it started from the ISO (a CD, a VM) or was copied to memory, a stick made from that
# same ISO that's plugged in. Those are found by their build file and mounted on $state/stick,
# read-only, for as long as the helper needs them (it unmounts them after). Fails when there's
# none. Needs $state and $BUILD_ID (from /etc/diskanvil-live.conf).
find_stick() {
    if mountpoint -q /run/live/medium && [ -e "/run/live/medium/.disk/diskanvil-live-$BUILD_ID" ] &&
        [ "$(findmnt -no FSTYPE /run/live/medium)" = vfat ]; then
        echo /run/live/medium
        return 0
    fi
    local mnt=$state/stick
    if mountpoint -q "$mnt" && [ -e "$mnt/.disk/diskanvil-live-$BUILD_ID" ]; then
        echo "$mnt"
        return 0
    fi
    mkdir -p "$mnt"
    local dev
    for dev in $(blkid -t TYPE=vfat -o device 2>/dev/null); do
        mount -o ro "$dev" "$mnt" 2>/dev/null || continue
        if [ -e "$mnt/.disk/diskanvil-live-$BUILD_ID" ]; then
            echo "$mnt"
            return 0
        fi
        umount "$mnt"
    done
    return 1
}
