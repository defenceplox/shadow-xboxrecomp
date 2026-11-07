@echo off
REM Shadow the Hedgehog - the OLD behaviour (loading screen loop), no env flags.
REM
REM This points the build at the pre-2026-11-07 truncated extraction
REM (..\WORKSPACE\files_trunc, 1,509 of the disc's 3,510 files). With the sound
REM bank missing, the audio stage aborts early and the title falls through to
REM the looping loading screen instead of crashing. Useful for A/B only - the
REM full tree at ..\WORKSPACE\files is the honest one.
cd /d "%~dp0"
set RECOMP_DATA_DIR=%~dp0..\WORKSPACE\files_trunc
set RECOMP_AC97_READY=1
if not exist "%RECOMP_DATA_DIR%" (
    echo ERROR: %RECOMP_DATA_DIR% not found.
    echo The truncated tree was removed; only the full one remains.
    pause
    exit /b 1
)
echo Using the TRUNCATED tree (old loading-screen behaviour):
echo   %RECOMP_DATA_DIR%
shadow_recomp.exe %*
pause
