@echo off
setlocal
set "ROOT=%~dp0.."
if exist "%ROOT%\build-win\bin\modelforge.exe" (
  set "EXE=%ROOT%\build-win\bin\modelforge.exe"
) else if exist "%ROOT%\build\bin\modelforge.exe" (
  set "EXE=%ROOT%\build\bin\modelforge.exe"
) else (
  echo ModelForge executable not found. Build it first:
  echo   scripts\build-windows.ps1
  pause
  exit /b 1
)
pushd "%ROOT%"
"%EXE%" serve --root "%ROOT%" %*
popd
endlocal
