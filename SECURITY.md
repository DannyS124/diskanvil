# Security

If you find a way DiskAnvil could wipe or change something it shouldn't, or a way for another
program, a drive or a file to make it do something, please report it privately (Security tab →
Report a vulnerability) instead of opening a public issue.

Only download DiskAnvil from https://github.com/DannyS124/diskanvil. Releases come with a
SHA256SUMS file, and the PKGBUILD checks the hash before building.

## How it's built to stay safe

- It doesn't listen for other programs: no network port, no socket, no D-Bus service of its own.
  `packaging/release.sh` refuses to build a release if that changes.
- It runs as you. Anything that needs root goes through UDisks or systemd, and your system's own
  password prompt (polkit) asks; DiskAnvil never sees that password.
- It only goes online when you open Check for Updates or Get Add-ons. The add-on list is signed with
  a key that isn't on GitHub, and every add-on is pinned to a commit and checked against a checksum.
- Everything that comes from a drive or a file (names, partition tables, backups, rescue maps,
  add-ons) is treated as untrusted: checked, size-limited, shown as plain text, and crash-tested
  with thousands of damaged versions before every release.
- Add-ons run without a shell, ask before running, and can't be steered by a drive's name. Look-only
  add-ons run in a read-only sandbox. See [docs/ADDONS.md](docs/ADDONS.md).
