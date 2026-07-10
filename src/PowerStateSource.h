#pragma once

#include <QObject>

#include <functional>
#include <utility>

class PowerStateSource : public QObject
{
    Q_OBJECT

public:
    using ResumeHandler = std::function<void()>;

    using QObject::QObject;
    ~PowerStateSource() override = default;

    virtual void start() = 0;
    virtual void stop() = 0;

    void setResumeHandler(ResumeHandler handler)
    {
        resume_handler = std::move(handler);
    }

public slots:
    void handlePrepareForSleep(bool sleeping)
    {
        if (!sleeping)
            publishResume();
    }

protected:
    void publishResume()
    {
        if (resume_handler)
            resume_handler();
    }

private:
    ResumeHandler resume_handler;
};

PowerStateSource* createPowerStateSource(QObject* parent);
