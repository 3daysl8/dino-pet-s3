# Edit Deadlee's brain files on your PC with Notepad (the server is headless).
#
#   .\deadlee-brain.ps1 pull   copy FAMILY.md + SOUL.md from the server to Documents\Deadlee
#                              (a local copy that already exists is kept as .bak) and open FAMILY.md
#   .\deadlee-brain.ps1 push   copy them back to the server, then check FAMILY.md
#
# The check prints counts only ("3 people, 5 birthdays..."), never the contents, so
# running this from a Claude session doesn't put family details in the chat.
# Deadlee reads the files at the start of its next conversation; no restart needed.
param([ValidateSet('pull', 'push')][string]$Action = 'pull')
$ErrorActionPreference = 'Stop'
$server = 'alex@192.168.1.50'
$local = Join-Path $env:USERPROFILE 'Documents\Deadlee'
$files = 'FAMILY.md', 'SOUL.md'
$ssh = "$env:SystemRoot\System32\OpenSSH"

if ($Action -eq 'pull') {
    New-Item -ItemType Directory -Force -Path $local | Out-Null
    foreach ($f in $files) {
        $dst = Join-Path $local $f
        if (Test-Path $dst) { Copy-Item $dst "$dst.bak" -Force }
        & "$ssh\scp.exe" -q -o BatchMode=yes "${server}:pet-bridge/brain/$f" $dst
        if ($LASTEXITCODE -ne 0) { throw "couldn't copy $f from the server (is it on?)" }
    }
    Write-Host "Copied to $local. Edit, save, then run: .\deadlee-brain.ps1 push"
    Start-Process notepad.exe (Join-Path $local 'FAMILY.md')
} else {
    foreach ($f in $files) {
        $src = Join-Path $local $f
        if (-not (Test-Path $src)) { continue }
        & "$ssh\scp.exe" -q -o BatchMode=yes $src "${server}:pet-bridge/brain/$f"
        if ($LASTEXITCODE -ne 0) { throw "couldn't copy $f to the server (is it on?)" }
        Write-Host "$f -> the server"
    }
    & "$ssh\ssh.exe" -o BatchMode=yes $server 'docker exec pet-bridge python family.py /data/brain/FAMILY.md'
}
