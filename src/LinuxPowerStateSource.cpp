#include "PowerStateSource.h"

#include <QDBusConnection>

namespace
{
constexpr const char* LoginService = "org.freedesktop.login1";
constexpr const char* LoginPath = "/org/freedesktop/login1";
constexpr const char* LoginInterface = "org.freedesktop.login1.Manager";

class LinuxPowerStateSource final : public PowerStateSource
{
public:
    using PowerStateSource::PowerStateSource;

    ~LinuxPowerStateSource() override
    {
        stop();
    }

    void start() override
    {
        if (connected)
            return;

        connected = QDBusConnection::systemBus().connect(
            LoginService,
            LoginPath,
            LoginInterface,
            "PrepareForSleep",
            this,
            SLOT(handlePrepareForSleep(bool)));
    }

    void stop() override
    {
        if (!connected)
            return;

        QDBusConnection::systemBus().disconnect(
            LoginService,
            LoginPath,
            LoginInterface,
            "PrepareForSleep",
            this,
            SLOT(handlePrepareForSleep(bool)));
        connected = false;
    }

private:
    bool connected = false;
};
}

PowerStateSource* createPowerStateSource(QObject* parent)
{
    return new LinuxPowerStateSource(parent);
}
