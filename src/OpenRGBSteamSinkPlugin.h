#pragma once

#include <QMenu>
#include <QObject>
#include <QWidget>

#include "OpenRGBPluginInterface.h"
#include "ResourceManagerInterface.h"

class OpenRGBSteamSinkPlugin : public QObject, public OpenRGBPluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID OpenRGBPluginInterface_IID)
    Q_INTERFACES(OpenRGBPluginInterface)

public:
    OpenRGBPluginInfo GetPluginInfo() override;
    unsigned int GetPluginAPIVersion() override;

    void Load(ResourceManagerInterface* resource_manager_ptr) override;
    QWidget* GetWidget() override;
    QMenu* GetTrayMenu() override;
    void Unload() override;

private:
    ResourceManagerInterface* resource_manager = nullptr;
    QObject* runtime = nullptr;
    QWidget* widget = nullptr;
};
