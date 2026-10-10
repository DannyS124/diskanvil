# How ntfs-shadow.img.zst was made (shadow-job.cmd runs this): Windows formats a blank 1 GB USB
# disk (one MBR partition, no file system yet) as NTFS, puts files on it, takes a shadow copy,
# changes some, takes another and changes more. Then, for the tests to compare with, each copy as
# Windows itself reads it: its files' SHA-256 and the whole volume's, a MiB at a time. Run in a
# Windows VM from a folder with the files (files.py makes them); then shadow-finish.sh.
$ErrorActionPreference = 'Stop'
$disk = Get-Disk | Where-Object { $_.Size -eq 1GB -and $_.BusType -eq 'USB' } | Select-Object -First 1
if (-not $disk) { 'no 1 GB disk'; exit 1 }
$part = Get-Partition -DiskNumber $disk.Number | Select-Object -First 1
$part | Format-Volume -FileSystem NTFS -NewFileSystemLabel WINVSS -Confirm:$false | Out-Null
Set-Partition -DiskNumber $disk.Number -PartitionNumber $part.PartitionNumber -NewDriveLetter V
# Windows writes its own files to a new volume in the first seconds.
Start-Sleep 20

function Random-Bytes($seed, $size) { $b = [byte[]]::new($size); [System.Random]::new($seed).NextBytes($b); $b }
function Overwrite($path, $at, $bytes) {
    $f = [IO.File]::Open($path, 'Open', 'ReadWrite'); $f.Position = $at; $f.Write($bytes, 0, $bytes.Length); $f.Close()
}
# Files copied off the job's CD keep its read-only flag.
function Put($from, $to) { Copy-Item $from $to -Force; Set-ItemProperty $to IsReadOnly $false }
function Snapshot() {
    Write-VolumeCache -DriveLetter V
    $r = Invoke-CimMethod -ClassName Win32_ShadowCopy -MethodName Create -Arguments @{ Volume = 'V:\' }
    if ($r.ReturnValue -ne 0) { "shadow copy failed: $($r.ReturnValue)"; exit 1 }
    Start-Sleep 3
}

New-Item -ItemType Directory V:\Docs | Out-Null
Put photo.jpg V:\Docs\photo.jpg
Put letter.pdf V:\Docs\letter.pdf
Put notes.txt V:\notes.txt
[IO.File]::WriteAllBytes('V:\big.bin', (Random-Bytes 7 (384KB)))
Snapshot
Set-Content V:\notes.txt 'The second version of the notes, longer than the first one was.'
Remove-Item V:\Docs\photo.jpg
Put holiday.jpg V:\Docs\holiday.jpg
Overwrite 'V:\big.bin' (128KB) (Random-Bytes 8 (64KB))
Snapshot
Remove-Item V:\Docs\letter.pdf
Put pic2.jpg V:\Docs\holiday.jpg
Put kitten.jpg V:\kitten.jpg
Overwrite 'V:\big.bin' 0 (Random-Bytes 9 (40KB))
Write-VolumeCache -DriveLetter V

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Microsoft.Win32.SafeHandles;
public static class Shadow {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    static FileStream Open(string path) {
        var h = CreateFile(path, 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h.IsInvalid) return null;
        return new FileStream(h, FileAccess.Read, 1 << 20);
    }
    static string Hex(byte[] h) { return BitConverter.ToString(h).Replace("-", "").ToLowerInvariant(); }
    public static string File(string path) {
        using (var f = Open(path)) {
            if (f == null) return "missing";
            using (var sha = SHA256.Create()) return Hex(sha.ComputeHash(f));
        }
    }
    // The volume a MiB at a time; the buffer is cleared first, since Windows leaves it as it was
    // for blocks no copy keeps.
    public static string[] Volume(string path) {
        var lines = new System.Collections.Generic.List<string>();
        using (var f = Open(path))
        using (var sha = SHA256.Create()) {
            var buffer = new byte[1 << 20];
            for (int i = 0; ; i++) {
                Array.Clear(buffer, 0, buffer.Length);
                int n = 0;
                try { n = f.Read(buffer, 0, buffer.Length); } catch (IOException) { break; }
                if (n <= 0) break;
                lines.Add(i + " " + n + " " + Hex(sha.ComputeHash(buffer, 0, n)));
            }
        }
        return lines.ToArray();
    }
}
'@

$volume = (Get-Volume -DriveLetter V).Path
$copies = Get-CimInstance Win32_ShadowCopy | Where-Object { $_.VolumeName -eq $volume } | Sort-Object InstallDate
foreach ($s in $copies) {
    "copy $($s.ID) $($s.InstallDate.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss')) $($s.DeviceObject)"
    foreach ($f in 'notes.txt', 'Docs\photo.jpg', 'Docs\letter.pdf', 'Docs\holiday.jpg', 'big.bin', 'kitten.jpg') {
        "file $f $([Shadow]::File($s.DeviceObject + '\' + $f))"
    }
    [Shadow]::Volume($s.DeviceObject) | ForEach-Object { "mib $_" }
}
fsutil volume dismount V:
