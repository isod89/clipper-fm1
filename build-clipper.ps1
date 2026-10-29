# Clipper 1.3: build the firmware, then open the browser installer.
# Uses the same tools as SLOOP (WSL MinUI-Build, the JieLi toolchain) and the three AC79 SDK files in
# build\deps\ac79 (copied from the SLOOP folder the first time if they are not here yet).
# Port 8767 (SLOOP uses 8766): a SLOOP window left open does not get mixed up with this one.
param([string]$Voices = "8")
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
Write-Host ""
Write-Host "  C L I P P E R   1.3   ($Voices voices)" -ForegroundColor White
Write-Host "  ---- ---- ---- ----" -ForegroundColor DarkGray
if ($Voices -notin @("8", "12", "16")) { throw "Voices: 8, 12 or 16" }
$sdk = "build\deps\ac79"
if (-not (Test-Path "$sdk\cpu\wl82\tools\uboot.boot")) {
  $from = @($env:CLIPPER_SDK, "..\sloop-fm1\build\deps\ac79", "..\SLOOP\build\deps\ac79", "..\sloop\build\deps\ac79") |
    Where-Object { $_ -and (Test-Path "$_\cpu\wl82\tools\uboot.boot") } | Select-Object -First 1
  if (-not $from) { throw "No AC79 SDK files: copy build\deps\ac79 from your SLOOP folder into this folder (or set CLIPPER_SDK)" }
  Write-Host "== Copying the SDK files from $from" -ForegroundColor Cyan
  New-Item -ItemType Directory -Force -Path "build\deps" | Out-Null
  Copy-Item -Recurse -Force $from "build\deps\"
  # the build caches too (as SLOOP's PREPARER does): the generated samples are not made again
  $sloop = Split-Path (Split-Path (Split-Path $from))
  foreach ($c in @("genwav", "hiphop-src")) {
    if ((Test-Path "$sloop\build\$c") -and -not (Test-Path "build\$c")) { Copy-Item -Recurse "$sloop\build\$c" "build\" }
  }
  if ((Test-Path "$sloop\build\gen_samples.cache") -and -not (Test-Path "build\gen_samples.cache")) { Copy-Item "$sloop\build\gen_samples.cache" "build\" }
}
Write-Host "== Building the firmware (WSL MinUI-Build)" -ForegroundColor Cyan
Remove-Item Env:FELUCCA_SLICE -ErrorAction SilentlyContinue
python tools/build_windows.py --distro MinUI-Build --toolchain /root/fm1-codex-deps/jieli/toolchain --sdk $sdk --slice 0 --nvoice $Voices
if ($LASTEXITCODE -ne 0) { throw "The build failed" }
Write-Host "== Making the installer site" -ForegroundColor Cyan
python web/make_site.py build/felucca.fwsc 1.3 build/clipper-site
if ($LASTEXITCODE -ne 0) { throw "make_site failed" }
Write-Host ""
Write-Host "Installer: http://localhost:8767/webapp/installer/  (Chrome or Edge, FM-1 on USB)" -ForegroundColor Green
Write-Host "Editor:    http://localhost:8767/webapp/editor/" -ForegroundColor Green
Write-Host "Keep this window open during the install. Ctrl+C stops the server."
Start-Process "http://localhost:8767/webapp/installer/"
python -m http.server 8767 --bind 127.0.0.1 --directory build/clipper-site
