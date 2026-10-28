@echo off
setlocal

echo.
echo ==============================
echo       C++ LINE COUNTER
echo ==============================
echo.
echo Project folder:
echo %~dp0
echo.
echo Counting...
echo.

powershell -NoProfile -Command "$files = Get-ChildItem -Path '%~dp0' -Recurse -File | Where-Object { $_.Extension -in '.cpp','.h','.hpp','.c' -and $_.FullName -notmatch '\\(bin|obj)\\' }; $total = 0; foreach ($file in $files) { $total += (Get-Content $file.FullName | Measure-Object -Line).Lines }; Write-Host ('Files found: ' + $files.Count); Write-Host ('Total lines: ' + $total)"

echo.
echo ==============================
echo Finished!
echo ==============================
echo.
pause