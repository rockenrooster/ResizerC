param(
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,

    [string]$Message,
    [switch]$NoUpx,
    [switch]$Upx
)

$ErrorActionPreference = "Stop"

function RunGit {
    & git @args
    if ($LASTEXITCODE -ne 0) {
        throw "git $($args -join ' ') failed"
    }
}

function Get-CMakeVersionFromContent {
    param([string]$Content, [string]$Source)

    if ($Content -notmatch 'set\(RESIZERC_VERSION\s+"(\d+)\.(\d+)\.(\d+)"') {
        throw "RESIZERC_VERSION not found in $Source"
    }

    [version]"$($matches[1]).$($matches[2]).$($matches[3])"
}

function Get-CurrentVersion {
    $cmakePath = Join-Path $PSScriptRoot "CMakeLists.txt"
    Get-CMakeVersionFromContent (Get-Content $cmakePath -Raw) $cmakePath
}

function Get-DefaultReleaseVersion {
    $current = Get-CurrentVersion
    $headContent = git show HEAD:CMakeLists.txt 2>$null
    if ($LASTEXITCODE -eq 0 -and $headContent) {
        $headVersion = Get-CMakeVersionFromContent ($headContent -join "`n") "HEAD:CMakeLists.txt"
        if ($current -gt $headVersion) {
            return $current.ToString()
        }
    }

    "$($current.Major).$($current.Minor).$($current.Build + 1)"
}

function Get-GitHubRemote {
    $origin = git remote get-url origin 2>$null
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($origin)) {
        throw "No git remote named origin is configured."
    }
    $origin = ($origin -join "`n").Trim()

    if ($origin -notmatch 'github\.com[:/](?<owner>[^/]+)/(?<repo>[^/]+?)(?:\.git)?$') {
        throw "origin is not a GitHub remote: $origin"
    }

    @{
        Owner = $matches.owner
        Repo = $matches.repo
        Url = $origin
    }
}

function Get-GeneratedCommitBody {
    $lines = git diff --cached --name-status
    if ($LASTEXITCODE -ne 0) {
        throw "Could not inspect staged changes."
    }

    if (!$lines) {
        return "Automated release."
    }

    $items = foreach ($line in $lines) {
        $parts = $line -split "`t"
        $status = $parts[0]
        $path = $parts[-1]
        $verb = switch -Regex ($status) {
            '^A' { "Added"; break }
            '^D' { "Removed"; break }
            '^R' { "Renamed"; break }
            default { "Updated" }
        }
        "- $verb $path"
    }

    "Changes:`n" + ($items -join "`n")
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = Get-DefaultReleaseVersion
}

$tag = "v$Version"
$remote = Get-GitHubRemote
Write-Host "Releasing $tag to $($remote.Owner)/$($remote.Repo)" -ForegroundColor Cyan

$branch = (git branch --show-current).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($branch)) {
    throw "Could not determine the current branch."
}

$null = git rev-parse -q --verify "refs/tags/$tag" 2>$null
if ($LASTEXITCODE -eq 0) {
    throw "Local tag $tag already exists."
}

$remoteTag = git ls-remote --tags origin "refs/tags/$tag"
if ($LASTEXITCODE -ne 0) {
    throw "Could not check remote tags."
}
if (![string]::IsNullOrWhiteSpace($remoteTag)) {
    throw "Remote tag $tag already exists."
}

$buildArgs = @{
    Version = $Version
    NoIncrement = $true
    GitHubOwner = $remote.Owner
    GitHubRepo = $remote.Repo
}
if ($Upx) {
    $buildArgs.Upx = $true
}
if ($NoUpx) {
    $buildArgs.NoUpx = $true
}
& (Join-Path $PSScriptRoot "build.ps1") @buildArgs

$artifactExe = Join-Path $PSScriptRoot "artifacts\ResizerC.exe"
$artifactSha = Join-Path $PSScriptRoot "artifacts\ResizerC.exe.sha256"
if (!(Test-Path $artifactExe) -or !(Test-Path $artifactSha)) {
    throw "Build did not create the release artifacts."
}

RunGit add -A

git diff --cached --quiet
if ($LASTEXITCODE -eq 1) {
    if ([string]::IsNullOrWhiteSpace($Message)) {
        RunGit commit -m "Release $tag" -m (Get-GeneratedCommitBody)
    }
    else {
        RunGit commit -m $Message
    }
}
elseif ($LASTEXITCODE -ne 0) {
    throw "Could not inspect staged changes."
}

RunGit push -u origin $branch
RunGit tag $tag
RunGit push origin $tag

Write-Host "Pushed $branch and $tag. GitHub Actions will create the release." -ForegroundColor Green
