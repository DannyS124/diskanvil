# Changelog

## 0.5.2
DiskForge is now called DiskAnvil (too many other things are called DiskForge). Your settings, add-ons, virus list
and the rest move over by themselves the first time it starts, the new package replaces the old one, and DiskForge
Live sticks keep working: Make a DiskAnvil Live USB can update them.

New:
- Who's Using It? (offered when a drive won't unmount because it's busy, and on a mounted partition's right-click menu): the programs with something open on it and what, an attached disk image on it or a drive mounted inside it, with Close It, Stop It and Try Again. Linux and DiskAnvil Live
- Why Did It Crash? (Tools menu, and a tile in DiskAnvil Live): a Windows that keeps blue-screening, freezing or restarting, explained from its own records without starting it. Its event log (read by our own reader), its crash dumps and its error reports give each stop: the stop code in plain words, the driver whose code was running and what it is, a held power button, a power cut or hard freeze, dying on the way to sleep, and the disk or hardware errors logged just before. Then what they have in common, and what to do
- Which Files Were Hit? (in Rescue Copy, once it couldn't read everything): reads the copy's FAT, exFAT or NTFS file systems and lists each file with unreadable parts and how much of it is missing, so a map of bad spots becomes "these files are damaged, the rest are fine". The list can be saved next to the copy
- Write-Protected Drive (Action menu, right-click a stick, and a tile in DiskAnvil Live): says why a stick, card or drive won't let anything be saved to it, and fixes what a program can: Linux's read-only flag, DiskAnvil Live's write protection, a file system opened read-only after errors (repaired and opened again), and on Windows the setting that write-protects every USB drive and diskpart's read-only marks. A card's lock switch and a stick that locked itself because it's wearing out are said plainly, with no pretend fix
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
- What This PC Did (Tools menu): what system is on a drive and what it was used for, from its own records, without starting it. Windows: version, name and owner, install and last shutdown, USB drives plugged in with their serials and times, programs run, files and folders opened, searches and what was typed, websites and downloads (Chrome, Edge, Brave, Vivaldi, Opera, Firefox), networks joined, accounts. Linux: distribution and kernels, accounts, sign-ins, start-ups and shut-downs, programs installed and removed, recent files, websites. Mac: version and accounts. Saved as a web page, CSV or PDF
- Where photos and films were taken, by name and without the internet (GeoNames' places), with a link to OpenStreetMap that asks first; Find Lost Files' Where They Were Taken puts them on a world map. Phone films (iPhone and Android) say where too, with the phone's model and the time in its own time zone
- A photo's details add the camera's serial number, which way it faced, the satellites' time (and from it the camera clock's time zone), and signs it was edited after it was taken
- Documents say when they were last printed, how many times they were saved, the time spent editing, the template, manager and category; a PDF saved again lists its earlier versions as files of their own
- Fingerprints (MD5, SHA-1, SHA-256) for found files, a SHA256SUMS file with what's saved, and a report of what was found as a web page, CSV or PDF
- The viewer looks closer: lighter, darker, the dark parts lifted, grain smoothed, sharpened, and blur taken back out (out of focus, or a movement drawn along its streak), saved as a new picture that says what was done. Look Closer (Tools menu, and a Live home tile) opens any picture or film from anywhere in the viewer with all its tools, not only found files. Pick a Spot draws a box around part of a picture, or of a paused film's frame (a face or a number plate on a security camera), shows it on its own to work on, and saves it bigger. More forensic tools, the kinds police video labs use: a JPEG decoded again within what it stores, Wiener de-blurring and a lens's disc, non-local means, uneven-light (homomorphic) and Retinex, single channels and the most-contrast one, a blown-out overlay, video levels to full, one field, a picture's proper shape, a wide lens's bend (fitted to three clicks), stacking by middle value or without outliers, the sharpest frames only, and a number plate followed by its corners frame to frame; pictures shown bigger are smoothed (Pixels shows the squares). Forensic video tools: local contrast (CLAHE), grain smoothed with edges kept, deinterlacing, Straighten (four corners made square-on), Stack Frames (the frames around a spot lined up to a fraction of a pixel and put together, also at twice the size) and a steadied copy of a shaky film; enlarging is Lanczos now. Enhance profiles: ready-made ones (too dark, out of focus, night camera, scanned page...), your own saved under a name, and shared as small files that are dropped on the viewer. Films play from a tenth of normal speed, step a frame at a time, zoom in while playing, are enhanced live, and save a frame or a slowed-down copy
- Files Windows keeps compressed come back whole: Find Lost Files unpacks NTFS compression and compact.exe's (CompactOS, CompactGUI) itself, deleted files too. Where Linux can't read them (DiskAnvil Live, or with ntfs-3g), Windows Drivers and Services, Scan for Viruses, Save My Files and putting Windows' boot files back unpack them straight from the drive, read-only
- Find Lost Files previews PDFs (their first page), Word, Excel, PowerPoint and OpenDocument files (their text), what's in archives, and songs and films (their details and cover, and a frame from a film when ffmpeg is installed). A separate program makes them, in a sandbox on Linux, so a damaged file can't take DiskAnvil down
- Drive settings in Properties for every SATA drive on Linux, SSDs too: power saving, the write cache and read-ahead, plus when to spin down and seek noise for hard drives
- Disk Health: the conveyance self-test for hard drives, Turn SMART On, and the last tests DiskAnvil saw finish
- Secure Erase on NVMe drives, on Linux: Sanitize (block erase or crypto erase, whichever the drive has) and the sector size it should have afterwards
- New Partition at the end of an SSD can leave part of it unused, so it stays fast and lasts longer
- DiskAnvil Live shows a hidden area (HPA) at the end of a SATA drive in Properties, and can give that space back
- BitLocker drives show as BitLocker and unlock with their password or their 48-digit recovery key. On Windows, Change BitLocker Password too
- Help → Support DiskAnvil (Ko-fi), and a line about it in About
- `--open virus-scan`, `remote-help`, `net-boot`, `live-on-pc`

Getting files back:
- What Happened? (Tools): start from what went wrong. It gives honest odds (an SSD with TRIM on erases deleted files within minutes), what not to do, and which tool to use. On a failing drive it makes a Rescue Copy before anything checks it
- Find Lost Files shows when a file was made, changed, last opened and deleted (FAT, exFAT, NTFS), a photo's details (when it was taken, the camera and lens, where), and a PDF's or Office file's own properties
- A viewer for found files before they're saved: pictures to zoom into and page through, PDFs page by page, films and songs that play, a document's words and the bytes of anything else. It's a program of its own, sandboxed on Linux, so a damaged file can't take DiskAnvil down
- Found files open in their own program or drag out to a folder. The copies for that never go on the drive being searched, and they're gone when Find Lost Files closes
- More kinds of files found and shown: camera raw photos, TIFF, PSD, RAR, MKV, FLAC, WMV, gzip, Office 97 files, emails and mailboxes, RTF, web pages, SVG, calendars and contacts
- Found files get names from what's inside them (a photo's date and camera, a document's title, a song's tags), can be sorted by kind and date, show small pictures, have their copies marked and are checked to see if they open
- Inside: each found file's parts, with the damage marked, and a hex editor that works on a copy
- Repair a Copy fixes PNG, JPEG, Office, PDF, gzip, WAV and SQLite files, and films that lost their index (with a working film from the same camera)
- Find text by the words you remember, in deleted files, Windows' page file and Word's AutoRecover files
- The small pictures Windows keeps of photos (thumbcache, Thumbs.db), even of ones that are gone
- Ransomware: its notes and the endings it adds are spotted, its family named, and it says what can be done (a free decryptor, Windows' earlier versions, the deleted originals)
- On a failing drive, Find Lost Files keeps a Rescue Copy of everything it reads, so nothing has to be read twice
- Btrfs files deleted since an older copy of its file tree (needs btrfs-progs)
- A timeline of what the drive kept, by date, saved as CSV or PDF

Drives:
- Disk Health: eMMC wear, whether partitions are aligned, SMR drives, known firmware bugs (Samsung 980 and 990 PRO), and What's That Noise?
- Disk Health compares a Seagate drive's own FARM hours with SMART's, which shows a history that was wiped
- Disk Health shows Backblaze's lifetime failure rate for the drive models in their records (Backblaze Drive Stats)
- Format as an SD Card: the SD Association's layout for the card's size, with our own exFAT and FAT16
- Bad Sectors: Refresh Slow Areas on an SSD (experimental): slow spots are read and written back unchanged
- Sector Size, in DiskAnvil Live: switches an ATA drive between 512 and 4096-byte sectors, when the drive can
- Properties shows a USB drive's speed, port and controller, and says so when a USB 3 drive runs at USB 2 speed
- LUKS: back up the encryption header and put it back (the same bytes cryptsetup writes)
- VeraCrypt volumes unlock, with a PIM, keyfiles, hidden and system volumes
- RAID arrays, NAS drives and Windows dynamic disks open read-only, from a bar that says they're there or from Tools

Fixing a PC:
- Windows Product Key: the one in the PC's firmware and each Windows install's, with generic keys explained
- Windows Drivers and Services: newest first with their makers. A suspect one can be switched off, with a backup
- Save My Files: people's folders and browser bookmarks off a drive, going on past what can't be read
- Boot Menu Entries (Tools): change the order, pick what starts next time, switch entries on and off, add and remove them, and a report of how the PC starts
- Repair Linux Boot, in DiskAnvil Live: GRUB, Fedora's shim and systemd-boot put back by the install's own tools
- Try the Repair First: a stand-in takes a file system repair, so you can look at the result before the drive changes
- Make Room for Windows Recovery (Windows): Microsoft's steps for update error 0x80070643 (KB5028997), done for you

Backups and records:
- Backups use zstd's seekable format, so Find Lost Files can read one without restoring it
- Restore from Clonezilla: partclone images, split and packed ones too, checked by their checksums
- Try Starting It: a drive or a disk image started in a virtual machine (QEMU), read-only, its changes thrown away
- A certificate of erasure (PDF) after a wipe, with Check It to read the drive back; Disk Health's report and benchmark results as PDFs too
- Evidence images: E01 files written and read by our own code (libewf reads them the same), with a case report

DiskAnvil Live:
- Make a DiskAnvil Live USB can keep your changes between starts (settings, downloads, programs) and add a drive for files (exFAT) on the rest of the stick. A stick made that way can be updated without wiping it
- ISO files copied into that drive's ISOs folder start from the boot menu, under Other ISOs on this stick (needs Secure Boot off)
- Put DiskAnvil Live on This PC (File menu): a partition in free space on the PC's own drive, last in its boot menu, so no stick is needed. Update It and Take It Off too. UEFI PCs only
- Start Other PCs from This One: PCs on the same network start DiskAnvil Live from it, with nothing installed on them, Secure Boot on or off
- Let Someone Help: someone you trust on the same network sees the screen (VNC) or gets a terminal (SSH), with passwords made up each time
- Write-Protect Every Drive: every drive but the stick is read-only, drives plugged in later too, from the boot menu or Tools; one can be let through to save to

Changed:
- Disk Usage says why a drive counts more space as used than its files add up to

Fixed:
- Find Lost Files was cut off on small screens. It fits any screen now, and remembers its sizes
- Saving found files closed Find Lost Files, and so did Enter. It stays open now, and marks what's been saved
- Thumbnails scrolled away and back could stay a blank card, and big PDFs only showed as ".pdf"
- Backups with something after their end are called that again, not damaged
- Windows dynamic disks on MBR are found the way Linux shows them, and their volumes open read-only
- On Windows, Make Room for Windows Recovery found no drive, and Windows Product Key and the Windows picker listed the
  running Windows twice
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
