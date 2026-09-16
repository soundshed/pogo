@echo off
rem Build the firmware and the desktop app: runs build.py with the first Python
rem 3.7 or later found, passing the arguments on.
rem
rem   build              host tests, firmware, app
rem   build firmware     host tests, firmware
rem   build app          app only
rem   build --help       the options
setlocal

set PY=
py -3 -c "import sys; sys.exit(sys.version_info < (3, 7))" >nul 2>&1 && set PY=py -3
if not defined PY python -c "import sys; sys.exit(sys.version_info < (3, 7))" >nul 2>&1 && set PY=python
if not defined PY "%LOCALAPPDATA%\Microsoft\WindowsApps\python.exe" -c "import sys; sys.exit(sys.version_info < (3, 7))" >nul 2>&1 && set PY="%LOCALAPPDATA%\Microsoft\WindowsApps\python.exe"
if not defined PY (
    echo No Python 3.7 or later found. Install one from python.org or with the Python install manager.
    exit /b 1
)

%PY% "%~dp0build.py" %*
exit /b %ERRORLEVEL%
