param(
    [string]$OutFile = "",
    [switch]$RunSelfTest
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutFile) {
    $OutFile = Join-Path $root "SnapTool.exe"
}

if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    throw "未找到 g++，请先安装 MinGW-w64 并将其 bin 目录加入 PATH。"
}

# 检测输出文件是否被当前正在运行的进程占用
if (Test-Path $OutFile) {
    try {
        $fs = [System.IO.File]::Open($OutFile, 'Open', 'ReadWrite', 'None')
        $fs.Close()
    } catch {
        throw "目标文件 $OutFile 正被占用，请先在右下角托盘退出正在运行的 SnapTool 后重试。"
    }
}

Write-Host "[1/2] 编译纯原生单文件 $OutFile ..." -ForegroundColor Cyan

& g++ -Os -s -std=c++17 -Wall -Wextra -mwindows -municode `
    -static -static-libgcc -static-libstdc++ `
    (Join-Path $root "src\main.cpp") `
    -o $OutFile `
    -luser32 -lgdi32 -lgdiplus -lshell32 -lcomdlg32 -ldwmapi -ladvapi32

if ($LASTEXITCODE -ne 0) {
    throw "编译失败！"
}

$sizeKB = [Math]::Round((Get-Item $OutFile).Length / 1KB, 1)
Write-Host "[2/2] 编译成功：$OutFile ($sizeKB KB)" -ForegroundColor Green

if ($RunSelfTest) {
    Write-Host "[自测] 执行端到端验证..." -ForegroundColor Cyan
    $p = Start-Process -FilePath $OutFile -ArgumentList "--selftest" -Wait -PassThru
    if ($p.ExitCode -ne 0) {
        throw "端到端自测失败，ExitCode=$($p.ExitCode)"
    }
    Write-Host "[自测] 通过 (ExitCode=0)" -ForegroundColor Green
}
