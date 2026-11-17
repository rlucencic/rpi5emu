# tools/pi.ps1 <project> [build | run [options] | sync | headless [options] | shot out.png | log]
#
# pi.sh for Windows (PowerShell 7.4+, Windows' OpenSSH, Git for Windows): from
# this computer to the Pi (RPI5_HOST, default robert-pi5), where the rpi5*
# projects are built and run:
#   sync    the working trees (rpi5emu, the project, esp32emu and the
#           project's esp32* sibling: uncommitted changes and new files too,
#           not what .gitignore leaves out) into the Pi's ~/Development,
#           through git (no rsync here): each tree a commit made with an index
#           of its own (git's here untouched), pushed to the Pi's clone
#           (refs/pi-sync/head) and checked out there with an index of its own
#           too (the Pi's index and HEAD untouched). git gives the text files
#           their LF line ends back (core.autocrlf checks them out with CRLF
#           here), keeps the executable bits Windows doesn't have, sends only
#           what changed and rewrites only the files that differ (ninja
#           rebuilds only those). A file deleted here after a sync is deleted
#           there at the next
#   build   sync, then cmake --build there (configures the first time)
#   run     the program there, its window here (ssh -C -Y: X forwarding,
#           compressed) on DISPLAY's X server (default localhost:0: VcXsrv's
#           or MobaXterm's, started when none answers), its log on this
#           terminal; options go to it. SDL draws in software (no GLX the
#           Pi's Mesa can use) and moves the grabbed mouse by warps (no XInput2)
#   headless [options]  the program there without a window (SDL's offscreen
#           display) and started at once, in the background: drive it with
#           console.ps1 (q ends it)
#   shot out.png        the machine's picture from a running one
#   log                 the headless one's log
#Requires -Version 7.4
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true   # set -e: a program failing ends the script

$project = "$($args[0])"
if (-not $project) { throw 'project, e.g. rpi5zx' }
$what = $args.Count -gt 1 ? "$($args[1])" : 'build'
$rest = @($args | Select-Object -Skip 2 | ForEach-Object { "$_" })
$pi = $env:RPI5_HOST ? $env:RPI5_HOST : 'robert-pi5'
$dev = Split-Path (Split-Path $PSScriptRoot)
$sibling = 'esp32' + ($project -replace '^rpi5', '')

# a word for the Pi's shell
function ConvertTo-ShellWord([string]$s) { "'" + $s.Replace("'", "'\''") + "'" }
$options = ($rest | ForEach-Object { ConvertTo-ShellWord $_ }) -join ' '

function Invoke-Pi([string]$script) { ssh -o BatchMode=yes $pi $script }

# the working tree as a commit: an index of its own (a copy of git's, whose
# stat data spares reading the unchanged files), text files with LF line ends
function New-Snapshot([string]$dir) {
    $gitDir = git -C $dir rev-parse --absolute-git-dir
    $index = Join-Path ([IO.Path]::GetTempPath()) "pi-sync-$([guid]::NewGuid()).index"
    if (Test-Path "$gitDir/index") { Copy-Item "$gitDir/index" $index }
    $env:GIT_INDEX_FILE = $index
    try {
        git -C $dir -c core.safecrlf=false add -A
        $tree = git -C $dir write-tree
        $parent = & { $PSNativeCommandUseErrorActionPreference = $false; git -C $dir rev-parse -q --verify HEAD }
        git -C $dir commit-tree $tree @(if ($parent) { '-p', $parent }) -m 'pi.ps1 sync'
    } finally {
        Remove-Item Env:GIT_INDEX_FILE
        Remove-Item $index -ErrorAction SilentlyContinue
    }
}

