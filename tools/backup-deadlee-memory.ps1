# Copy Deadlee's brain folder (server:~/pet-bridge/brain: SOUL, FAMILY, MEMORY,
# people/, skills/, gaps) to your PC. Voiceprints (brain/voices) are biometric
# and are left on the server.
# Run nightly by the "Deadlee memory backup" scheduled task (see HANDOVER.md).
# Keeps 60 days of dated copies in %USERPROFILE%\Backups\deadlee and logs each run.
$ErrorActionPreference = 'Stop'
$dest = Join-Path $env:USERPROFILE 'Backups\deadlee'
$log  = Join-Path $dest 'backup.log'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$out = Join-Path $dest ("brain-{0}.tgz" -f (Get-Date -Format 'yyyy-MM-dd'))

# Archive on the server, then scp it (PowerShell 5.1's ">" would re-encode binary output as text).
$ssh = "$env:SystemRoot\System32\OpenSSH"
& "$ssh\ssh.exe" -o BatchMode=yes -o ConnectTimeout=15 alex@192.168.1.50 `
    "tar -C pet-bridge --exclude=brain/voices -czf /tmp/deadlee-brain.tgz brain"
if ($LASTEXITCODE -eq 0) {
    & "$ssh\scp.exe" -q -o BatchMode=yes -o ConnectTimeout=15 alex@192.168.1.50:/tmp/deadlee-brain.tgz $out
}
if ($LASTEXITCODE -ne 0) {
    Add-Content $log ("{0} FAILED (ssh exit {1}) - is the server on?" -f (Get-Date -Format s), $LASTEXITCODE)
    exit 1
}

# Sanity check: it must be a real archive holding FAMILY.md, not an error or an empty file.
$files = & tar.exe -tzf $out 2>$null
if ($LASTEXITCODE -ne 0 -or -not ($files -match 'brain/FAMILY.md')) {
    Add-Content $log ("{0} FAILED (archive unreadable or missing FAMILY.md)" -f (Get-Date -Format s))
    exit 1
}

Get-ChildItem $dest -Filter 'brain-*.tgz' |
    Where-Object { $_.LastWriteTime -lt (Get-Date).AddDays(-60) } | Remove-Item
Get-ChildItem $dest -Filter 'memory-*.json' |
    Where-Object { $_.LastWriteTime -lt (Get-Date).AddDays(-60) } | Remove-Item
Add-Content $log ("{0} ok, {1} files" -f (Get-Date -Format s), @($files).Count)
