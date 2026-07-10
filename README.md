# OpenRGB Steam Sink

OpenRGB plugin that maps Steam's 17-LED Front bar state onto a segment of any
OpenRGB controller.

The effect engine and OpenRGB mapping are shared across platforms. Only the
state transport differs:

- Linux reads the 100-byte VLED snapshot from `/dev/valve-leds-shim`.
- Windows reads the same snapshot from `Local\SteamFrontBar.State.v1`, produced
  by the `steam-frontbar` mod.

## Linux development

```sh
nix develop
qmake6 OpenRGBSteamSinkPlugin.pro
make
```

## Windows development

Use the 64-bit MSVC 2019 Qt 5.15.0 environment used by the OpenRGB 1.0rc3
Qt 5 Windows build, set
`OPENRGB_SOURCE_DIR` to an OpenRGB 1.0rc3 source checkout, then run:

```bat
scripts\build-windows.bat
```

The Windows OpenRGB process must run in the same interactive session and as a
user allowed to open the mod's `Local\` shared-memory objects.
