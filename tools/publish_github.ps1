# Clipper: publish the release on GitHub, from this folder (PUBLISH-GITHUB.bat).
#   1. the source: clipper-1.2.bundle -> the repository OWNER/REPO (created if it does not exist), branch main, the tag
#   2. the release v1.2 (the latest): docs/release-1.2.md, the firmware package (if INSTALL-CLIPPER.bat built it),
#      the demo WAV
#   3. the installer and the editor on GitHub Pages (branch gh-pages, from build\clipper-site)
# Uses your own Git and GitHub CLI (gh) and your own GitHub login. Nothing is ever forced on main: a repository that
# already has another history stops the script. gh-pages is generated: it is replaced each time.
param([string]$Owner = "", [string]$Repo = "clipper-fm1", [string]$Tag = "v1.2")
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path $PSScriptRoot)
function Fail([string]$m) { Write-Host ""; Write-Host "ERROR: $m" -ForegroundColor Red; exit 1 }
function Step([string]$m) { Write-Host "== $m" -ForegroundColor Cyan }

Write-Host ""
Write-Host "  C L I P P E R   1.2   ->   GitHub" -ForegroundColor White
Write-Host "  ---- ---- ---- ----" -ForegroundColor DarkGray
# Git and GitHub CLI: on the PATH, else where their installers put them, else installed now with winget (asked first)
function Find-Tool([string]$t) {
  if (Get-Command $t -ErrorAction SilentlyContinue) { return $true }
  $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
  if (Get-Command $t -ErrorAction SilentlyContinue) { return $true }
  $dirs = if ($t -eq "gh") { @("$env:ProgramFiles\GitHub CLI", "$env:LOCALAPPDATA\Programs\GitHub CLI", "${env:ProgramFiles(x86)}\GitHub CLI") }
          else { @("$env:ProgramFiles\Git\cmd", "$env:LOCALAPPDATA\Programs\Git\cmd", "${env:ProgramFiles(x86)}\Git\cmd") }
  foreach ($d in $dirs) {
    if ($d -and (Test-Path (Join-Path $d "$t.exe"))) { $env:Path = "$d;$env:Path"; return $true }
  }
  return $false
}
$ids = @{ git = "Git.Git"; gh = "GitHub.cli" }
$names = @{ git = "Git (https://git-scm.com)"; gh = "GitHub CLI (https://cli.github.com)" }
foreach ($t in @("git", "gh")) {
  if (Find-Tool $t) { continue }
  if (Get-Command winget -ErrorAction SilentlyContinue) {
    $a = Read-Host "$($names[$t]) is not installed. Install it now with winget? (Y/N)"
    if ($a -match '^[yYoO]') {
      Step "Installing $($names[$t])"
      winget install --id $ids[$t] -e --source winget --accept-package-agreements --accept-source-agreements
      if (Find-Tool $t) { continue }
      Fail "$t is installed but not visible yet: close this window and run PUBLISH-GITHUB.bat again"
    }
  }
  Fail "$t not found. Install $($names[$t]), then run PUBLISH-GITHUB.bat again."
}
gh auth status *> $null
if ($LASTEXITCODE -ne 0) {
  Step "Signing in to GitHub (the browser will open)"
  gh auth login --web --git-protocol https
  if ($LASTEXITCODE -ne 0) { Fail "signing in to GitHub failed" }
}
gh auth setup-git *> $null
if (-not $Owner) { $Owner = "$(gh api user --jq .login)".Trim() }
if (-not $Owner) { Fail "could not read your GitHub user name" }
$full = "$Owner/$Repo"
$pages = "https://$Owner.github.io/$Repo"
$bundle = "clipper-1.2.bundle"
if (-not (Test-Path $bundle)) { Fail "$bundle not found in this folder" }
$work = Join-Path "build" "publish"
$repoDir = Join-Path $work "repo"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null
$email = "$Owner@users.noreply.github.com"

