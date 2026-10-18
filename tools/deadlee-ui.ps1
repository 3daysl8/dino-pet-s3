# Open Deadlee's web page (chat by text, edit the brain files and skills) already unlocked.
#
#   .\deadlee-ui.ps1          open it in the default browser on your PC
#   .\deadlee-ui.ps1 -Copy    put the unlocked link on the clipboard instead (to send to a phone)
#
# The page lives on the bridge: http://192.168.1.50:8090/ui (home network only). It needs the
# pet token; this reads it from the server and hands it over in the link's #fragment, which the
# browser never sends anywhere. The page keeps it in that browser and tidies the address bar.
# The token is never printed, so running this from a Claude session doesn't put it in the chat.
param([switch]$Copy)
$ErrorActionPreference = 'Stop'
$server = 'alex@192.168.1.50'
$ssh = "$env:SystemRoot\System32\OpenSSH"

$token = (& "$ssh\ssh.exe" -o BatchMode=yes $server 'cat ~/pet-bridge/pet.token').Trim()
if (-not $token) { throw 'Could not read the pet token from the server.' }
$url = "http://192.168.1.50:8090/ui#token=$token"
if ($Copy) {
    Set-Clipboard $url
    'Unlocked link copied. Anyone with it can talk to Deadlee and edit the brain, so only send it to yourself.'
} else {
    Start-Process $url
    'Opened Deadlee in the browser.'
}
