#pragma once

#include "ValveLedsSnapshot.h"

#include <QObject>
#include <QString>

#include <functional>
#include <utility>

class SteamStateSource : public QObject
{
public:
    using SnapshotHandler = std::function<void(const ValveLedsSnapshot&)>;
    using StatusHandler = std::function<void(QString)>;
    using UnavailableHandler = std::function<void()>;

    using QObject::QObject;
    ~SteamStateSource() override = default;

    virtual void start() = 0;
    virtual void stop() = 0;

    void setSnapshotHandler(SnapshotHandler handler)
    {
        snapshot_handler = std::move(handler);
    }

    void setStatusHandler(StatusHandler handler)
    {
        status_handler = std::move(handler);
    }

    void setUnavailableHandler(UnavailableHandler handler)
    {
        unavailable_handler = std::move(handler);
    }

protected:
    void publishSnapshot(const ValveLedsSnapshot& snapshot)
    {
        if (snapshot_handler)
            snapshot_handler(snapshot);
    }

    void publishStatus(QString status)
    {
        if (status_handler)
            status_handler(std::move(status));
    }

    void publishUnavailable()
    {
        if (unavailable_handler)
            unavailable_handler();
    }

private:
    SnapshotHandler snapshot_handler;
    StatusHandler status_handler;
    UnavailableHandler unavailable_handler;
};

SteamStateSource* createSteamStateSource(QObject* parent);
