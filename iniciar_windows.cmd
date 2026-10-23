@echo off
setlocal
set "APP=%~dp0build-mingw\cualpdf.exe"
if not exist "%APP%" (
  echo Primero ejecuta compilar_windows.cmd.
  pause
  exit /b 1
)
start "" "%APP%" %*
