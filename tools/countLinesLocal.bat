@echo off
setlocal

echo.
echo ==============================
echo       C++ LINE COUNTER
echo ==============================
echo.
echo Folder:
echo %~dp0
echo.
echo Counting...
echo.

powershell -NoProfile -Command "$files = Get-ChildItem -Path '%~dp0' -File | Where-Object { $_.Extension -in '.cpp','.h','.hpp','.c' }; $total = 0; foreach ($file in $files) { $total += (Get-Content -LiteralPath $file.FullName | Measure-Object -Line).Lines }; Write-Host ('Files found: ' + $files.Count); Write-Host ('Total lines: ' + $total)"

echo.
echo ==============================
echo Finished!
echo ==============================
echo.
pause