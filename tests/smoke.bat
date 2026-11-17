@echo off
rem Переходим в корневую папку LR4 (на уровень выше от tests)
cd /d "%~dp0.."

set FAIL=0
echo Starting tests...
echo.

for %%M in (wait drop) do (
  for %%I in (10 1 0) do (
    echo Testing mode=%%M interval=%%I...
    lab4_queue.exe mode=%%M gen=4 work=3 cap=5 interval=%%I service=2 ops=300 | findstr /R /C:"VIOLATIONS: 0$" >nul
    if errorlevel 1 (
        echo   [FAIL]
        set FAIL=1
    ) else (
        echo   [OK]
    )
  )
)

echo.
if %FAIL%==0 (
    echo ALL TESTS PASSED
) else (
    echo TESTS FAILED
)
pause