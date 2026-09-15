#pragma once

#include <QMenu>
#include <QObject>
#include <QWidget>

#include "OpenRGBPluginInterface.h"

class OpenRGBSteamSinkPlugin : public QObject, public OpenRGBPluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID OpenRGBPluginInterface_IID FILE "OpenRGBSteamSinkPlugin.json")
    Q_INTERFACES(OpenRGBPluginInterface)

public:
    OpenRGBPluginInfo GetPluginInfo() override;
    unsigned int GetPluginAPIVersion() override;

    void Load(OpenRGBPluginAPIInterface* resource_manager_ptr) override;
    QWidget* GetWidget() override;
    QMenu* GetTrayMenu() override;
    void Unload() override;
    void OnProfileAboutToLoad() override {}
    void OnProfileLoad(nlohmann::json) override {}
    nlohmann::json OnProfileSave() override { return nlohmann::json::object(); }
    unsigned char* OnSDKCommand(unsigned int, unsigned char*, unsigned int* size) override
    {
        if (size) *size = 0;
        return nullptr;
    }
    void ProfileManagerUpdated(unsigned int) override {}
    void ResourceManagerUpdated(unsigned int) override {}
    void SettingsManagerUpdated(unsigned int) override {}

private:
    OpenRGBPluginAPIInterface* resource_manager = nullptr;
    QObject* runtime = nullptr;
    QWidget* widget = nullptr;
};
