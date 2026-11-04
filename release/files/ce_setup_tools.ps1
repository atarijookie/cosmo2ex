# Sets up the programs CosmosEx's helpers need, by asking simple yes / no questions - Windows version of
# ce_setup_tools.sh. Start it with ce_setup_tools.bat (double-click), it's in the CosmosEx folder.
#   - ffmpeg  - for the video and audio players (CE_VIDEO.TTP, CE_AUDIO.TTP), used by ce_media.exe
#               downloaded from gyan.dev (the build ffmpeg.org links for Windows), or ffmpeg in Docker Desktop
#   - Chrome  - headless, for the web browser (CE_BROWS.PRG), used by ce_browser.exe
#               Chrome for Testing's chrome-headless-shell from Google
#   - Freedoom - the game data for Doom (CE_DOOM.PRG), played by ce_doomgeneric.exe (comes with CosmosEx);
#               from its GitHub release, checked against its SHA-256 - freedoom1.wad to tools\
# The config drive shows these Atari programs only when what they need is here.
# Both are unpacked to the tools folder next to this script, their paths go to .env (MEDIA_FFMPEG, CHROME_PATH).
# It can be run again any time - it skips what's working. Enter takes the answer in capitals ([Y/n]: yes).

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'            # Invoke-WebRequest is very slow with its progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$Dir      = $PSScriptRoot
$EnvFile  = Join-Path $Dir '.env'
$ToolsDir = Join-Path $Dir 'tools'

$FfmpegUrl    = 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip'
$FfmpegImage  = 'linuxserver/ffmpeg'
$CftVersion   = 'https://googlechromelabs.github.io/chrome-for-testing/LATEST_RELEASE_STABLE'
$CftDownload  = 'https://storage.googleapis.com/chrome-for-testing-public'
$FreedoomUrl    = 'https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip'
$FreedoomSha256 = '3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59'

#-------------
# helpers

function Ok($s)   { Write-Host "  OK: $s" }
function Warn($s) { Write-Host "  !! $s" -ForegroundColor Yellow }

function Ask($question, $default) {
    $hint = if ($default) { '[Y/n]' } else { '[y/N]' }
    while ($true) {
        $a = (Read-Host "$question $hint").Trim().ToLower()
        if ($a -eq '')                       { return $default }
        if ($a -eq 'y' -or $a -eq 'yes')     { return $true }
        if ($a -eq 'n' -or $a -eq 'no')      { return $false }
    }
}

function Read-EnvLines { return [IO.File]::ReadAllText($EnvFile) -split "`r?`n" }

function Write-EnvLines($lines) {
    while ($lines.Count -gt 0 -and $lines[-1] -eq '') { $lines = $lines[0..($lines.Count - 2)] }
    # LF and no BOM, like the .env in the zip - it's read by ce_cores (Cygwin) and the webserver
    [IO.File]::WriteAllText($EnvFile, (($lines -join "`n") + "`n"), (New-Object Text.UTF8Encoding $false))
}

# value of KEY in .env (only uncommented lines)
function Get-EnvValue($key) {
    $v = ''
    foreach ($l in Read-EnvLines) {
        if ($l -match "^$key=(.*)$") { $v = $Matches[1].Trim().Trim('"') }
    }
    return $v
}

# KEY=VALUE in .env: replaces the KEY= line, else the first #KEY= line, else adds it at the end
function Set-EnvValue($key, $value) {
    $value = $value -replace '\\', '/'                # C:/... - no escaping questions, ce_cores takes both
    if ($value -match ' ') { $value = '"' + $value + '"' }
    $lines = [Collections.ArrayList]@(Read-EnvLines)
    $at = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match "^$key=") { $at = $i; break } }
    if ($at -lt 0) {
        for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match "^#$key=") { $at = $i; break } }
    }
    if ($at -ge 0) { $lines[$at] = "$key=$value" } else { [void]$lines.Add("$key=$value") }
    Write-EnvLines $lines
    Ok "$key=$value in .env"
}

# KEY in .env commented out again
function Clear-EnvValue($key) {
    $lines = @(Read-EnvLines | ForEach-Object { if ($_ -match "^$key=") { "#$_" } else { $_ } })
    Write-EnvLines $lines
}

