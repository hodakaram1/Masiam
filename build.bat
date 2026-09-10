@echo off
setlocal
echo ==========================================
echo   Building DBKKernel driver & Imno GUI
echo ==========================================

REM Find MSBuild path
set "MSBUILD_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe"
if not exist "%MSBUILD_PATH%" (
    set "MSBUILD_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\MSBuild\Current\Bin\MSBuild.exe"
)

if not exist "%MSBUILD_PATH%" (
    echo [ERROR] MSBuild.exe could not be found automatically!
    goto error
)

echo Using MSBuild: "%MSBUILD_PATH%"
echo.

REM Run build in Debug x64 and output detailed error log to build_error.log if it fails
"%MSBUILD_PATH%" King.sln -p:Configuration=Debug -p:Platform=x64 /v:normal /fl /flp:LogFile=build_error.log;ErrorsOnly

if %ERRORLEVEL% NEQ 0 (
    goto error
)

echo.
echo ==========================================
echo   BUILD SUCCESSFUL! (Debug x64)
echo ==========================================

REM Make sure the DBK64 driver sits next to the app (both projects output to solution x64\Debug)
if not exist "x64\Debug\DBK64.sys" (
    if exist "DBKKernel\x64\Debug\DBK64.sys" (
        copy /Y "DBKKernel\x64\Debug\DBK64.sys" "x64\Debug\" >nul
        echo Copied DBK64.sys next to Imno.exe
    ) else (
        echo [INFO] DBK64.sys already in x64\Debug or driver output is elsewhere
    )
) else (
    echo DBK64.sys is already next to Imno.exe
)

echo.
pause
exit /b 0

:error
echo.
echo ==========================================
echo   BUILD FAILED! Check build_error.log for details.
echo ==========================================
if exist build_error.log (
    echo ------------------------------------------
    type build_error.log
    echo ------------------------------------------
)
pause
exit /b 1
