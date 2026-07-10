#include "SteamStateSource.h"

#include <windows.h>

#include <QTimer>
#include <QWinEventNotifier>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
constexpr wchar_t MappingName[] = L"Local\\SteamFrontBar.State.v1";
constexpr wchar_t ChangedEventName[] = L"Local\\SteamFrontBar.Changed.v1";

struct SharedMappingV1
{
    alignas(8) volatile std::uint64_t write_sequence;
    std::uint32_t steam_process_id;
    ValveLedsSnapshot snapshot;
};

static_assert(offsetof(SharedMappingV1, steam_process_id) == 8,
              "Steam front-bar PID offset changed");
static_assert(offsetof(SharedMappingV1, snapshot) == 12,
              "Steam front-bar snapshot offset changed");
static_assert(sizeof(SharedMappingV1) == 112, "Steam front-bar mapping size changed");

class WindowsStateSource final : public SteamStateSource
{
public:
    explicit WindowsStateSource(QObject* parent)
        : SteamStateSource(parent)
    {
        retry_timer = new QTimer(this);
        retry_timer->setInterval(2000);
        connect(retry_timer, &QTimer::timeout, this, [this]() {
            openMapping();
        });
    }

    ~WindowsStateSource() override
    {
        stop();
    }

    void start() override
    {
        openMapping();
    }

    void stop() override
    {
        retry_timer->stop();
        disconnectMapping();
    }

private:
    static bool copySnapshot(const volatile SharedMappingV1* shared,
                             ValveLedsSnapshot& snapshot, std::uint32_t& process_id)
    {
        for (unsigned int attempt = 0; attempt < 100; ++attempt) {
            const std::uint64_t before = shared->write_sequence;
            MemoryBarrier();
            if ((before & 1U) != 0) {
                SwitchToThread();
                continue;
            }

            process_id = shared->steam_process_id;
            std::memcpy(&snapshot, const_cast<const ValveLedsSnapshot*>(&shared->snapshot),
                        sizeof(snapshot));
            MemoryBarrier();
            const std::uint64_t after = shared->write_sequence;
            if (before == after && (after & 1U) == 0)
                return true;
        }

        return false;
    }

    void openMapping()
    {
        if (shared)
            return;

        mapping_handle = OpenFileMappingW(FILE_MAP_READ, FALSE, MappingName);
        if (!mapping_handle) {
            waitForSteam();
            return;
        }

        shared = static_cast<const volatile SharedMappingV1*>(
            MapViewOfFile(mapping_handle, FILE_MAP_READ, 0, 0, sizeof(SharedMappingV1)));
        changed_event = OpenEventW(SYNCHRONIZE, FALSE, ChangedEventName);

        ValveLedsSnapshot snapshot;
        std::uint32_t process_id = 0;
        if (!shared || !changed_event || !copySnapshot(shared, snapshot, process_id) ||
            process_id == 0) {
            disconnectMapping();
            waitForSteam();
            return;
        }

        steam_process = OpenProcess(SYNCHRONIZE, FALSE, process_id);
        if (!steam_process) {
            disconnectMapping();
            waitForSteam();
            return;
        }

        change_notifier = new QWinEventNotifier(changed_event, this);
        connect(change_notifier, &QWinEventNotifier::activated, this, [this](HANDLE) {
            readSnapshot();
        });

        process_notifier = new QWinEventNotifier(steam_process, this);
        connect(process_notifier, &QWinEventNotifier::activated, this, [this](HANDLE) {
            publishStatus("Steam front bar stopped.");
            publishUnavailable();
            disconnectMapping();
            retry_timer->start();
        });

        retry_timer->stop();
        publishStatus("Reading Steam front-bar shared memory.");
        publishSnapshot(snapshot);
    }

    void readSnapshot()
    {
        ValveLedsSnapshot snapshot;
        std::uint32_t process_id = 0;
        if (shared && copySnapshot(shared, snapshot, process_id))
            publishSnapshot(snapshot);
    }

    void waitForSteam()
    {
        publishStatus("Waiting for Steam front-bar shared memory.");
        retry_timer->start();
    }

    void disconnectMapping()
    {
        delete process_notifier;
        process_notifier = nullptr;
        delete change_notifier;
        change_notifier = nullptr;

        if (shared) {
            UnmapViewOfFile(const_cast<const SharedMappingV1*>(shared));
            shared = nullptr;
        }
        if (steam_process) {
            CloseHandle(steam_process);
            steam_process = nullptr;
        }
        if (changed_event) {
            CloseHandle(changed_event);
            changed_event = nullptr;
        }
        if (mapping_handle) {
            CloseHandle(mapping_handle);
            mapping_handle = nullptr;
        }
    }

    QTimer* retry_timer = nullptr;
    QWinEventNotifier* change_notifier = nullptr;
    QWinEventNotifier* process_notifier = nullptr;
    HANDLE mapping_handle = nullptr;
    HANDLE changed_event = nullptr;
    HANDLE steam_process = nullptr;
    const volatile SharedMappingV1* shared = nullptr;
};
}

SteamStateSource* createSteamStateSource(QObject* parent)
{
    return new WindowsStateSource(parent);
}
