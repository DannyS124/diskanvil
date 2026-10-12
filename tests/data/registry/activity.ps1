# Makes tests/data/registry/activity-*.hiv: hives shaped like a used Windows' SOFTWARE, SYSTEM and
# a user's NTUSER.DAT, with our own values where Windows keeps what a PC was used for: USB drives
# plugged in, programs run (UserAssist), recent files, searches, typed folders, the networks it
# joined. Run in Windows as an administrator with a drive as T: (the hives go to T:\hives).
$ErrorActionPreference = 'Stop'
$base = 'HKCU\Software\DFActivity'
if (Test-Path 'HKCU:\Software\DFActivity') { Remove-Item 'HKCU:\Software\DFActivity' -Recurse -Force }

function Hex([byte[]]$b) { ($b | ForEach-Object { $_.ToString('x2') }) -join '' }
function Add($path, $name, $type, $data) {
    $cmd = @('add', "$base\$path", '/f', '/t', $type)
    if ($name -eq '') { $cmd += '/ve' } else { $cmd += @('/v', $name) }
    if ($null -ne $data) { $cmd += @('/d', $data) }
    & reg.exe @cmd | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "reg add $path $name failed" }
}
function FileTime([datetime]$t) { Hex ([BitConverter]::GetBytes($t.ToFileTimeUtc())) }
function SystemTime([datetime]$t) {
    $b = New-Object byte[] 16
    $i = 0
    foreach ($v in @($t.Year, $t.Month, [int]$t.DayOfWeek, $t.Day, $t.Hour, $t.Minute, $t.Second, 0)) {
        $b[$i] = $v -band 0xFF; $b[$i + 1] = ($v -shr 8) -band 0xFF; $i += 2
    }
    Hex $b
}
function Utf16z($s) { Hex ([Text.Encoding]::Unicode.GetBytes($s + [char]0)) }
function Rot13($s) {
    -join ($s.ToCharArray() | ForEach-Object {
        $c = [int]$_
        if ($c -ge 97 -and $c -le 122) { [char](97 + ($c - 97 + 13) % 26) }
        elseif ($c -ge 65 -and $c -le 90) { [char](65 + ($c - 65 + 13) % 26) }
        else { $_ }
    })
}
# UserAssist's record (Windows 7 and later): 72 bytes, the run count at 4, the last run at 60.
function UserAssist($runs, [datetime]$last) {
    $b = New-Object byte[] 72
    [BitConverter]::GetBytes([int]$runs).CopyTo($b, 4)
    [BitConverter]::GetBytes($last.ToFileTimeUtc()).CopyTo($b, 60)
    Hex $b
}
$utc = [DateTimeKind]::Utc

# SOFTWARE
$nt = 'Software\Microsoft\Windows NT\CurrentVersion'
Add $nt 'ProductName' REG_SZ 'Windows 10 Pro'
Add $nt 'DisplayVersion' REG_SZ '23H2'
Add $nt 'CurrentBuildNumber' REG_SZ '22631'
Add $nt 'RegisteredOwner' REG_SZ 'Sam Tester'
Add $nt 'RegisteredOrganization' REG_SZ 'Anvil Test'
Add $nt 'InstallDate' REG_DWORD 1704100000
Add $nt 'SystemRoot' REG_SZ 'C:\WINDOWS'
Add "$nt\ProfileList\S-1-5-21-1-2-3-1001" 'ProfileImagePath' REG_EXPAND_SZ 'C:\Users\tester'
$net = "$nt\NetworkList\Profiles\{11111111-2222-3333-4444-555555555555}"
Add $net 'ProfileName' REG_SZ 'Cafe Wifi'
Add $net 'DateCreated' REG_BINARY (SystemTime (New-Object DateTime 2024, 3, 1, 10, 0, 0, $utc))
Add $net 'DateLastConnected' REG_BINARY (SystemTime (New-Object DateTime 2024, 6, 2, 18, 30, 0, $utc))

