# Installs brine on Windows (PowerShell 5.1 or later):
#
#   irm https://raw.githubusercontent.com/brinelang/brine/main/install.ps1 | iex
#
# Settings (environment variables):
#   $env:BRINE_VERSION = '0.4.0'          a version (default: the latest release)
#   $env:BRINE_INSTALL = "$HOME\.brine"   where to install (the binary goes in its bin folder)
#   $env:BRINE_NO_MODIFY_PATH = '1'       don't add brine to your user PATH
#   $env:BRINE_DOWNLOAD_URL = '...'       where the release files are (default: GitHub releases; for mirrors)
#
# To uninstall, delete $HOME\.brine and remove its bin folder from your user PATH.

# Nothing runs until the last line calls Install-Brine, so a download cut short does nothing.
function Install-Brine {
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest is much faster without it
    # PowerShell 5.1 may default to TLS 1.0, which GitHub refuses
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

    $repo = 'brinelang/brine'
    $installDir = if ($env:BRINE_INSTALL) { $env:BRINE_INSTALL } else { Join-Path $HOME '.brine' }
    $binDir = Join-Path $installDir 'bin'

    $cpu = $env:PROCESSOR_ARCHITECTURE
    if ($env:PROCESSOR_ARCHITEW6432) { $cpu = $env:PROCESSOR_ARCHITEW6432 }
    switch ($cpu) {
        'AMD64' { $arch = 'x64' }
        # Windows on ARM runs the x64 build through its emulator
        'ARM64' { $arch = 'x64'; Write-Host 'brine: no ARM64 Windows build yet; installing the x64 one, which runs under emulation' }
        default { throw "brine has no build for $cpu CPUs; build it from source: https://github.com/$repo" }
    }

    $name = "brine-windows-$arch"
    $version = "$env:BRINE_VERSION".TrimStart('v')
    if ($env:BRINE_DOWNLOAD_URL) {
        $base = $env:BRINE_DOWNLOAD_URL.TrimEnd('/')
    } elseif ($version) {
        $base = "https://github.com/$repo/releases/download/v$version"
    } else {
        $base = "https://github.com/$repo/releases/latest/download"
    }

    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("brine-install-" + [Guid]::NewGuid())
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        $label = if ($version) { $version } else { '(latest)' }
        Write-Host "brine: downloading $name $label"
        $zip = Join-Path $tmp "$name.zip"
        $sums = Join-Path $tmp 'SHA256SUMS'
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "$base/$name.zip" -OutFile $zip
            Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS" -OutFile $sums
        } catch {
            throw "download failed from $base ($($_.Exception.Message))"
        }

        $want = $null
        foreach ($line in Get-Content $sums) {
            $parts = $line -split '\s+'
            if ($parts.Count -ge 2 -and $parts[1].TrimStart('*') -eq "$name.zip") { $want = $parts[0].ToLower() }
        }
        if (-not $want) { throw "SHA256SUMS has no entry for $name.zip" }
        $got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
        if ($got -ne $want) { throw "checksum mismatch for $name.zip (expected $want, got $got)" }

        Expand-Archive -Path $zip -DestinationPath $tmp -Force
        New-Item -ItemType Directory -Force -Path $binDir | Out-Null
        $exe = Join-Path $binDir 'brine.exe'
        # A running brine.exe can't be overwritten, but it can be renamed out of the way.
        if (Test-Path $exe) {
            $old = "$exe.old"
            Remove-Item -Force $old -ErrorAction SilentlyContinue
            Move-Item -Force $exe $old
        }
        Copy-Item (Join-Path $tmp "$name\brine.exe") $exe
        Remove-Item -Force "$exe.old" -ErrorAction SilentlyContinue
        Write-Host "brine: installed $(& $exe --version) to $exe"
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }

    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $onPath = ($userPath -split ';') -contains $binDir
    if (-not $onPath) {
        if ($env:BRINE_NO_MODIFY_PATH) {
            Write-Host "brine: add $binDir to your PATH to use brine"
        } else {
            $newPath = if ($userPath) { "$binDir;$userPath" } else { $binDir }
            [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
            Write-Host "brine: added $binDir to your user PATH (new terminals will see it)"
        }
    }
    if (-not (($env:Path -split ';') -contains $binDir)) { $env:Path = "$binDir;$env:Path" }

    if (-not (Get-Command gcc, clang, cc -ErrorAction SilentlyContinue)) {
        Write-Warning ('brine needs a C compiler to build programs, and none was found; install one with ' +
            '`winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT` or `scoop install mingw-winlibs`')
    }
    Write-Host "brine: run 'brine doctor' to check your setup, and 'brine new hello' to start a project"
}

Install-Brine
