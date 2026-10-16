@echo off
rem Raptor Engine repository commands: see tools\raptor.py, or run without arguments for a menu.
setlocal

set "RAPTOR_PY=%~dp0tools\raptor.py"

rem The py launcher first: "python" can be the Microsoft Store alias, which opens the Store.
where py >nul 2>nul
if not errorlevel 1 (
    py -3 "%RAPTOR_PY%" %*
    goto done
)
where python >nul 2>nul
if not errorlevel 1 (
    python "%RAPTOR_PY%" %*
    goto done
)
echo Python 3 not found: install it from python.org and add it to PATH.
set "RAPTOR_RESULT=1"
goto pause_if_double_click

:done
set "RAPTOR_RESULT=%errorlevel%"

:pause_if_double_click
rem Started with a double click: keep the window open to read the output.
echo %cmdcmdline% | find /i "%~0" >nul
if not errorlevel 1 pause

exit /b %RAPTOR_RESULT%