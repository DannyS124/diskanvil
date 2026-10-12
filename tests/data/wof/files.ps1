# Files of our own on W:, in every way Windows compresses them (wof.img.zst's tests make the same
# bytes again and compare).
Get-Volume -FileSystemLabel WOFTEST | Get-Partition | Set-Partition -NewDriveLetter W -ErrorAction SilentlyContinue
$t = (1..2000 | ForEach-Object { "DiskAnvil compressed file test, line $_" }) -join "`r`n"
# "Mixed": 70000 bytes that don't compress (stored as they are), the text, then 8192 x86 calls
# (0xE8 and an offset that goes from +32768 down past zero), for LZX's E8 translation.
Add-Type -TypeDefinition @"
public static class Anvil {
    public static byte[] Mixed(byte[] text) {
        byte[] o = new byte[70000 + text.Length + 8192 * 5];
        ulong x = 12345;
        for (int i = 0; i < 70000; i++) { x = (x * 1103515245 + 12345) % 2147483648; o[i] = (byte)(x >> 16); }
        System.Array.Copy(text, 0, o, 70000, text.Length);
        int at = 70000 + text.Length;
        for (int i = 0; i < 8192; i++) {
            uint v = unchecked((uint)(0x8000 - 5 * i));
            o[at++] = 0xE8;
            o[at++] = (byte)v; o[at++] = (byte)(v >> 8); o[at++] = (byte)(v >> 16); o[at++] = (byte)(v >> 24);
        }
        return o;
    }
}
"@
$textBytes = [System.Text.Encoding]::ASCII.GetBytes($t)
$mixed = [Anvil]::Mixed($textBytes)
New-Item -ItemType Directory -Force 'W:\Windows\System32\drivers', 'W:\Program Files\Tool', 'W:\Mixed', 'W:\Docs', 'W:\Gone' | Out-Null
foreach ($f in 'W:\Windows\System32\drivers\anvil.sys', 'W:\Program Files\Tool\tool.exe', 'W:\plain.txt',
               'W:\Windows\System32\lzx.dll', 'W:\Windows\System32\x8.dll', 'W:\Windows\System32\x16.dll',
               'W:\Docs\notes.txt', 'W:\Gone\gone.txt') {
    [System.IO.File]::WriteAllBytes($f, $textBytes)
}
foreach ($f in 'W:\Mixed\mixed4k.bin', 'W:\Mixed\mixedlzx.bin', 'W:\Docs\mixed.bin', 'W:\Gone\gone-lzx.dll') {
    [System.IO.File]::WriteAllBytes($f, $mixed)
}
'sha256 text ' + (Get-FileHash 'W:\plain.txt').Hash
'sha256 mixed ' + (Get-FileHash 'W:\Mixed\mixed4k.bin').Hash
