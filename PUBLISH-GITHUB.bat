@echo off
title Clipper - publish on GitHub
rem Publishes Clipper 1.2 on your GitHub: the source, the release, the installer (GitHub Pages).
rem Run INSTALL-CLIPPER.bat first (it builds the firmware the release and the installer publish).
rem Usage: PUBLISH-GITHUB.bat                 (the repository YOUR-NAME/clipper-fm1)
rem        PUBLISH-GITHUB.bat -Repo another-name
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\publish_github.ps1" %*
pause
