DiskAnvil Live
==============

A USB stick that starts any PC into its own small Linux desktop with DiskAnvil and some
other repair tools. Nothing on the PC changes unless you tell it to, so it's safe to use
on a PC that won't start, a drive that's failing, or a Windows PC you don't want to
install anything on.

Starting a PC from the stick
----------------------------
1. Plug the stick in and turn the PC on.
2. Press the boot menu key right away. It's usually F12, F11, F10, F9, F8 or Esc,
   depending on the maker (Dell F12, HP F9, Lenovo F12, ASUS F8 or Esc, Acer F12,
   MSI F11). Pick the USB stick, the "UEFI" entry if there are two.
3. The DiskAnvil Live menu shows up. Enter starts it. The desktop opens with DiskAnvil
   after a minute or so.

Secure Boot can stay on. The stick starts the same way Debian does, through boot files
signed for Secure Boot.

What's in the menu
------------------
Start DiskAnvil Live                  the normal way
Safe graphics                         if the screen stays black or looks broken
Text mode                             no desktop, just a command line
Copy to memory                        takes longer to start, then the stick can come out
Tools and troubleshooting:
  Memory test                         memtest86+. It isn't signed for Secure Boot, so turn
                                      Secure Boot off in the PC's setup to use it
  Detailed messages                   more on screen and on the serial port, for when
                                      something goes wrong while starting
  Check the stick                     reads every file on the stick and checks it
  Firmware settings                   the PC's UEFI setup

The home screen
---------------
It opens on a home screen: big tiles for the jobs people bring the stick for, the everyday
programs under them, and at the top whether Secure Boot is on, whether there's internet,
and how many drives it found.

Drives and Partitions   DiskAnvil: partitions, health, wipe, back up, clone, rescue a
                        failing drive, recover lost partitions
Get Files Back          Find Lost Files: deleted files, or files off a formatted or
                        damaged drive, with previews (PhotoRec is in the start menu too)
Find Lost Partitions    TestDisk: lost partitions and broken boot sectors
Write an Image to USB   an ISO or a disk image onto a USB stick (compressed ones too)
Make a Windows USB      a stick that installs Windows 10 or 11
Check a USB Stick       bad spots, and sticks that are smaller than they say
Write-Protected Drive   a stick or card that won't let you save: why, and the fix
Why Did It Crash?       a Windows that keeps blue-screening or restarting: why
Windows Won't Start     stuck on an update, or a damaged registry file
Are My Files Still OK?  photos and papers checked for going bad over the years
Burn In a New Drive     a new drive worked hard once, before your files go on it
Write Speed Over Time   an SSD's fast cache, and its real speed after it
Copy This Stick         another stick like this one. After "Copy to memory" the stick
                        isn't mounted: mount it in DiskAnvil first
Programs                Files, Web Browser, Terminal, Task Manager, Text Editor, Save Logs
                        (see below), Read Me, and Firmware Settings (restarts into the
                        PC's setup, after asking)

Everything is also in the start menu: the DiskAnvil Live button on the taskbar.

Mount at Startup, Disk Cleanup, snapshots and the TRIM and scrub schedules are off in here:
this system starts fresh from the stick every time, so they'd change nothing.

The user is "rescue" and has no password. sudo doesn't ask for one.

Logs
----
Every time the stick starts a PC, it keeps notes on what happened: the PC's model, how it
started (UEFI or BIOS, Secure Boot on or off), the graphics and network, the drives and
their health, errors, and what DiskAnvil did. They go to the logs folder on the stick, one
folder per start, named by date and PC model. They're saved a couple of minutes after
starting, every few minutes after that, when you shut down, and when you click Save Logs.

They're plain text, so any PC can open them. If something didn't work, they're what to send
along with the bug report. They do contain the PC's and the drives' serial numbers, so look
before posting them somewhere public.

When the ISO runs as a CD (in VMware or VirtualBox, say), it can't write to itself. Plug in a
stick made from the same ISO and the logs go there. Or pick "Detailed messages" in the boot
menu and give the VM a serial port that writes to a file; everything that happens while it
starts ends up in that file.

Making the stick
----------------
DiskAnvil does it: File > Make a DiskAnvil Live USB, then pick this ISO and the stick (inside
the rescue system it copies the stick it's running from). That stick starts UEFI PCs (Secure
Boot on or off) and old BIOS PCs, and keeps its logs. It also works to write the ISO with
any image writer (DiskAnvil's Write Image to USB, Rufus, balenaEtcher, dd); that starts
both kinds too, but it's read-only, so it can't keep logs.

What it's made of
-----------------
Debian 13 with the LXQt desktop. DiskAnvil and everything else on it are free software.
The source and the build scripts are at https://github.com/DannyS124/diskanvil (rescue/).