# 1. the source
Step "The source (from $bundle)"
git clone --quiet --branch main $bundle $repoDir
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $repoDir)) { Fail "git clone of the bundle" }
Push-Location $repoDir
gh repo view $full *> $null
if ($LASTEXITCODE -ne 0) {
  Step "Creating the public repository $full"
  gh repo create $full --public --description "Clipper: a synthesizer firmware for the M-VAVE FM-1 (from SLOOP, based on Felucca)" --homepage "$pages/webapp/installer/"
  if ($LASTEXITCODE -ne 0) { Pop-Location; Fail "creating the repository $full" }
}
git remote remove origin *> $null
git remote add origin "https://github.com/$full.git"
Step "Pushing the main branch to $full"
git push -u origin main
if ($LASTEXITCODE -ne 0) {
  Pop-Location
  Fail "git push was refused (the repository $full may already have another history). Nothing was forced. Pick another name: PUBLISH-GITHUB.bat -Repo another-name"
}
if ("$(git ls-remote --tags origin "refs/tags/$Tag")".Trim()) {
  Write-Host "   (the tag $Tag is already on GitHub: kept as it is)" -ForegroundColor DarkGray
} else {
  git -c user.name="$Owner" -c user.email="$email" tag -a $Tag -m "Clipper 1.2"
  git push origin $Tag
  if ($LASTEXITCODE -ne 0) { Pop-Location; Fail "pushing the tag $Tag" }
}
$notes = Join-Path (Resolve-Path "..").Path "release-notes.md"
$src = Join-Path "docs" "release-1.2.md"
if (-not (Test-Path $src)) { Pop-Location; Fail "docs/release-1.2.md is missing from the bundle (an older bundle?)" }
$text = (Get-Content -Raw -Encoding UTF8 $src).Replace("https://isod89.github.io/clipper-fm1", $pages).Replace("isod89/clipper-fm1", $full)
[IO.File]::WriteAllText($notes, $text, (New-Object Text.UTF8Encoding $false))   # (no BOM: GitHub shows the text as it is)
Pop-Location

# 2. the release
$site = Join-Path "build" "clipper-site"
$assets = @()
$fw = Get-ChildItem (Join-Path (Join-Path $site "firmware") "clipper-*.fwsc") -ErrorAction SilentlyContinue | Select-Object -First 1
if ($fw) { $assets += $fw.FullName }
else { Write-Host "(No firmware built: run INSTALL-CLIPPER.bat first. The release is published without the .fwsc; run this script again afterwards to add it.)" -ForegroundColor Yellow }
$ed = Join-Path (Join-Path (Join-Path $site "webapp") "editor") "clipper-editor.html"   # (1.2) the editor in one file, for offline use
if (Test-Path $ed) { $edc = Join-Path $work "clipper-editor-1.2.html"; Copy-Item $ed $edc; $assets += (Resolve-Path $edc).Path }
if (Test-Path "clipper-1.0-demo.wav") { $assets += (Resolve-Path "clipper-1.0-demo.wav").Path }
gh release view $Tag --repo $full *> $null
if ($LASTEXITCODE -eq 0) {
  Step "The release $Tag exists: updating its notes and files"
  gh release edit $Tag --repo $full --prerelease=false --latest --title "Clipper 1.2" --notes-file $notes
  if ($assets.Count -gt 0) { gh release upload $Tag @assets --repo $full --clobber }
} else {
  Step "Creating the release $Tag"
  gh release create $Tag @assets --repo $full --latest --title "Clipper 1.2" --notes-file $notes
}
if ($LASTEXITCODE -ne 0) { Fail "the release $Tag" }

# 3. GitHub Pages: the installer and the editor
if (Test-Path (Join-Path (Join-Path (Join-Path $site "webapp") "installer") "index.html")) {
  Step "The installer and the editor on GitHub Pages"
  $pg = Join-Path $work "pages"
  New-Item -ItemType Directory -Force $pg | Out-Null
  Copy-Item -Recurse (Join-Path $site "*") $pg
  New-Item -ItemType File (Join-Path $pg ".nojekyll") | Out-Null
  Push-Location $pg
  git init --quiet
  git checkout --quiet -b gh-pages
  git add -A
  git -c user.name="$Owner" -c user.email="$email" commit --quiet -m "Clipper 1.2: the installer and the editor"
  git push --force "https://github.com/$full.git" gh-pages
  $ok = $LASTEXITCODE -eq 0
  Pop-Location
  if (-not $ok) { Fail "pushing the gh-pages branch" }
  gh api -X POST "repos/$full/pages" -f "source[branch]=gh-pages" -f "source[path]=/" *> $null   # (already on: refused, fine)
  Write-Host "   (the first time, GitHub takes a minute or two to publish the page)" -ForegroundColor DarkGray
} else {
  Write-Host "(No build\clipper-site: run INSTALL-CLIPPER.bat first to publish the installer on GitHub Pages.)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Repository:  https://github.com/$full" -ForegroundColor Green
Write-Host "Release:     https://github.com/$full/releases/tag/$Tag" -ForegroundColor Green
Write-Host "Installer:   $pages/webapp/installer/" -ForegroundColor Green
Write-Host "Editor:      $pages/webapp/editor/" -ForegroundColor Green
