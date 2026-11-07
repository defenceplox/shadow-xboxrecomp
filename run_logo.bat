@echo off
REM Shadow the Hedgehog - run and reach the boot logo (or the loading screen).
REM
REM A plain `shadow_recomp.exe` crashes on purpose of a known bug: the title
REM calls DirectSoundCreate, gets DSERR_NODRIVER because the AC'97 codec poll
REM never comes up, never checks the result, and then calls IDirectSound8 methods
REM on the resulting NULL object. RECOMP_AC97_READY=1 makes the codec report
REM ready, so the create succeeds and the title renders instead of dying.
REM
REM What you should see: either the "PRESENTED BY SEGA" logo or the loading
REM screen. Which one is a race - about 2 runs in 3 land on the loading screen.
cd /d "%~dp0"
set RECOMP_AC97_READY=1
echo Running with the codec reported ready (no sound is produced).
shadow_recomp.exe %*
pause
