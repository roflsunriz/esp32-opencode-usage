param(
    [switch]$NoBrowser,
    [ValidateRange(1024,65535)][int]$Port = 8765
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
if (-not (Get-Command bun -ErrorAction SilentlyContinue)) {
    throw 'Bun が必要です。https://bun.sh/ の公式手順でインストールしてから起動してください。'
}
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    throw 'Python 3.12以降が必要です。Pythonをインストールしてから起動してください。'
}
& python -c 'import serial' 2>$null
if ($LASTEXITCODE -ne 0) {
    & python -m pip install --user -r requirements.txt
    if ($LASTEXITCODE -ne 0) { throw 'USB通信ライブラリを導入できませんでした。requirements.txtのセットアップ手順を確認してください。' }
}
if (-not (Test-Path -LiteralPath (Join-Path $projectRoot 'node_modules'))) {
    $taskTemp = Join-Path $projectRoot '.private\tmp'
    $taskCache = Join-Path $projectRoot '.private\bun-cache'
    New-Item -ItemType Directory -Force -Path $taskTemp,$taskCache | Out-Null
    $env:TEMP = $taskTemp
    $env:TMP = $taskTemp
    $env:BUN_INSTALL_CACHE_DIR = $taskCache
    & bun install --frozen-lockfile
    if ($LASTEXITCODE -ne 0) { throw '依存関係のインストールに失敗しました。' }
}
$hostArguments = @('host/cli.ts', 'serve', '--port', "$Port")
if (-not $NoBrowser) { $hostArguments += '--open' }
& bun @hostArguments
exit $LASTEXITCODE
