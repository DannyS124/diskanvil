# Making a DiskAnvil add-on

An add-on is one file, `addon.json`, that adds actions to DiskAnvil's right-click menu and Tools
menu. An action runs a command on the selected drive.

Put it in `~/.local/share/diskanvil/addons/<id>/addon.json`, or use Tools → Add-ons → Install from
File. There are examples in [`examples/addons`](../examples/addons).

Don't feel like writing JSON? Tools → Add-ons → Make an Add-on does all of this in a window, checks it
as you go, and has a Test button that runs it on the selected drive before you save.

## Example
```json
{
  "id": "open-terminal",
  "name": "Open Terminal Here",
  "version": "1.0",
  "author": "you",
  "description": "Opens a terminal in a mounted drive's folder.",
  "actions": [
    {
      "label": "Open Terminal Here",
      "icon": "utilities-terminal",
      "applies_to": "volume",
      "when": ["mounted"],
      "command": ["konsole", "--workdir", "{mountpoint}"]
    }
  ]
}
```

## Fields
- `id`: lowercase letters, digits and dashes. Also the folder name.
- `name`, `version`, `author`, `description`: shown in the Add-ons window.
- `actions`: one or more of:
  - `label`: the menu text.
  - `icon`: an icon name from your icon theme (optional).
  - `applies_to`: `volume` (a partition), `disk`, `free` (unallocated space) or `any`.
  - `when`: conditions that all have to be true: `mounted`, `unmounted`, `removable`, `internal`,
    `encrypted`, `unlocked`, `locked`, `has-health`, or `filesystem:ext4|vfat` (any of those).
  - `command`: the program and its arguments, as a list. Placeholders: `{device}` (`/dev/sdb1`),
    `{disk}` (`/dev/sdb`), `{mountpoint}`, `{label}`, `{uuid}`, `{fstype}`, `{size}` (bytes),
    `{model}`, `{home}`.
  - `output`: where what it prints goes: `terminal` (a terminal window), `window` (a DiskAnvil window
    with Stop, Copy and Save), or `none` (the default). The old `"terminal": true` still works.
  - `ask`: things to ask in a form before it runs, each `{"id": ..., "type": ..., "label": ...,
    "default": ...}`. Types: `text`, `number` (with `min` and `max`), `choice` (with `choices`: a list
    of strings, or of `{"label": ..., "value": ...}`), `check` (with the `on` and `off` values it puts in
    the command), `folder` and `file` (with a Browse button). Use them in `command` as `{ask:<id>}`.
    At most 20.
  - `confirm`: a question to ask before running (placeholders work here too).
  - `look_only`: `true` for actions that only look at things. They run in a read-only sandbox
    (needs bubblewrap): no changing files, no network, and other programs' sockets aren't there.
    A shell is fine in there.
  - `system_disks`: `true` to also offer a `look_only` action on the disk your system runs from.
    Without `look_only` it's ignored.
- `settings`: fields like `ask`, but set once in the Add-ons window (Settings…) instead of every time.
  Use them as `{setting:<id>}`.
- `theme`: makes the add-on a theme (it can have actions too, or only the theme). See Themes below.

## Rules
- Commands run as you, never as root, and without a shell. If something needs root, put `pkexec` in
  the command (or `sudo` with `"terminal": true`). DiskAnvil then warns and asks before every run.
- Whoever made a drive picked its name, so values from the drive (`{label}`, `{model}`, `{uuid}`,
  `{fstype}`, `{mountpoint}`) are checked first:
  - one that would start an argument with `-` isn't run, so a drive named `--delete` can't become
    an option. If a value goes first in an argument to a program that takes options, put `--`
    before it anyway;
  - `/` in a name becomes `_`, so a name stays one folder name (UDisks does the same under
    /run/media);
  - names that are `.` or `..`, or have hidden characters in them, are refused.
- The program to run (the first part of `command`) can't come from the drive or a form: only `{home}`
  works there.
- Typed form answers and settings (`text`, `number`, `folder`, `file`) follow the same rules as drive
  values. `choice` and `check` values come from the add-on file itself, so they can be options like
  `--checksum`. An argument that's just `{ask:x}` or `{setting:x}` and comes out empty is left out, so a
  checkbox can add or drop a flag.
- `"output": "window"` can't be used with `sudo`, `su` or `doas`: they need a terminal to ask for the
  password.
- An add-on can't pin itself to the toolbar or give itself a shortcut. Only you can, in the Add-ons
  window.
- Before an action runs, DiskAnvil shows the command, one part per line, and asks. "Don't ask again"
  is remembered for that exact add-on file, so any change to the file asks again. Actions that use
  admin power, or run a shell or script interpreter, ask every time.
- Only `look_only` actions with `system_disks` are offered for the disk your system runs from.
- DiskAnvil notes the add-ons it installs. One that turns up in the folder some other way (or
  changes) is marked "Added outside", and its actions warn about it until you press I Added It in
  Tools → Add-ons. Hand-made add-ons get that too: press I Added It once you've checked yours.
- Only install add-ons you trust, the same as any other program.

## Themes
A theme sets the colors DiskAnvil draws with. Every color is `#rrggbb`.
```json
{
  "id": "my-theme",
  "name": "My Theme",
  "theme": {
    "palette": {"window": "#0a0a0f", "base": "#13131c", "text": "#cbd6e6", "highlight": "#00b4ff"},
    "colors": {"partition": "#00b4ff", "free": "#44485a", "danger": "#ff2d55"},
    "filesystems": {"ext4": "#00e676", "vfat": "#ffb300"},
    "usage": ["#0e7fb8", "#10a35a", "#b8860b"]
  }
}
```
- `palette` (optional): `window`, `base`, `alternate`, `button`, `text`, `highlight`,
  `highlighted_text`, `link`, `mid`. Leave it out to keep your desktop's colors.
- `colors`: `partition`, `free`, `selection`, `good`, `warning`, `danger`, `muted`, the block map's
  `map_good`, `map_slow`, `map_retry`, `map_bad`, `map_unread`, and Disk Usage's `usage_small_files` and
  `usage_file`, and `encrypted` (the strip on a locked partition). Anything left out stays as it was.
- `filesystems`: a partition color per file system (`ext4`, `btrfs`, `vfat`, `exfat`, `ntfs`, `swap`,
  `crypto_LUKS`...).
- `usage`: the colors Disk Usage takes turns with.

Danger has to stay red, warning orange or yellow and good green, and text has to stay readable on its
background. Colors that don't are swapped for DiskAnvil's own, and a palette with unreadable text isn't
used. The Theme Maker (View → Theme → Make a Theme) shows all of this as you pick. A theme from an
add-on marked "Added outside" isn't used until you press I Added It.

## Sharing an add-on
There's no online list of add-ons yet. To share one, send its `addon.json` (and any files next to it); the
other person adds it with Tools → Add-ons → Install from File, and DiskAnvil shows each action's command and asks
before it runs, the same as for any add-on. Only install add-ons from people you trust.
