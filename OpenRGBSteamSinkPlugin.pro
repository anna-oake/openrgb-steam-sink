TEMPLATE = lib
CONFIG += plugin c++17
QT += core gui widgets

TARGET = OpenRGBSteamSinkPlugin
DESTDIR = build

OPENRGB_SOURCE_DIR = $$(OPENRGB_SOURCE_DIR)
isEmpty(OPENRGB_SOURCE_DIR) {
    error("OPENRGB_SOURCE_DIR is not set; enter nix develop or export it manually")
}

INCLUDEPATH += \
    $$OPENRGB_SOURCE_DIR \
    $$OPENRGB_SOURCE_DIR/dependencies/json \
    $$OPENRGB_SOURCE_DIR/i2c_smbus \
    $$OPENRGB_SOURCE_DIR/net_port \
    $$OPENRGB_SOURCE_DIR/RGBController

HEADERS += \
    src/OpenRGBSteamSinkPlugin.h \
    src/PowerStateSource.h \
    src/SteamStateSource.h \
    src/ValveLedsSnapshot.h

SOURCES += \
    src/OpenRGBSteamSinkPlugin.cpp

win32 {
    DEFINES += NOMINMAX WIN32_LEAN_AND_MEAN
    SOURCES += \
        src/WindowsPowerStateSource.cpp \
        src/WindowsStateSource.cpp
}

unix:!macx {
    QT += dbus
    SOURCES += \
        src/LinuxPowerStateSource.cpp \
        src/LinuxStateSource.cpp
}

macx {
    error("OpenRGB Steam Sink does not support macOS")
}
