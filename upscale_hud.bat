@echo off
rem Makes the HUD (text, icons, menus) sharp: upscales with AI (Real-ESRGAN) the HUD pictures the game has shown so far.
rem Play with run_rayman_hd.bat first (it collects the pictures into hud_dump), then run this. Run it again after
rem playing new parts of the game: only new pictures are processed.
cd /d "%~dp0"
python tools\hud_upscale.py
pause
