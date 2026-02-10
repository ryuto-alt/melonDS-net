@echo off
setlocal enabledelayedexpansion

set MSYS2=C:\msys64\mingw64\bin
set BUILD_DIR=%~dp0build
set DIST_DIR=%BUILD_DIR%\ds
set ZIP_OUT=%~dp0melonDS-dist.zip
set VERSION_FILE=%~dp0version.txt
set REPO=ryuto-alt/melonDS-net
set PATH=%MSYS2%;C:\Program Files\GitHub CLI;%PATH%

echo === melonDS Build + Release ===

:: Build
echo [1/4] Building melonDS...
cmake --build "%BUILD_DIR%" 2>&1
if errorlevel 1 (
    echo BUILD FAILED
    pause
    exit /b 1
)

:: Copy exe + DLLs to dist folder
echo [2/4] Updating dist folder...
copy "%BUILD_DIR%\melonDS.exe" "%DIST_DIR%\" >nul
call :copy_deps "%DIST_DIR%\melonDS.exe"
if not exist "%DIST_DIR%\platforms" mkdir "%DIST_DIR%\platforms"
if not exist "%DIST_DIR%\styles" mkdir "%DIST_DIR%\styles"
if not exist "%DIST_DIR%\multimedia" mkdir "%DIST_DIR%\multimedia"
copy "%BUILD_DIR%\platforms\*.dll" "%DIST_DIR%\platforms\" >nul 2>&1
copy "%BUILD_DIR%\styles\*.dll" "%DIST_DIR%\styles\" >nul 2>&1
copy "%BUILD_DIR%\multimedia\*.dll" "%DIST_DIR%\multimedia\" >nul 2>&1
echo Done.

:: Increment version
set /p OLD_VER=<"%VERSION_FILE%"
set /a NEW_VER=%OLD_VER%+1
echo %NEW_VER%> "%VERSION_FILE%"
echo %NEW_VER%> "%DIST_DIR%\version.txt"
echo   Version: v%OLD_VER% -^> v%NEW_VER%

:: Create zip
echo [3/4] Creating zip...
if exist "%ZIP_OUT%" del "%ZIP_OUT%"
python3 -c "import shutil; shutil.make_archive(r'%~dp0melonDS-dist', 'zip', r'%DIST_DIR%')"

:: Upload to GitHub Release
echo [4/4] Uploading to GitHub...
gh release delete "v%NEW_VER%" -R "%REPO%" --yes >nul 2>&1
gh release create "v%NEW_VER%" "%ZIP_OUT%" -R "%REPO%" --title "melonDS v%NEW_VER%" --notes "Auto-update release" 2>&1
if errorlevel 1 (
    echo RELEASE UPLOAD FAILED
    pause
    exit /b 1
)

:: Push version.txt to repo
cd /d "%~dp0"
if not exist melonDS-net-meta (
    mkdir melonDS-net-meta
    cd melonDS-net-meta
    git init
    git config user.email "ryuto-alt@users.noreply.github.com"
    git config user.name "ryuto-alt"
    git remote add origin "https://github.com/%REPO%.git"
) else (
    cd melonDS-net-meta
)
copy "%VERSION_FILE%" version.txt >nul
git add version.txt
git commit -m "v%NEW_VER%" >nul 2>&1
git branch -M main
git push -f origin main >nul 2>&1
cd ..

echo.
echo === Complete! v%NEW_VER% released ===
echo   melonDS will auto-update on next launch!
echo.
pause
exit /b 0

:copy_deps
for /f "tokens=3" %%d in ('objdump -p "%~1" 2^>nul ^| findstr "DLL Name"') do (
    if not exist "%DIST_DIR%\%%d" (
        if exist "%MSYS2%\%%d" (
            echo   + %%d
            copy "%MSYS2%\%%d" "%DIST_DIR%\" >nul
            call :copy_deps "%DIST_DIR%\%%d"
        )
    )
)
exit /b 0
