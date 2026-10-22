@echo off
echo =======================================================
echo    BUILDING BEARBYTE PC RESOURCE MONITOR STANDALONE EXE
echo =======================================================
echo.
echo Installing dependencies...
py -3.12 -m pip install -r requirements.txt

echo.
echo Building BearByte.exe using PyInstaller (with UAC elevation for CPU Diode access)...
py -3.12 -m PyInstaller --clean --onefile --noconsole --uac-admin --name "BearByte" --add-data "C:\Users\gokul\AppData\Local\Programs\Python\Python312\Lib\site-packages\WinTmp\LibreHardwareMonitorLib.dll;." --collect-all customtkinter bear_monitor_gui.py

echo.
echo Copying BearByte.exe to root folder...
copy /y dist\BearByte.exe .\BearByte.exe
copy /y dist\BearByte.exe .\BearMonitor.exe

echo.
echo =======================================================
echo BUILD FINISHED!
echo Executable is located at: BearByte.exe
echo =======================================================
pause
