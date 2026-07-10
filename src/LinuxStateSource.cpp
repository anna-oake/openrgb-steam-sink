#include "SteamStateSource.h"

#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace
{
constexpr const char* ShimDevicePath = "/dev/valve-leds-shim";

class LinuxStateSource final : public SteamStateSource
{
public:
    explicit LinuxStateSource(QObject* parent)
        : SteamStateSource(parent)
    {
        retry_timer = new QTimer(this);
        retry_timer->setInterval(2000);
        connect(retry_timer, &QTimer::timeout, this, [this]() {
            openDevice();
        });
    }

    ~LinuxStateSource() override
    {
        stop();
    }

    void start() override
    {
        openDevice();
    }

    void stop() override
    {
        retry_timer->stop();
        disconnectDevice();
    }

private:
    void openDevice()
    {
        if (fd >= 0)
            return;

        fd = ::open(ShimDevicePath, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            publishStatus(QString("Waiting for %1.").arg(ShimDevicePath));
            retry_timer->start();
            return;
        }

        retry_timer->stop();
        publishStatus(QString("Reading %1.").arg(ShimDevicePath));
        readSnapshot();

        notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this]() {
            notifier->setEnabled(false);
            readSnapshot();
            notifier->setEnabled(true);
        });
    }

    void disconnectDevice()
    {
        delete notifier;
        notifier = nullptr;

        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    void readSnapshot()
    {
        ValveLedsSnapshot snapshot;
        const ssize_t bytes_read = ::read(fd, &snapshot, sizeof(snapshot));

        if (bytes_read == static_cast<ssize_t>(sizeof(snapshot))) {
            publishSnapshot(snapshot);
            return;
        }
        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;

        publishStatus(QString("Lost %1.").arg(ShimDevicePath));
        publishUnavailable();
        disconnectDevice();
        retry_timer->start();
    }

    QSocketNotifier* notifier = nullptr;
    QTimer* retry_timer = nullptr;
    int fd = -1;
};
}

SteamStateSource* createSteamStateSource(QObject* parent)
{
    return new LinuxStateSource(parent);
}
