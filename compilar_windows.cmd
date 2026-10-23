@echo off
setlocal
cd /d "%~dp0"
set "TOOLS=%~dp0..\.cualpdf-tools\Scripts"
set "QT=%~dp0..\.qt\6.9.3\mingw_64"
set "MINGW=%~dp0..\.qt\Tools\mingw1310_64\bin"
set "PATH=%MINGW%;%QT%\bin;%TOOLS%;C:\Program Files\Git\cmd;%PATH%"
if not exist "%MINGW%\g++.exe" (
  echo Falta GCC: %MINGW%
  exit /b 1
)
if not exist "%QT%\bin\windeployqt.exe" (
  echo Falta Qt: %QT%
  exit /b 1
)
rem Leer la version sin que CMake modifique la configuracion de Git.
for /f "delims=" %%V in ('git describe --tags --match "v[0-9]*" --always --dirty') do set "GITHUB_REF_NAME=%%V"
set "GITHUB_REF_TYPE=tag"
"%TOOLS%\cmake.exe" -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT%" -DCMAKE_C_COMPILER="%MINGW%\gcc.exe" -DCMAKE_CXX_COMPILER="%MINGW%\g++.exe" -DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE -DBUILD_TESTING=ON
if errorlevel 1 exit /b 1
"%TOOLS%\cmake.exe" --build build-mingw --target cualpdf cualpdf_pdfdocument_structure_test cualpdf_ocr_test --parallel 2
if errorlevel 1 exit /b 1
for /r "%~dp0build-mingw" %%E in (cualpdf.exe cualpdf_pdfdocument_structure_test.exe cualpdf_ocr_test.exe) do (
  if exist "%%E" "%QT%\bin\windeployqt.exe" --release --no-translations --no-opengl-sw --no-system-d3d-compiler "%%E"
  if errorlevel 1 exit /b 1
)
rem La prueba OCR usa QT_QPA_PLATFORM=offscreen; windeployqt copia solo windows.
copy /y "%QT%\plugins\platforms\qoffscreen.dll" "build-mingw\platforms\qoffscreen.dll" >nul
if errorlevel 1 exit /b 1
"%TOOLS%\ctest.exe" --test-dir build-mingw --output-on-failure
if errorlevel 1 exit /b 1
echo Compilacion y pruebas terminadas.