# SYSTEM
Add 'System\Select' 'Current' REG_DWORD 1
$cs = 'System\ControlSet001'
Add "$cs\Control\ComputerName\ComputerName" 'ComputerName' REG_SZ 'ANVIL-PC'
Add "$cs\Control\TimeZoneInformation" 'TimeZoneKeyName' REG_SZ 'GMT Standard Time'
Add "$cs\Control\Windows" 'ShutdownTime' REG_BINARY (FileTime (New-Object DateTime 2024, 6, 10, 22, 15, 0, $utc))
$props = 'Properties\{83da6326-97a6-4088-9453-a1923f573b29}'
# A stick with its own serial number (Windows 8 and later keep the times as the key's own value)...
$usb = "$cs\Enum\USBSTOR\Disk&Ven_SanDisk&Prod_Cruzer_Blade&Rev_1.00\4C530001230508116054&0"
Add $usb 'FriendlyName' REG_SZ 'SanDisk Cruzer Blade USB Device'
Add "$usb\$props\0064" '' REG_BINARY (FileTime (New-Object DateTime 2024, 2, 14, 8, 0, 0, $utc))
Add "$usb\$props\0066" '' REG_BINARY (FileTime (New-Object DateTime 2024, 6, 9, 17, 45, 0, $utc))
# ...and one without, so Windows made one up (Windows 7 keeps the times one level down, as Data).
$generic = "$cs\Enum\USBSTOR\Disk&Ven_Generic&Prod_Flash_Disk&Rev_8.07\7&2a1b3c4d&0"
Add "$generic\$props\0064\00000000" 'Data' REG_BINARY (FileTime (New-Object DateTime 2023, 11, 5, 12, 0, 0, $utc))

# NTUSER.DAT
$ex = 'NtUser\Software\Microsoft\Windows\CurrentVersion\Explorer'
$ua = "$ex\UserAssist\{CEBFF5CD-ACE2-4F4F-9178-9926F41749EA}\Count"
Add $ua (Rot13 'C:\Users\tester\Downloads\Tool\cleaner.exe') REG_BINARY (UserAssist 5 (New-Object DateTime 2024, 6, 8, 21, 0, 0, $utc))
Add $ua (Rot13 '{6D809377-6AF0-444B-8957-A3773F02200E}\Notepad++\notepad++.exe') REG_BINARY (UserAssist 12 (New-Object DateTime 2024, 6, 9, 9, 30, 0, $utc))
Add $ua (Rot13 'UEME_CTLSESSION') REG_BINARY (UserAssist 0 (New-Object DateTime 2024, 6, 9, 9, 30, 0, $utc))
Add "$ex\RunMRU" 'a' REG_SZ 'cmd\1'
Add "$ex\RunMRU" 'MRUList' REG_SZ 'a'
Add "$ex\RecentDocs" '0' REG_BINARY ((Utf16z 'bank statement.pdf') + '000000')
Add "$ex\RecentDocs" 'MRUListEx' REG_BINARY '00000000ffffffff'
Add "$ex\RecentDocs\.pdf" '0' REG_BINARY ((Utf16z 'bank statement.pdf') + '000000')
Add "$ex\WordWheelQuery" '0' REG_BINARY (Utf16z 'tax return')
Add "$ex\WordWheelQuery" 'MRUListEx' REG_BINARY '00000000ffffffff'
Add "$ex\TypedPaths" 'url1' REG_SZ '\\nas\photos'

$out = 'T:\hives'
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory $out | Out-Null
foreach ($h in @('Software', 'System', 'NtUser')) {
    & reg.exe save "$base\$h" "$out\activity-$($h.ToLower()).hiv" /y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "reg save $h failed" }
}
Remove-Item 'HKCU:\Software\DFActivity' -Recurse -Force
Get-ChildItem $out | Format-Table Name, Length -AutoSize
