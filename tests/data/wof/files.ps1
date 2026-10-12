# Three files of our own on W:, the same text in each (wof.img.zst's test makes it again).
Get-Volume -FileSystemLabel WOFTEST | Get-Partition | Set-Partition -NewDriveLetter W -ErrorAction SilentlyContinue
$t = (1..2000 | ForEach-Object { "DiskAnvil compressed file test, line $_" }) -join "`r`n"
New-Item -ItemType Directory -Force 'W:\Windows\System32\drivers', 'W:\Program Files\Tool' | Out-Null
foreach ($f in 'W:\Windows\System32\drivers\anvil.sys', 'W:\Program Files\Tool\tool.exe', 'W:\plain.txt') {
    [System.IO.File]::WriteAllText($f, $t)
}
'sha256 ' + (Get-FileHash 'W:\plain.txt').Hash
