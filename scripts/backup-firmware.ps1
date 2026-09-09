param(
    [Parameter(Mandatory)][string]$Port,
    [string]$Python = 'python',
    [string]$OutputPath = '.private\backups\original-flash.bin'
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
$backupPath = [System.IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $backupPath) { throw '既存バックアップを上書きしません。別の出力先を指定してください。' }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $backupPath) | Out-Null
& $Python -m esptool --port $Port --baud 460800 read-flash 0 ALL $backupPath
if ($LASTEXITCODE -ne 0) { throw '全フラッシュの退避に失敗しました。書き込みを行わないでください。' }
& $Python -m esptool --port $Port --baud 460800 verify-flash 0 $backupPath
if ($LASTEXITCODE -ne 0) { throw 'バックアップと実機が一致しません。書き込みを行わないでください。' }
$file = Get-Item -LiteralPath $backupPath
$manifest = @{ version = 1; size = $file.Length; sha256 = (Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash; createdAt = (Get-Date).ToUniversalTime().ToString('o') }
$manifest | ConvertTo-Json | Set-Content -LiteralPath "$backupPath.json" -Encoding UTF8
Write-Output '全フラッシュのバックアップと実機との照合が完了しました。'
