@echo off
cd /d "%~dp0.."

if not exist data mkdir data
set EXE=lab4_queue.exe
set CSV=data\results.csv
set COMMON=gen=2 work=2 cap=10 service=5

echo Starting standard runs...
call :runset std 1000 1

echo Starting individual runs...
call :runset ind 1350 7790

echo Starting sweep runs (this may take a minute)...
for %%M in (wait drop) do (
  for %%I in (40 20 10 5 2 1) do (
    for /L %%R in (1,1,3) do (
      %EXE% mode=%%M %COMMON% interval=%%I ops=1350 seed=7790 csv=%CSV% label=sweep-int%%I-%%M >nul
    )
  )
)

echo.
echo DONE. Now run: python scripts\plot.py
pause
goto :eof

:runset
for %%M in (wait drop) do (
  for %%I in (20 2) do (
    for /L %%R in (1,1,3) do (
      %EXE% mode=%%M %COMMON% interval=%%I ops=%2 seed=%3 csv=%CSV% label=%1-int%%I-%%M >nul
    )
  )
)
goto :eof