# tools/console.ps1 [-p port] 'cmd' ['@secs' | 'cmd' ...]
#
# console.sh for Windows: line commands to an rpi5* emulator's console on the
# Pi (127.0.0.1:6400 there, through ssh; RPI5_HOST, default robert-pi5):
# '@n' waits n seconds between them (quoted: a bare @ is PowerShell's
# splatting); the answers come back at the end. E.g.
#   tools\console.ps1 'T 10 PRINT "HI"\n' '@4' 'T RUN\n' '@2' 's /tmp/a.png' i
#Requires -Version 7.4
$port = 6400
$cmds = @($args | ForEach-Object { "$_" })
if ($cmds.Count -ge 2 -and $cmds[0] -eq '-p') {
    $port = [int]$cmds[1]
    $cmds = @($cmds | Select-Object -Skip 2)
}
function ConvertTo-ShellWord([string]$s) { "'" + $s.Replace("'", "'\''") + "'" }
$script = "exec 3<>/dev/tcp/127.0.0.1/$port || exit 1; "
foreach ($c in $cmds) {
    if ($c.StartsWith('@')) { $script += "sleep $(ConvertTo-ShellWord $c.Substring(1)); " }
    else { $script += "printf '%s\n' $(ConvertTo-ShellWord $c) >&3; " }
}
$script += 'timeout 1 cat <&3; true'
ssh -o BatchMode=yes ($env:RPI5_HOST ? $env:RPI5_HOST : 'robert-pi5') $script
exit $LASTEXITCODE
