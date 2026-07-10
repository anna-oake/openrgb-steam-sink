@echo off
setlocal

if not defined OPENRGB_SOURCE_DIR (
    echo OPENRGB_SOURCE_DIR must point to the OpenRGB source tree.
    exit /b 1
)

qmake OpenRGBSteamSinkPlugin.pro CONFIG-=debug_and_release CONFIG+=release || exit /b 1
nmake || exit /b 1
