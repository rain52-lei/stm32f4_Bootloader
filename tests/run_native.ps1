$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputFolder = Join-Path $projectRoot '.build\native'
New-Item -ItemType Directory -Path $outputFolder -Force | Out-Null
$tempFolder = Join-Path $projectRoot '.build\tmp'
New-Item -ItemType Directory -Path $tempFolder -Force | Out-Null
$env:TEMP = $tempFolder
$env:TMP = $tempFolder
$env:PATH = 'G:\Lvgl\lvgl\mingw64\bin;' + $env:PATH
$core = Join-Path $projectRoot 'BOOTLOADER\MDK-ARM\Core'
$stubs = Join-Path $PSScriptRoot 'native'
$testBinary = Join-Path $outputFolder 'test_firmware.exe'
& 'G:\Lvgl\lvgl\mingw64\bin\gcc.exe' -std=c99 -O1 -Wall -Wextra -Werror -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -static -I $stubs -I $core (Join-Path $stubs 'test_firmware.c') (Join-Path $core 'boot_flash.c') (Join-Path $core 'boot_update.c') (Join-Path $core 'boot_metadata.c') -o $testBinary
if ($LASTEXITCODE -ne 0) { throw 'Native firmware build failed' }
& $testBinary
if ($LASTEXITCODE -ne 0) { throw 'Native firmware tests failed' }
