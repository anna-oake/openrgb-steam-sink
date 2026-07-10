#include "PowerStateSource.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>

#include <windows.h>

namespace
{
class WindowsPowerStateSource final : public PowerStateSource, public QAbstractNativeEventFilter
{
public:
    using PowerStateSource::PowerStateSource;

    ~WindowsPowerStateSource() override
    {
        stop();
    }

    void start() override
    {
        if (active)
            return;

        QCoreApplication::instance()->installNativeEventFilter(this);
        active = true;
    }

    void stop() override
    {
        if (!active)
            return;

        QCoreApplication::instance()->removeNativeEventFilter(this);
        active = false;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override
#else
    bool nativeEventFilter(const QByteArray&, void* message, long*) override
#endif
    {
        const auto* native_message = static_cast<const MSG*>(message);
        if (native_message->message == WM_POWERBROADCAST &&
            native_message->wParam == PBT_APMRESUMEAUTOMATIC) {
            publishResume();
        }

        return false;
    }

private:
    bool active = false;
};
}

PowerStateSource* createPowerStateSource(QObject* parent)
{
    return new WindowsPowerStateSource(parent);
}
