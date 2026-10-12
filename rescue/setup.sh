#!/bin/bash
# Runs inside the new system while it's being built (make-image.sh starts it).
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
version=$1 build_id=$2 built=$3
export DEBIAN_FRONTEND=noninteractive

cat > /etc/diskforge-live.conf <<CONF
VERSION=$version
BUILD_ID=$build_id
BUILT=$built
CONF

# English and UTC. The keyboard layout can be changed on the desktop
# (Preferences > LXQt Settings > Keyboard and Mouse).
sed -i 's/^# *\(en_US.UTF-8 UTF-8\)/\1/' /etc/locale.gen
locale-gen >/dev/null
echo 'LANG=en_US.UTF-8' > /etc/default/locale
echo diskforge-live > /etc/hostname

# Openbox draws the window frames: Debian's own settings, with our theme and a bigger title font.
mkdir -p /etc/skel/.config/openbox
sed -e 's|<name>Clearlooks</name>|<name>DiskForge-Live</name>|' \
    -e 's|<name>sans</name>|<name>DejaVu Sans</name>|g' -e 's|<size>8</size>|<size>10</size>|g' \
    /etc/xdg/openbox/rc.xml > /etc/skel/.config/openbox/rc.xml
grep -q '<name>DiskForge-Live</name>' /etc/skel/.config/openbox/rc.xml || { echo "Openbox's rc.xml has changed, the theme isn't set"; exit 1; }

# The one user, without a password: whoever is at the PC owns it. sudo and polkit
# (files/etc) don't ask either.
useradd --create-home --shell /bin/bash --comment "DiskForge Live" rescue
passwd --delete rescue >/dev/null
for group in sudo netdev plugdev; do
    if getent group "$group" >/dev/null; then usermod -aG "$group" rescue; fi
done
chmod 0440 /etc/sudoers.d/diskforge-live

# No desktop icons: the home screen (diskforge --home, started by the session) is the desktop.
chown -R rescue:rescue /home/rescue

# Everything DiskForge can use is here, so nothing it offers is missing in the rescue system.
missing=
while read -r program _; do
    case $program in ''|'#'*) continue ;; esac
    command -v "$program" >/dev/null || missing="$missing $program"
done < /tmp/programs.txt
if [ -n "$missing" ]; then
    echo "These programs DiskForge uses aren't in the image:$missing" >&2
    exit 1
fi

# Started on every boot
systemctl enable diskforge-live-logs.service diskforge-live-logs.timer diskforge-live-writeblock.service >/dev/null
# Nothing that touches the PC's own drives on its own, or keeps the stick busy
systemctl mask fstrim.timer e2scrub_all.timer e2scrub_reap.service smartmontools.service \
    apt-daily.timer apt-daily-upgrade.timer man-db.timer dpkg-db-backup.timer >/dev/null 2>&1
# ClamAV's own updater would download 250 MB into memory at every start; Scan for Viruses
# updates the list on the stick itself, when asked (diskforge-live-virus-list).
systemctl mask clamav-freshclam.service >/dev/null 2>&1

# Our boot animation. The initrd below gets it, so it shows from the start.
plymouth-set-default-theme diskforge-live

# The build ID goes into the initrd too (hooks/diskforge-live), so this initrd only
# starts from its own stick.
update-initramfs -u -k all

# Smaller and cleaner
apt-get clean
rm -rf /var/lib/apt/lists/* /var/cache/debconf/*-old /tmp/* /var/tmp/*
find /var/log -type f -delete
: > /etc/machine-id
rm -f /var/lib/dbus/machine-id
