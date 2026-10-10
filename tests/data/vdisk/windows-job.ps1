# How win-*.vhd and win-dynamic.vhdx were made: Windows creates them with diskpart, the test disk
# (disk.raw, unpacked from disk.raw.zst next to this) is written into each one raw, and they're
# copied to a stick labelled VDXFER (exFAT) to come out of the Windows VM. Windows' own way of
# laying them out, next to qemu-img's (make.sh).
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class Raw {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    public static void Write(string device, string file) {
        using (var d = new FileStream(CreateFile(device, 0xC0000000, 3, IntPtr.Zero, 3, 0x20000000, IntPtr.Zero), FileAccess.ReadWrite, 1))
        using (var f = File.OpenRead(file)) {
            var buffer = new byte[1 << 20];
            int n;
            while ((n = f.Read(buffer, 0, buffer.Length)) > 0) d.Write(buffer, 0, n);
            d.Flush();
        }
    }
}
'@
$stick = (Get-Volume -FileSystemLabel VDXFER).DriveLetter
foreach ($v in @(@{ File = 'win-dynamic.vhdx'; Type = 'expandable' }, @{ File = 'win-dynamic.vhd'; Type = 'expandable' }, @{ File = 'win-fixed.vhd'; Type = 'fixed' })) {
    $path = "C:\dftest\$($v.File)"
    Remove-Item $path -ErrorAction SilentlyContinue
    "create vdisk file=$path maximum=48 type=$($v.Type)`nselect vdisk file=$path`nattach vdisk" | Set-Content C:\dftest\dp.txt -Encoding ascii
    diskpart /s C:\dftest\dp.txt | Out-Null
    Start-Sleep 3
    $disk = Get-Disk | Where-Object { $_.Location -eq $path }
    Set-Disk -Number $disk.Number -IsOffline $true -ErrorAction SilentlyContinue
    [Raw]::Write("\\.\PhysicalDrive$($disk.Number)", (Join-Path $PSScriptRoot 'disk.raw'))
    "select vdisk file=$path`ndetach vdisk" | Set-Content C:\dftest\dp.txt -Encoding ascii
    diskpart /s C:\dftest\dp.txt | Out-Null
    Copy-Item $path "${stick}:\$($v.File)"
    "$($v.File) $((Get-Item $path).Length) $((Get-FileHash $path -Algorithm SHA256).Hash)"
}
Write-VolumeCache -DriveLetter $stick
