@echo off
rem How ntfs-windows.img.zst was made: Windows itself formats a blank 64 MB stick (one MBR
rem partition, no file system yet) as NTFS, puts the test files on it (files.py makes them)
rem and deletes the same ones make.sh does. Run in a Windows VM, from a folder with the files.
rem Then blank-boot-code.py on the volume: Windows' boot code isn't ours to put in the repository.
setlocal
for /f %%d in ('powershell -NoProfile -Command "(Get-Disk | Where-Object { $_.Size -eq 64MB -and $_.BusType -eq 'USB' } | Select-Object -First 1).Number"') do set DISK=%%d
if "%DISK%"=="" (echo no 64 MB stick & exit /b 1)
powershell -NoProfile -Command "$p = Get-Partition -DiskNumber %DISK% | Select-Object -First 1; Format-Volume -Partition $p -FileSystem NTFS -NewFileSystemLabel WINNTFS -Confirm:$false | Out-Null; Set-Partition -DiskNumber %DISK% -PartitionNumber $p.PartitionNumber -NewDriveLetter T; 'formatted'"
rem Windows writes its own files to a new volume (System Volume Information) in the first
rem seconds; once they're there, they won't take the records of the files deleted below.
timeout /t 20 /nobreak >nul
mkdir "T:\Pictures\2024" "T:\Old stuff"
copy /y photo.jpg T:\photo.jpg >nul
copy /y holiday.jpg "T:\Holiday photo with a long name.jpg" >nul
copy /y notes.txt T:\notes.txt >nul
copy /y readme.txt T:\README.TXT >nul
copy /y pic1.png T:\Pictures\2024\pic1.png >nul
copy /y pic2.jpg T:\Pictures\2024\pic2.jpg >nul
copy /y letter.pdf "T:\Old stuff\letter.pdf" >nul
copy /y scan.png "T:\Old stuff\scan.png" >nul
timeout /t 3 /nobreak >nul
del "T:\Holiday photo with a long name.jpg" T:\notes.txt T:\README.TXT T:\Pictures\2024\pic1.png
rmdir /s /q "T:\Old stuff"
powershell -NoProfile -Command "Write-VolumeCache -DriveLetter T; 'flushed'"
dir /s T:\
