@echo off
setlocal EnableDelayedExpansion
rem ============================================================
rem  COATHEAL Ground Station launcher - double-click to run.
rem
rem  First run:  creates a local Python environment, installs the
rem              dependencies, and offers to open the Windows
rem              firewall for onboard auto-discovery (one UAC
rem              prompt). Takes a few minutes.
rem  After that: launches instantly.
rem
rem  Diagnostics:  COATHEAL-GroundStation.bat --check
rem                (verifies the environment without opening the GUI)
rem ============================================================

cd /d "%~dp0"
set "VENV=.venv"
set "REQ=requirements.txt"
set "REQ_MARKER=%VENV%\.requirements.sha"
set "FW_MARKER=%VENV%\.firewall.done"

rem ---- 1. Find Python ---------------------------------------------------
set "PYTHON="
py -3 -c "import sys" >nul 2>&1 && set "PYTHON=py -3"
if not defined PYTHON (
  python -c "import sys" >nul 2>&1 && set "PYTHON=python"
)
if not defined PYTHON (
  echo.
  echo   Python 3 was not found on this computer.
  echo.
  choice /C YN /M "  Install Python 3 automatically now (winget)"
  if errorlevel 2 (
    echo   Install Python 3 from https://www.python.org/downloads/
    echo   ^(tick "Add python.exe to PATH" in the installer^), then run this again.
    goto :fail
  )
  winget install -e --id Python.Python.3.12 --accept-package-agreements --accept-source-agreements
  if errorlevel 1 (
    echo   Automatic install failed. Install Python 3 from python.org and re-run.
    goto :fail
  )
  echo   Python installed. Please CLOSE this window and double-click the launcher again
  echo   ^(a fresh window is needed so the new Python is on PATH^).
  goto :fail
)

rem ---- 2. Create the local environment on first run ---------------------
if not exist "%VENV%\Scripts\python.exe" (
  echo ==^> First run: creating the Python environment...
  %PYTHON% -m venv "%VENV%"
  if errorlevel 1 (
    echo   Could not create the Python environment.
    goto :fail
  )
)
set "VPY=%VENV%\Scripts\python.exe"

rem ---- 3. Install dependencies only when requirements changed -----------
set "REQ_HASH="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%REQ%" SHA256 2^>nul') do (
  if not defined REQ_HASH set "REQ_HASH=%%H"
)
set "OLD_HASH="
if exist "%REQ_MARKER%" set /p OLD_HASH=<"%REQ_MARKER%"
if not "%REQ_HASH%"=="%OLD_HASH%" (
  echo ==^> Installing dependencies ^(first run or requirements changed^)...
  "%VPY%" -m pip install --upgrade pip --quiet
  "%VPY%" -m pip install -r "%REQ%"
  if errorlevel 1 (
    echo   Dependency installation failed. Check your internet connection and re-run.
    goto :fail
  )
  >"%REQ_MARKER%" echo %REQ_HASH%
)

rem ---- 4. Diagnostics mode (skips the firewall prompt) -------------------
if /I "%~1"=="--check" (
  echo ==^> Environment check...
  "%VPY%" -c "import PyQt6, pyqtgraph, numpy; print('  Python environment OK')"
  if errorlevel 1 goto :fail
  echo   All good. Double-click the launcher without --check to start the GUI.
  goto :eof
)

rem ---- 5. First-run firewall setup (needed for auto-discovery) ----------
if not exist "%FW_MARKER%" (
  echo.
  echo   To auto-discover the onboard, Windows Firewall must allow
  echo   COATHEAL's telemetry ^(TCP 4000^) and discovery ^(UDP 4100^).
  choice /C YN /M "  Configure the firewall now (one administrator prompt)"
  if not errorlevel 2 (
    powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0scripts\configure_firewall.ps1'"
    if errorlevel 1 (
      echo   Firewall setup was cancelled or failed - discovery may not work.
      echo   You can re-run it later: right-click scripts\configure_firewall.ps1 ^> Run with PowerShell as admin.
    ) else (
      >"%FW_MARKER%" echo done
    )
  ) else (
    >"%FW_MARKER%" echo skipped
    echo   Skipped. If the onboard is never discovered, run scripts\configure_firewall.ps1 as admin.
  )
)

rem ---- 6. Launch ---------------------------------------------------------
echo ==^> Starting COATHEAL Ground Station...
"%VPY%" gui_app.py %*
if errorlevel 1 (
  echo.
  echo   The ground station exited with an error ^(see messages above^).
  goto :fail
)
goto :eof

:fail
echo.
pause
exit /b 1
