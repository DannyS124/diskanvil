# Makes tests/data/registry/components.hiv: a hive shaped like Windows' COMPONENTS, with the three
# values that point at a waiting pending.xml, one that stays, and a key. Our own values.
$ErrorActionPreference = 'Stop'
$k = 'HKCU:\Software\DFTest\Components'
if (Test-Path 'HKCU:\Software\DFTest') { Remove-Item 'HKCU:\Software\DFTest' -Recurse -Force }
New-Item -Path $k -Force | Out-Null
New-ItemProperty -Path $k -Name 'StoreFormatVersion' -PropertyType String -Value '0.0.0.6' | Out-Null
New-ItemProperty -Path $k -Name 'PendingXmlIdentifier' -PropertyType Binary -Value ([byte[]](0x30,0,0x30,0,0x31,0,0x64,0)) | Out-Null
New-ItemProperty -Path $k -Name 'NextQueueEntryIndex' -PropertyType Binary -Value ([byte[]](1,0,0,0)) | Out-Null
New-ItemProperty -Path $k -Name 'AdvancedInstallersNeedResolving' -PropertyType Binary -Value ([byte[]](1)) | Out-Null
New-Item -Path "$k\CanonicalData\Catalogs" -Force | Out-Null
New-ItemProperty -Path "$k\CanonicalData\Catalogs" -Name 'x' -PropertyType DWord -Value 7 | Out-Null
Remove-Item C:\dftest\components.hiv -ErrorAction SilentlyContinue
& reg.exe save 'HKCU\Software\DFTest\Components' C:\dftest\components.hiv /y | Out-Null
Remove-Item 'HKCU:\Software\DFTest' -Recurse -Force
