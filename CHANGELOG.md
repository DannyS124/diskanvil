# Changelog

## 0.5.2
DiskForge is now called DiskAnvil (too many other things are called DiskForge). Your settings, add-ons, virus list
and the rest move over by themselves the first time it starts, the new package replaces the old one, and DiskForge
Live sticks keep working: Make a DiskAnvil Live USB can update them.

New:
- DiskAnvil for Windows: the same program on 64-bit Windows 10 and 11, with an installer (Program Files, the Start menu, a desktop shortcut if you want one, and Settings → Apps to remove it) or a zip to run it from a folder. Partitions, formatting, Disk Health, the USB makers, Check a USB Stick, Clone, Back Up and Restore, Disk Usage, Disk Cleanup, Optimize Drives, Recover Partitions, Find Lost Files and Scan for Viruses all work there, and it formats FAT32 over 32 GB, which Windows itself won't. The README's Windows part says what's different
- Scan for Viruses (Tools menu, and a tile in DiskAnvil Live): looks through a drive or a folder with ClamAV, or on Windows with Microsoft Defender when ClamAV isn't installed. On Linux the drive is mounted read-only while it looks. Quick checks programs, scripts and documents; Everything checks every file
- Scan for Viruses also finds files dressed up as something else: `invoice.pdf.exe`, names with a hidden character that flips them around, and programs named like a picture or a document
- On a drive with Windows on it, Scan for Viruses goes through everything that starts with Windows, read straight from its registry files, and shows the odd ones. It can switch them off, after backing up what it changes into a "DiskAnvil backups" folder on that drive
- It checks the boot files too: who signed each one against the PC's own Secure Boot lists, the boot entries, the MBR's boot code and the Secure Boot settings, and can put Windows' own boot files back. On Linux it adds fwupd's firmware checks
- Odd startup programs and boot files can be looked up online by their SHA-256 fingerprint (CIRCL's hashlookup), only when you say so, every time. Nothing else is sent
- Found files are deleted only after you've seen the list, and every deletion is written down in a log. In DiskAnvil Live the virus list is kept on the stick (when it was made with Make a DiskAnvil Live USB), so it only needs a small update next time
- Give New IDs (Action menu): gives a copy of a drive its own disk, partition and file system IDs, so it can be used next to the drive it was copied from, and fixes a Linux on it to match (fstab, crypttab, GRUB, systemd-boot, keeping each old file). When two drives share IDs, a bar at the top says so and offers it
- Find Lost Files brings deleted files back with their names and folders from FAT, exFAT and NTFS drives, and the files in Windows' Recycle Bin under their old names. On NTFS it reads the change journal for names of files whose records were reused, and lists what was deleted recently, newest first. Files found by what's inside them still come after, as before
- Find Lost Files looks in Windows' earlier versions of a drive (shadow copies) and brings back what has changed or gone since, in folders named after the date of each copy. DiskAnvil reads them with its own code, so this works on Linux and DiskAnvil Live too
- Find Lost Files opens virtual machines' disks: .vdi, .vmdk, .vhd, .vhdx and .qcow2
- Find Lost Files previews PDFs (their first page), Word, Excel, PowerPoint and OpenDocument files (their text), what's in archives, and songs and films (their details and cover, and a frame from a film when ffmpeg is installed). A separate program makes them, in a sandbox on Linux, so a damaged file can't take DiskAnvil down
- Drive settings in Properties for every SATA drive on Linux, SSDs too: power saving, the write cache and read-ahead, plus when to spin down and seek noise for hard drives
- Disk Health: the conveyance self-test for hard drives, Turn SMART On, and the last tests DiskAnvil saw finish
- Secure Erase on NVMe drives, on Linux: Sanitize (block erase or crypto erase, whichever the drive has) and the sector size it should have afterwards
- New Partition at the end of an SSD can leave part of it unused, so it stays fast and lasts longer
- DiskAnvil Live shows a hidden area (HPA) at the end of a SATA drive in Properties, and can give that space back
- BitLocker drives show as BitLocker and unlock with their password or their 48-digit recovery key. On Windows, Change BitLocker Password too
- Help → Support DiskAnvil (Ko-fi), and a line about it in About
- `--open virus-scan`

Changed:
- Disk Usage says why a drive counts more space as used than its files add up to

Fixed:
- Typing a drive's name to confirm in Clone, Restore or Rescue Copy could move the keyboard to another button, so a space pressed Cancel
- Check a USB Stick's map stayed yellow after a good check. Now it turns green
- Tall windows could run off a small screen
- Write Image to USB, Make a Windows USB and Make a DiskAnvil Live USB now refuse to write onto the stick the image is on
- The unlock window's explanation stretched the window instead of wrapping
- Change Passphrase is greyed out for BitLocker drives on Linux, where only Windows can change their password

## 0.5.1
New:
- DiskAnvil Live: a rescue stick that starts any PC, new (UEFI, Secure Boot on is fine) or old (BIOS), into a small desktop with DiskAnvil, TestDisk, PhotoRec, a browser and a terminal, plus memtest86+ in the boot menu. It keeps a log of every start on the stick. Built from `rescue/`, see [docs/DISKANVIL-LIVE.md](docs/DISKANVIL-LIVE.md)
- DiskAnvil Live has its own look (logo, boot menu, boot animation, icons, desktop theme) and opens on a home screen with big tiles for the usual jobs
- Make a DiskAnvil Live USB (File menu, or right-click a stick): puts DiskAnvil Live on a stick as normal files on FAT32 and checks every one, so the stick still works for files. Open Logs shows what a used stick saved
- Find Lost Files (Action menu, or File menu for disk images): gets deleted files back, or files off a drive that was formatted or won't open, by what's inside them (pictures, documents, PDFs, videos, music, archives). Thumbnails and a preview, each file says if it looks whole, and you save the ones you tick to another drive. It only reads the drive it looks through
- Check a USB Stick (Action menu, or right-click a stick): finds bad spots and fake sticks that say they're bigger than they are. It says how much a fake one really holds and can make it safe to use. Quick takes minutes, Full checks every byte
- Make a Windows USB (File menu): a Windows 10 or 11 install stick from Microsoft's ISO that starts with Secure Boot on. install.wim gets split with wimlib when it's too big. Windows 11 options: skip the TPM/Secure Boot/RAM check, no Microsoft account, local account, skip the privacy questions, no automatic BitLocker
- Write Image to USB takes compressed images (.xz, .gz, .bz2, .lzma, .zst, or a .zip) and any checksum (MD5, SHA-1, SHA-256, SHA-512, or a SHA256SUMS file)
- Write Image to USB can copy a Linux ISO's files instead, so the stick stays usable for files (UEFI only). Ubuntu and Debian live sticks can keep changes between starts
- Tools > Quick Fixes: the DiskAnvil Live home screen on a normal PC
- A DiskAnvil Live theme (View > Theme)
- `--open <tool>` opens just one tool, for launchers (write-image, windows-usb, rescue-usb, check-stick, lost-files)
- `--log <file>` writes down what DiskAnvil does, for bug reports
- A .deb for Debian and Ubuntu (`packaging/deb/build.sh`)

Changed:
- Secure Erase says in plain words why a drive refused it and what to do instead (most USB adapters block it, Wipe Disk works through any of them), and warns about USB before it starts
- Secure Erase on a frozen drive: Unplug and Replug walks you through pulling the drive's power and plugging it back in, finds the drive again (even under a new name) and checks it's not frozen anymore. On DiskAnvil Live it also asks old controllers to look for the drive, which they don't do by themselves
- New Partition Table only asks for the admin password when something on the drive is in /etc/fstab or encrypted
- On a live system, the system's own image isn't listed as a drive anymore
- On a screen smaller than the window (small laptops, VMs) it opens maximized
- Staging a release also builds DiskAnvil Live, and publishing attaches the ISO

Fixed:
- After a Secure Erase the drive shows up blank right away. Before, it kept showing its old partitions until DiskAnvil was reopened
- A window that needed a drive unmounted first (Back Up, Clone, Rescue Copy...) waited forever when the unmount failed. Now it says the drive couldn't be opened
- Pressing Enter in Write Image to USB, Back Up or Restore no longer opens the file picker

## 0.5.0
Everything new since 0.4.3.

Safety:
- A drive's name can't steer an add-on anymore: a name starting with "-" can't turn into an option, "/" and ".." can't point at another folder, and names with hidden characters are refused
- Add-ons that use admin power (pkexec, sudo) or run a shell ask every time, with a warning. The question shows the command one part per line
- Look-only add-ons (new `look_only` field) run in a read-only sandbox: they can't change files, use the network or reach other programs. Only these are offered on the system disk
- "Don't ask again" is tied to the add-on file, so any change to the file asks again
- Add-ons that turn up in the add-on folder without going through DiskAnvil are flagged until you say they're yours
- The online add-on list is signed with a key that isn't on GitHub, and DiskAnvil checks the signature
- Drive names, file names and backup descriptions are shown as plain text, and hidden characters (like ones that flip text direction) are dropped
- Check for Updates builds the release link itself instead of opening whatever link comes back
- Rescue maps whose blocks don't add up are refused, and Clone refuses partition tables that run past the end of the drive
- Every release now runs crash tests: thousands of damaged add-ons, lists, signatures, backups, maps and partition tables, also in a build with memory checks
- Programs DiskAnvil starts (add-ons, terminals) don't get handed an open drive anymore

New:
- Stop button: wipes, checks and self-tests get a bar at the top showing how far along they are, with a Stop button (also on the toolbar). A drive's own Secure Erase says up front that it can't be stopped
- A warning bar at the top when a drive is failing or needs a look, with Back Up (or Rescue Copy), Details and Dismiss. It comes back if things get worse
- Disk Health explains itself: every reason behind the verdict, what each number means, and the ones that count are in bold. It goes by the numbers Backblaze's drive stats tie to failures. Connection errors say to check the cable, since the drive itself may be fine
- Firmware: Disk Health shows the drive's firmware and asks fwupd if there's a newer one. DiskAnvil never installs firmware itself
- Btrfs: error counts for each Btrfs partition in Disk Health, and a scrub now or every month
- Recover Partitions: put a lost or damaged partition table back, from the copy GPT keeps at the end of the drive, from a layout DiskAnvil saw on that drive before (it keeps the last 10), or by scanning the drive for file systems
- Inspect Partition Table: the MBR, both GPT copies and the partition list as they are on the drive, checksums checked, plus any sector in hex. Only reads, so it works on the system disk too
- Partition type and flags (GPT attributes, the MBR boot flag)
- A lock on encrypted partitions in the map: click it to unlock or lock
- Power settings for hard drives: when to spin down, how hard to save power, the write cache, and Sleep Now
- Rescue Copy can go easy on a failing drive: pause when it's too hot, rest after a run of read errors, or cap the speed
- RAID arrays are listed like drives, with their status (checking, rebuilding, degraded), and you can start a check
- LVM volume groups are listed like drives, their logical volumes as the volumes (needs udisks2-lvm2)
- Add-ons can ask for things in a form, have their own settings, and show their output in a window. Every add-on action is in the Tools menu, with the reason when one doesn't fit, and you can pin actions to the toolbar or give them a shortcut
- Add-on Maker: make or change an add-on in a window instead of writing JSON
- Themes: Classic, Deadshadow and High Contrast built in, theme add-ons, and a Theme Maker. Danger stays red and text stays readable whatever the theme
- Clone Drive: copies a whole drive onto another one, skips empty space, and can grow the last partition into a bigger drive. Pick whether the copy replaces the old drive (same IDs) or sits next to it (new IDs)
- Back Up and Restore: saves a drive or partition to a compressed .img.zst file with a checksum, and checks the whole backup before restoring anything
- Rescue Copy: gets what's still readable off a failing drive, easy parts first. Stop it any time and carry on later; the map file works with GNU ddrescue too
- Scan for Bad Sectors now draws a map of the drive as it goes: green is fine, orange is slow, red can't be read
- Disk Usage: a map of what's using the space on a drive, click into folders
- Disk Cleanup: old package files, packages you've uninstalled, old system logs, your cache folder and the trash
- Optimize Drives: TRIM now, or every week
- Btrfs Snapshots: see your subvolumes and snapper's snapshots
- Secure Erase: the drive's own erase command (ATA and NVMe), with help for drives that are "frozen"
- Get Add-ons: install add-ons from the online list (checked against a checksum before installing)
- Flatpak and AppImage downloads on each release
- Ready for translations (see CONTRIBUTING.md)
- Disk health (SMART): healthy/warning/failing on every disk, temperature, hours, bad sectors, self-tests
- Write an ISO to a USB stick, with an optional SHA-256 check and verification afterwards
- Encrypted drives: encrypt when formatting or creating a partition, unlock, lock, change passphrase
- Wipe a whole disk with zeros
- Check for errors and repair
- Mount at startup (adds an /etc/fstab entry under /mnt)
- Safely remove USB drives
- Open .iso and .img files as disks
- Benchmark read speed, access time and write speed
- Progress bar for long operations
- Fix: actions that ask something first (repair after a check, format, delete, resize...) could fail with "disk not found" if the disk list refreshed while the question was open
- Right-click any partition, free space or drive to get everything you can do with it, partition and drive options together
- Scan for bad sectors and repair them (rewrites just those sectors so the drive swaps in spares)
- Long self-test button in Disk Health
- Add-ons: JSON files that add menu actions (Tools → Add-ons), with three examples in examples/addons

## 0.4.3
- Check for Updates says so when there's no release, instead of "Not Found"
- Includes the 0.4.2 wording changes (0.4.2 was taken down before it was tested)

## 0.4.2
- Simpler wording in the help, About window and docs

## 0.4.1
- Switched the license to GPL-3.0-or-later

## 0.4.0
- Renamed to DiskForge (it was Disk Manager)
- Help (F1), About window, Check for Updates
- App icon, man page, AppStream metadata

## 0.3.0
- Resize partitions (ext4, btrfs, NTFS, FAT32)

## 0.2.0
- Format, create and delete partitions, rename, new partition tables

## 0.1.0
- First version: view disks, mount/unmount