function Download($url, $file) {
    Write-Host "  downloading $url"
    Write-Host '  (this can take a few minutes)'
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue      # Windows 10 1803+: shows progress, faster
    if ($curl) {
        & $curl.Source -fL --retry 3 -o $file $url
        if ($LASTEXITCODE -ne 0) { throw "download failed: $url" }
    } else {
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $file
    }
}

function Have($cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

#-------------
Write-Host 'CosmosEx tools setup'
Write-Host '===================='
Write-Host 'The video / audio players and the web browser need programs which are not part of CosmosEx.'
Write-Host 'This asks before downloading anything. Enter = the answer in capitals.'
Write-Host ''

if (-not (Test-Path $EnvFile)) {
    Warn "No .env in $Dir - run this from the CosmosEx folder."
    exit 1
}
$changed = $false

#-------------
# ffmpeg

function Test-Ffmpeg {
    $img = Get-EnvValue 'MEDIA_DOCKER_IMAGE'
    if ($img) {
        if (Have 'docker') { Ok "ffmpeg: Docker image $img"; return $true }
        Warn "MEDIA_DOCKER_IMAGE=$img is set in .env, but Docker is not installed"
        return $false
    }
    $f = Get-EnvValue 'MEDIA_FFMPEG'
    if ($f) {
        if (Test-Path $f) { Ok "ffmpeg: $f"; return $true }
        Warn "MEDIA_FFMPEG=$f is set in .env, but it's not there"
        return $false
    }
    if ((Have 'ffmpeg.exe') -and (Have 'ffprobe.exe')) { Ok "ffmpeg: $((Get-Command ffmpeg.exe).Source)"; return $true }
    return $false
}

function Install-Ffmpeg {
    if (-not (Ask "  Download ffmpeg (about 110 MB, from gyan.dev) to $ToolsDir?" $true)) { return $false }
    New-Item -ItemType Directory -Force -Path $ToolsDir | Out-Null
    $zip = Join-Path $ToolsDir 'ffmpeg.zip'
    Download $FfmpegUrl $zip
    Get-ChildItem $ToolsDir -Directory -Filter 'ffmpeg-*' | Remove-Item -Recurse -Force
    Expand-Archive -Path $zip -DestinationPath $ToolsDir -Force
    Remove-Item $zip
    $exe = Get-ChildItem $ToolsDir -Recurse -Filter 'ffmpeg.exe' | Select-Object -First 1
    $probe = Get-ChildItem $ToolsDir -Recurse -Filter 'ffprobe.exe' | Select-Object -First 1
    if (-not $exe -or -not $probe) { Warn 'ffmpeg.exe / ffprobe.exe not in the download'; return $false }
    Clear-EnvValue 'MEDIA_DOCKER_IMAGE'
    Set-EnvValue 'MEDIA_FFMPEG' $exe.FullName
    Set-EnvValue 'MEDIA_FFPROBE' $probe.FullName
    return $true
}

function Use-FfmpegDocker {
    if (-not (Have 'docker')) { return $false }                     # Docker Desktop is not installed by this script
    if (-not (Ask "  Use ffmpeg in Docker Desktop instead (image $FfmpegImage, about 1 GB)?" $false)) { return $false }
    Clear-EnvValue 'MEDIA_FFMPEG'
    Clear-EnvValue 'MEDIA_FFPROBE'
    Set-EnvValue 'MEDIA_DOCKER_IMAGE' $FfmpegImage
    if (Ask '  Download the image now? (else when a player starts the first time)' $true) {
        & docker pull $FfmpegImage | Out-Host       # not into the return value of this function
        if ($LASTEXITCODE -ne 0) { Warn "download failed - is Docker Desktop running? It's tried again when a player starts." }
    }
    return $true
}

Write-Host '1) ffmpeg - for CE_VIDEO.TTP and CE_AUDIO.TTP'
if (-not (Test-Ffmpeg)) {
    $done = $false
    try { $done = (Install-Ffmpeg) -or (Use-FfmpegDocker) } catch { Warn $_.Exception.Message }
    if ($done) { $changed = $true; Ok 'ffmpeg is ready' }
    else { Warn "no ffmpeg - the video and audio players won't work (run this again to install it)" }
}
Write-Host ''

#-------------
# Chrome

function Test-Chrome {
    $c = Get-EnvValue 'CHROME_PATH'
    if ($c) {
        if (Test-Path $c) { Ok "Chrome: $c"; return $true }
        Warn "CHROME_PATH=$c is set in .env, but it's not there"
    }
    return $false
}

function Install-Chrome {
    if (-not (Ask "  Download Chrome headless shell from Google (about 120 MB) to $ToolsDir?" $true)) { return $false }
    New-Item -ItemType Directory -Force -Path $ToolsDir | Out-Null
    $ver = (Invoke-WebRequest -UseBasicParsing -Uri $CftVersion).Content.Trim()
    $zip = Join-Path $ToolsDir 'chrome-headless-shell.zip'
    Download "$CftDownload/$ver/win64/chrome-headless-shell-win64.zip" $zip
    $target = Join-Path $ToolsDir 'chrome-headless-shell-win64'
    if (Test-Path $target) { Remove-Item $target -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $ToolsDir -Force
    Remove-Item $zip
    $exe = Join-Path $target 'chrome-headless-shell.exe'
    if (-not (Test-Path $exe)) { Warn 'chrome-headless-shell.exe not in the download'; return $false }
    Ok "Chrome $ver in $ToolsDir"
    Set-EnvValue 'CHROME_PATH' $exe
    return $true
}

Write-Host '2) Chrome (headless) - for CE_BROWS.PRG'
if (-not (Test-Chrome)) {
    $done = $false
    try { $done = Install-Chrome } catch { Warn $_.Exception.Message }
    if ($done) { $changed = $true }
    else { Warn "no Chrome - the web browser won't work (run this again to install it)" }
}
Write-Host ''

#-------------
# Doom: the engine comes with CosmosEx, the game data (a WAD) is downloaded

function Get-DoomWad {
    $w = Get-EnvValue 'MEDIA_DOOM_WAD'
    if ($w) { if (Test-Path $w) { return $w } else { return '' } }
    foreach ($n in 'doom.wad', 'doom1.wad', 'freedoom1.wad') {
        $p = Join-Path $ToolsDir $n
        if (Test-Path $p) { return $p }
    }
    return ''
}

function Install-Freedoom {
    if (-not (Ask "  Download Freedoom (a free Doom game, about 25 MB) to $ToolsDir?" $true)) { return $false }
    New-Item -ItemType Directory -Force -Path $ToolsDir | Out-Null
    $zip = Join-Path $ToolsDir 'freedoom.zip'
    Download $FreedoomUrl $zip
    if ((Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower() -ne $FreedoomSha256) {
        Remove-Item $zip
        Warn "the download doesn't match its checksum - not used"
        return $false
    }
    $tmp = Join-Path $ToolsDir 'freedoom-unpack'
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $tmp -Force
    Remove-Item $zip
    $wad = Get-ChildItem $tmp -Recurse -Filter 'freedoom1.wad' | Select-Object -First 1
    if (-not $wad) { Remove-Item $tmp -Recurse -Force; Warn 'freedoom1.wad not in the download'; return $false }
    Move-Item -Force $wad.FullName (Join-Path $ToolsDir 'freedoom1.wad')
    Remove-Item $tmp -Recurse -Force
    Ok "Freedoom in $(Join-Path $ToolsDir 'freedoom1.wad')"
    return $true
}

Write-Host '3) Doom - for CE_DOOM.PRG'
if (-not (Test-Path (Join-Path $Dir 'ce_doomgeneric.exe')) -and -not (Get-EnvValue 'MEDIA_DOOM_BIN')) {
    Warn 'the Doom engine (ce_doomgeneric.exe) is not in this CosmosEx folder - update CosmosEx'
} elseif (Get-DoomWad) {
    Ok "Doom: $(Get-DoomWad)"
} else {
    $done = $false
    try { $done = Install-Freedoom } catch { Warn $_.Exception.Message }
    if ($done) { Write-Host '  CE_DOOM.PRG shows up on the config drive within a minute.' }
    else { Warn 'no Doom WAD - CE_DOOM.PRG stays hidden (run this again to download Freedoom)' }
}
Write-Host ''

if ($changed) { Write-Host 'Restart CosmosEx (ce_run.exe), so it uses them.' }
Write-Host 'Done.'