# the trees into the Pi's ~/Development, then $then there
function Sync-Trees([string]$then) {
    $repos = foreach ($repo in @('rpi5emu', $project, 'esp32emu', $sibling) | Select-Object -Unique) {
        if (Test-Path "$dev/$repo/.git") { $repo }
        elseif (Test-Path "$dev/$repo") { Write-Warning "$repo isn't a git clone: not synced" }
    }
    # the Pi's copies clones first (of GitHub's when there's one); a folder
    # there that isn't one (an earlier rsync) becomes one
    $make = foreach ($repo in $repos) {
        $d = "Development/$repo"
        $url = & { $PSNativeCommandUseErrorActionPreference = $false; git -C "$dev/$repo" remote get-url origin 2>$null }
        $init = "git init -q $d" + ($url ? " && git -C $d remote add origin $(ConvertTo-ShellWord $url)" : '')
        $new = $url ? "if [ -d $d ]; then $init; else git clone -q $(ConvertTo-ShellWord $url) $d; fi" : $init
        "{ [ -d $d/.git ] || { $new; }; }"
    }
    Invoke-Pi ($make -join ' && ')
    foreach ($repo in $repos) {
        $commit = New-Snapshot "$dev/$repo"
        git -C "$dev/$repo" -c core.sshCommand='ssh -o BatchMode=yes' push -q "${pi}:Development/$repo" "+${commit}:refs/pi-sync/head"
    }
    # checked out with an index of its own: started from the Pi's (its stat
    # data refreshed: an rsync from elsewhere changes it), it rewrites only
    # the files that differ from the commit
    $checkout = foreach ($repo in $repos) {
        "(cd ~/Development/$repo && export GIT_INDEX_FILE=`$PWD/.git/pi-sync.index && " +
        "{ [ -f `$GIT_INDEX_FILE ] || [ ! -f .git/index ] || cp .git/index `$GIT_INDEX_FILE; } && " +
        "{ git update-index -q --refresh > /dev/null; true; } && git read-tree --reset -u refs/pi-sync/head)"
    }
    Invoke-Pi ((@($checkout) + @($then | Where-Object { $_ })) -join ' && ')
}

# the X server DISPLAY names: one on this computer started when it doesn't answer
function Test-XServer([string]$address, [int]$port) {
    $tcp = [Net.Sockets.TcpClient]::new()
    try { $tcp.ConnectAsync($address, $port).Wait(300) } catch { $false } finally { $tcp.Dispose() }
}
function Start-XServer {
    if ($env:DISPLAY -notmatch '^(?<host>[^:]*):(?<n>\d+)') { return }
    $number = [int]$Matches.n
    $address = $Matches.host -in '', 'localhost' ? '127.0.0.1' : $Matches.host
    if (Test-XServer $address (6000 + $number)) { return }
    if ($address -ne '127.0.0.1') { throw "no X server at $env:DISPLAY" }
    $vcxsrv = "$env:ProgramFiles\VcXsrv\vcxsrv.exe"
    $moba = "${env:ProgramFiles(x86)}\Mobatek\MobaXterm\MobaXterm.exe"
    if (Test-Path $vcxsrv) { Start-Process $vcxsrv ":$number -multiwindow -clipboard -wgl -ac" }
    elseif (Test-Path $moba) { Start-Process $moba -WindowStyle Minimized }
    else { throw "no X server at $env:DISPLAY (VcXsrv: winget install marha.VcXsrv; or MobaXterm)" }
    foreach ($i in 1..80) {
        if (Test-XServer $address (6000 + $number)) { return }
        Start-Sleep -Milliseconds 250
    }
    throw "no X server at $env:DISPLAY"
}

try {
    switch ($what) {
        'sync' { Sync-Trees }
        'build' {
            Sync-Trees "cd ~/Development/$project && { [ -f build/build.ninja ] || cmake -S . -B build -G Ninja; } && cmake --build build"
        }
        'run' {
            $display = $env:DISPLAY
            try {
                if (-not $env:DISPLAY) { $env:DISPLAY = 'localhost:0.0' }
                Start-XServer
                ssh -C -Y $pi ("cd ~/Development/$project && SDL_RENDER_DRIVER=software SDL_FRAMEBUFFER_ACCELERATION=0 " +
                    "SDL_VIDEO_X11_XINPUT2=0 exec build/$project $options")
            } finally {
                $env:DISPLAY = $display
            }
        }
        'headless' {
            # no window anywhere (SDL's offscreen display), started at once, the
            # log in /tmp/<project>.log there; ready when the console answers
            $start = "cd ~/Development/$project && SDL_VIDEO_DRIVER=offscreen exec nohup build/$project --no-stdin -S $options"
            Invoke-Pi ("($start) > /tmp/$project.log 2>&1 < /dev/null & " +
                'for i in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/6400) 2>/dev/null && exit 0; sleep 0.2; done; ' +
                "echo 'no console'; cat /tmp/$project.log; exit 1")
        }
        'shot' {
            # the machine's picture (the console's s) into the file named here
            $out = $rest[0]
            if (-not $out) { throw 'out.png' }
            & "$PSScriptRoot/console.ps1" "s /tmp/$project-shot.png" > $null
            scp -q "${pi}:/tmp/$project-shot.png" $out
            $out
        }
        'log' { Invoke-Pi "cat /tmp/$project.log" }
        default {
            'pi.ps1 <project> [sync | build | run [options] | headless [options] | shot out.png | log]'
            exit 2
        }
    }
} catch [System.Management.Automation.NativeCommandExitException] {
    exit $_.Exception.ExitCode   # the program said why
}
