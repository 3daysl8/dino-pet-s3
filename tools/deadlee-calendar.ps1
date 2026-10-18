# Give Deadlee the family calendar (a private, read-only ICS link).
#
#   .\deadlee-calendar.ps1           ask for the link, save it on the server, sync, print counts
#   .\deadlee-calendar.ps1 -Add      add a second calendar instead of replacing the first
#   .\deadlee-calendar.ps1 -Remove   forget the link(s) and what was synced
#   .\deadlee-calendar.ps1 -Status   sync now and print counts
#
# Where to find the link:
#   Google    Settings > the calendar > "Secret address in iCal format"
#   iCloud    share the calendar as a Public Calendar and copy its webcal:// link
#   Fastmail  Settings > Calendars > the calendar's export / share options (private ICS link)
# The link is a secret (anyone with it can read the calendar), so it is typed here hidden and
# kept only in server:~/pet-bridge/calendar.url. This prints counts only, never event titles,
# so running it from a Claude session doesn't put the calendar in the chat.
# Deadlee keeps each event's title and time only, skips events marked private, and re-syncs
# every couple of hours. Everything in that calendar can be told to whoever is talking to the
# toy, so a shared family calendar is a better pick than a work or personal one.
param([switch]$Add, [switch]$Remove, [switch]$Status)
$ErrorActionPreference = 'Stop'
$server = 'alex@192.168.1.50'
$ssh = "$env:SystemRoot\System32\OpenSSH\ssh.exe"

function Sync-Now {
    $token = (& $ssh -o BatchMode=yes $server 'cat ~/pet-bridge/pet.token').Trim()
    $r = Invoke-RestMethod -Method Post -Uri 'http://192.168.1.50:8090/calendar/sync' -Headers @{ 'X-Pet-Token' = $token }
    if ($r.ok) { "Synced: $($r.events) events in the next two weeks. See them on the web page (Digest tab)." }
    else { "Sync failed ($($r.error)). $($r.events) events kept from before." }
}

if ($Remove) {
    & $ssh -o BatchMode=yes $server 'rm -f ~/pet-bridge/calendar.url ~/pet-bridge/brain/calendar.json'
    'Calendar link and synced events removed.'
} elseif ($Status) {
    Sync-Now
} else {
    $secure = Read-Host 'Paste the private ICS link (hidden)' -AsSecureString
    $link = [Runtime.InteropServices.Marshal]::PtrToStringBSTR([Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)).Trim()
    if ($link -notmatch '^(https|webcal)://\S+$') { throw 'That does not look like an https:// or webcal:// link.' }
    $op = if ($Add) { '>>' } else { '>' }
    $link | & $ssh -o BatchMode=yes $server "umask 077; tr -d '\r' $op ~/pet-bridge/calendar.url"
    'Link saved on the server.'
    Sync-Now
}
