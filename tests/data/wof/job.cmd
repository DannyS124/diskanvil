@echo off
rem Run in a Windows VM with a blank 16 MB disk attached, next to files.ps1; then wof-finish.sh.
rem The disk gets one NTFS volume, WOFTEST, with files of our own: compressed by compact.exe in
rem each of its four ways (one in a Windows folder, one in Program Files...), with NTFS's own
rem compression, two of them deleted after, and one left as it is.
powershell -NoProfile -Command "$d = Get-Disk | Where-Object { $_.Size -eq 16MB -and $_.PartitionStyle -eq 'RAW' } | Select-Object -First 1; Set-Content C:\dftest\disknum.txt $d.Number"
set /p N=<C:\dftest\disknum.txt
(echo select disk %N%& echo online disk noerr& echo attributes disk clear readonly noerr& echo create partition primary& echo format quick fs=ntfs label=WOFTEST& echo assign letter=W) > C:\dftest\wof.txt
diskpart /s C:\dftest\wof.txt
powershell -NoProfile -ExecutionPolicy Bypass -File files.ps1
compact /c /exe:xpress4k W:\Windows\System32\drivers\anvil.sys "W:\Program Files\Tool\tool.exe" W:\Mixed\mixed4k.bin
compact /c /exe:lzx W:\Windows\System32\lzx.dll W:\Mixed\mixedlzx.bin W:\Gone\gone-lzx.dll
compact /c /exe:xpress8k W:\Windows\System32\x8.dll
compact /c /exe:xpress16k W:\Windows\System32\x16.dll
compact /c W:\Docs\notes.txt W:\Docs\mixed.bin W:\Gone\gone.txt
compact /s:W:\ /q
powershell -NoProfile -Command "Get-ChildItem W:\ -Recurse -File | ForEach-Object { $_.FullName + ' ' + (Get-FileHash $_.FullName).Hash }"
del W:\Gone\gone.txt W:\Gone\gone-lzx.dll
(echo select volume W& echo remove letter=W) > C:\dftest\wof2.txt
diskpart /s C:\dftest\wof2.txt
