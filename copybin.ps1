#Requires -Version 5.0
# copybin.ps1 - 将首次烧录的 4 个 bin 复制到桌面
# 用法: 双击 copybin.bat  或  .\copybin.ps1
# 本文件须以 UTF-8(带 BOM) 保存，以便 Windows PowerShell 5.1 正确解析中文。

$ErrorActionPreference = "Stop"

# 不要 chcp 65001 / 不要强行改成 UTF-8。
# 中文 Windows 控制台默认 GBK(CP936)。脚本以 BOM 保证源码中文正确，
# Write-Host 会把 Unicode 转成控制台编码，汉字即可正常显示。

$Root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Root "build"
$Dest  = Join-Path ([Environment]::GetFolderPath("Desktop")) "ESP32P4_FirstFlash"

$Files = @(
    @{ Src = Join-Path $Build "bootloader\bootloader.bin";           Dst = "bootloader.bin" }
    @{ Src = Join-Path $Build "partition_table\partition-table.bin"; Dst = "partition-table.bin" }
    @{ Src = Join-Path $Build "ota_data_initial.bin";                Dst = "ota_data_initial.bin" }
    @{ Src = Join-Path $Build "HowToCreateProject.bin";              Dst = "HowToCreateProject.bin" }
)

Write-Host ""
Write-Host "[copybin] 源目录: $Build"
Write-Host "[copybin] 目标目录: $Dest"
Write-Host ""

$Missing = @($Files | Where-Object { -not (Test-Path -LiteralPath $_.Src) })
if ($Missing.Count -gt 0) {
    Write-Host "[错误] 缺少编译产物，请先执行: idf.py build" -ForegroundColor Red
    foreach ($m in $Missing) {
        Write-Host "  缺失: $($m.Src)" -ForegroundColor Red
    }
    exit 1
}

New-Item -ItemType Directory -Force -Path $Dest | Out-Null

foreach ($f in $Files) {
    $target = Join-Path $Dest $f.Dst
    Copy-Item -LiteralPath $f.Src -Destination $target -Force
    $len = (Get-Item -LiteralPath $target).Length
    Write-Host ("  OK  {0,-28} {1,10} bytes" -f $f.Dst, $len)
}

$Readme = @"
ESP32-P4 首次烧录文件
Flash: 16MB, DIO, 80MHz

地址映射:
  0x2000  bootloader.bin
  0x8000  partition-table.bin
  0x10000 ota_data_initial.bin
  0x20000 HowToCreateProject.bin
"@

$readmePath = Join-Path $Dest "烧录地址说明.txt"
$utf8Bom = New-Object System.Text.UTF8Encoding $true
[System.IO.File]::WriteAllText($readmePath, $Readme.Trim() + [Environment]::NewLine, $utf8Bom)

Write-Host ""
Write-Host "[完成] 已复制到: $Dest" -ForegroundColor Green
Write-Host "烧录地址: 0x2000 / 0x8000 / 0x10000 / 0x20000"
Write-Host ""
