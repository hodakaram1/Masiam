@echo off
setlocal
echo ==========================================
echo   Building DBKKernel driver & Imno GUI
echo   Configuration: Release x64
echo ==========================================

REM Override with:  build.bat Debug   to build the Debug configuration instead
set "CONFIG=Release"
if /I "%~1"=="Debug" set "CONFIG=Debug"
if /I "%~1"=="Release" set "CONFIG=Release"
set "PLATFORM=x64"

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
echo Configuration: %CONFIG% %PLATFORM%
echo.

REM Build the whole solution (DBKKernel first, then Imno) and log errors to build_error.log
"%MSBUILD_PATH%" King.sln -p:Configuration=%CONFIG% -p:Platform=%PLATFORM% /v:normal /fl /flp:LogFile=build_error.log;ErrorsOnly

if %ERRORLEVEL% NEQ 0 (
    goto error
)

echo.
echo ==========================================
echo   BUILD SUCCESSFUL! (%CONFIG% %PLATFORM%)
echo ==========================================

REM Both projects output to x64\<Config>\, so DBK64.sys should already be next to Imno.exe.
REM If the driver landed elsewhere, copy it next to the app.
if not exist "x64\%CONFIG%\DBK64.sys" (
    if exist "DBKKernel\x64\%CONFIG%\DBK64.sys" (
        copy /Y "DBKKernel\x64\%CONFIG%\DBK64.sys" "x64\%CONFIG%\" >nul
        echo Copied DBK64.sys next to Imno.exe
    ) else (
        echo [INFO] DBK64.sys not found - make sure the DBKKernel project built successfully.
    )
) else (
    echo DBK64.sys is already next to Imno.exe
)

echo.
echo Output folder: x64\%CONFIG%\
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
