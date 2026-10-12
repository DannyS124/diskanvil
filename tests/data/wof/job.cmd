@echo off
rem Run in a Windows VM with a blank 16 MB disk attached, next to files.ps1; then wof-finish.sh.
rem The disk gets one NTFS volume, WOFTEST, with three files of our own: two kept compressed by
rem compact.exe (one in a Windows folder, one in Program Files), one left as it is.
powershell -NoProfile -Command "$d = Get-Disk | Where-Object { $_.Size -eq 16MB -and $_.PartitionStyle -eq 'RAW' } | Select-Object -First 1; Set-Content C:\dftest\disknum.txt $d.Number"
set /p N=<C:\dftest\disknum.txt
(echo select disk %N%& echo online disk noerr& echo attributes disk clear readonly noerr& echo create partition primary& echo format quick fs=ntfs label=WOFTEST& echo assign letter=W) > C:\dftest\wof.txt
diskpart /s C:\dftest\wof.txt
powershell -NoProfile -ExecutionPolicy Bypass -File files.ps1
compact /c /exe:xpress4k W:\Windows\System32\drivers\anvil.sys "W:\Program Files\Tool\tool.exe"
compact /q W:\Windows\System32\drivers\anvil.sys "W:\Program Files\Tool\tool.exe" W:\plain.txt
(echo select volume W& echo remove letter=W) > C:\dftest\wof2.txt
diskpart /s C:\dftest\wof2.txt
