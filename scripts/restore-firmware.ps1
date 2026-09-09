param(
    [Parameter(Mandatory)][string]$Port,
    [string]$Python = 'python',
    [string]$BackupPath = '.private\backups\original-flash.bin'
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
$originalPath = [System.IO.Path]::GetFullPath($BackupPath)
$manifest = Get-Content -Raw -LiteralPath "$originalPath.json" | ConvertFrom-Json
$file = Get-Item -LiteralPath $originalPath
$hash = (Get-FileHash -LiteralPath $originalPath -Algorithm SHA256).Hash
if ($manifest.version -ne 1 -or $manifest.size -ne $file.Length -or $manifest.sha256 -ne $hash) {
    throw 'バックアップの検証に失敗しました。元ファイルを確認してください。'
}
& $Python -m esptool --port $Port --baud 460800 write-flash 0 $originalPath
if ($LASTEXITCODE -ne 0) { throw '元のファームウェアの復元に失敗しました。USB接続を確認して再実行してください。' }
& $Python -m esptool --port $Port --baud 460800 verify-flash 0 $originalPath
if ($LASTEXITCODE -ne 0) { throw '復元後の全フラッシュが一致しません。実機を利用せず再復元してください。' }
Write-Output '元の全フラッシュを書き戻し、完全一致を確認しました。'
