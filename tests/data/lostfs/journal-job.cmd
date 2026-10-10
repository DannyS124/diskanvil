@echo off
rem How ntfs-journal.img.zst was made: Windows formats a blank 64 MB stick (one MBR partition, no
rem file system yet) as NTFS with its change journal on, puts files on it, deletes a folder and
rem then makes new files that take the deleted ones' records, so only the journal still knows
rem their names. Run in a Windows VM, from a folder with the files (files.py makes them); then
rem journal-finish.sh on the stick.
setlocal
for /f %%d in ('powershell -NoProfile -Command "(Get-Disk | Where-Object { $_.Size -eq 64MB -and $_.BusType -eq 'USB' } | Select-Object -First 1).Number"') do set DISK=%%d
if "%DISK%"=="" (echo no 64 MB stick & exit /b 1)
powershell -NoProfile -Command "$p = Get-Partition -DiskNumber %DISK% | Select-Object -First 1; Format-Volume -Partition $p -FileSystem NTFS -NewFileSystemLabel WINJRNL -Confirm:$false | Out-Null; Set-Partition -DiskNumber %DISK% -PartitionNumber $p.PartitionNumber -NewDriveLetter T; 'formatted'"
fsutil usn createjournal m=8388608 a=1048576 T:
rem Windows writes its own files to a new volume (System Volume Information) in the first
rem seconds; once they're there, they won't take the records freed below.
timeout /t 20 /nobreak >nul
mkdir T:\Drafts T:\Trip
copy /y notes.txt T:\Drafts\plan.txt >nul
copy /y letter.pdf "T:\Drafts\old letter.pdf" >nul
copy /y photo.jpg T:\Trip\beach.jpg >nul
copy /y pic2.jpg T:\Trip\sunset.jpg >nul
copy /y readme.txt T:\keep.txt >nul
for %%f in (T:\Drafts T:\Drafts\plan.txt "T:\Drafts\old letter.pdf" T:\Trip\beach.jpg T:\keep.txt) do fsutil file queryfileid %%f
timeout /t 3 /nobreak >nul
rmdir /s /q T:\Drafts
rem New files take the records the folder and its files had.
echo one> T:\new1.txt
echo two> T:\new2.txt
echo three> T:\new3.txt
for %%f in (T:\new1.txt T:\new2.txt T:\new3.txt) do fsutil file queryfileid %%f
del T:\Trip\beach.jpg
powershell -NoProfile -Command "Write-VolumeCache -DriveLetter T; 'flushed'"
fsutil usn readjournal T: csv
fsutil volume dismount T:
