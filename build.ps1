param(
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,

    [switch]$NoIncrement,
    [string]$GitHubOwner,
    [string]$GitHubRepo,
    [switch]$Upx
)

$ErrorActionPreference = "Stop"

function Get-WindowsCMake {
    $candidates = @(
        "$env:ProgramFiles\CMake\bin\cmake.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "cmake"
    )

    foreach ($candidate in $candidates) {
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }

    throw "cmake was not found."
}

function Get-CMakeVersion {
    $cmakePath = Join-Path $PSScriptRoot "CMakeLists.txt"
    $content = Get-Content $cmakePath -Raw
    if ($content -notmatch 'set\(RESIZERC_VERSION\s+"(\d+)\.(\d+)\.(\d+)"') {
        throw "RESIZERC_VERSION not found in $cmakePath"
    }

    [version]"$($matches[1]).$($matches[2]).$($matches[3])"
}

function Set-ProjectVersion {
    param([string]$NewVersion)

    $cmakePath = Join-Path $PSScriptRoot "CMakeLists.txt"
    $content = Get-Content $cmakePath -Raw
    $content = $content -replace 'set\(RESIZERC_VERSION\s+"[^"]+"', "set(RESIZERC_VERSION `"$NewVersion`""
    Set-Content $cmakePath -Value $content -NoNewline -Encoding ascii

    $vcpkgPath = Join-Path $PSScriptRoot "vcpkg.json"
    if (Test-Path $vcpkgPath) {
        $content = Get-Content $vcpkgPath -Raw
        $content = $content -replace '"version-string"\s*:\s*"[^"]+"', "`"version-string`": `"$NewVersion`""
        Set-Content $vcpkgPath -Value $content -NoNewline -Encoding ascii
    }
}

function Get-GitHubRemote {
    $origin = (& git remote get-url origin 2>$null)
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($origin)) {
        return $null
    }

    $origin = $origin.Trim()
    if ($origin -match 'github\.com[:/](?<owner>[^/]+)/(?<repo>[^/]+?)(?:\.git)?$') {
        return @{
            Owner = $matches.owner
            Repo = $matches.repo
        }
    }

    return $null
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $current = Get-CMakeVersion
    if ($NoIncrement) {
        $Version = $current.ToString()
    }
    else {
        $Version = "$($current.Major).$($current.Minor).$($current.Build + 1)"
    }
}

$remote = Get-GitHubRemote
if ([string]::IsNullOrWhiteSpace($GitHubOwner)) {
    $GitHubOwner = if ($remote) { $remote.Owner } else { "OWNER" }
}
if ([string]::IsNullOrWhiteSpace($GitHubRepo)) {
    $GitHubRepo = if ($remote) { $remote.Repo } else { "ResizerC" }
}

Set-ProjectVersion $Version

$cmake = Get-WindowsCMake
$buildDir = Join-Path $PSScriptRoot "build"
$artifactDir = Join-Path $PSScriptRoot "artifacts"
$artifactExe = Join-Path $artifactDir "ResizerC.exe"
$artifactSha = Join-Path $artifactDir "ResizerC.exe.sha256"

$configureArgs = @("-S", $PSScriptRoot, "-B", $buildDir, "-G", "Visual Studio 17 2022")
$cachePath = Join-Path $buildDir "CMakeCache.txt"
if (Test-Path $cachePath) {
    $cache = Get-Content $cachePath -Raw
    if ($cache -notmatch 'CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022') {
        throw "Existing build directory uses a different CMake generator. Delete build/ and retry."
    }
    if ($cache -match 'CMAKE_GENERATOR_PLATFORM:INTERNAL=(.+)' -and ![string]::IsNullOrWhiteSpace($matches[1])) {
        $configureArgs += @("-A", $matches[1].Trim())
    }
}
else {
    $configureArgs += @("-A", "x64")
}

$configureArgs += @(
    "-DRESIZERC_VERSION=$Version",
    "-DRESIZERC_GITHUB_OWNER=$GitHubOwner",
    "-DRESIZERC_GITHUB_REPO=$GitHubRepo"
)

$toolchain = Join-Path $PSScriptRoot "vcpkg\scripts\buildsystems\vcpkg.cmake"
if (Test-Path $toolchain) {
    $configureArgs += @(
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
        "-DVCPKG_TARGET_TRIPLET=x64-windows-static"
    )
}

& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

& $cmake --build $buildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$builtExe = Join-Path $buildDir "bin\Release\ResizerC.exe"
if (!(Test-Path $builtExe)) {
    throw "Build did not create $builtExe"
}

New-Item -ItemType Directory -Path $artifactDir -Force | Out-Null
Copy-Item $builtExe $artifactExe -Force

if ($Upx) {
    $upxPath = Join-Path $PSScriptRoot "upx.exe"
    if (!(Test-Path $upxPath)) {
        throw "upx.exe not found."
    }
    & $upxPath -3 $artifactExe
    if ($LASTEXITCODE -ne 0) { throw "UPX failed." }
}

Copy-Item $artifactExe (Join-Path $PSScriptRoot "ResizerC.exe") -Force

$hash = (Get-FileHash $artifactExe -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  ResizerC.exe" | Set-Content $artifactSha -Encoding ascii

Write-Host "Built $artifactExe ($Version, $GitHubOwner/$GitHubRepo)" -ForegroundColor Green
