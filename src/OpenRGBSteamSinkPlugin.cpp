#include "OpenRGBSteamSinkPlugin.h"
#include "PowerStateSource.h"
#include "SteamStateSource.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QShowEvent>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "RGBController.h"
#include "SettingsManager.h"

namespace
{
constexpr unsigned int MinRealLedCount = SteamLedCount;
constexpr unsigned int MaxRealLedCount = SteamLedCount * 10;
constexpr int StaticRedrawDelayMs = 100;
constexpr const char* PluginName = "OpenRGB Steam Sink";
constexpr const char* SettingsKey = "steam-sink";

struct TargetOption
{
    QString label;
    RGBController* controller = nullptr;
    unsigned int start_index = 0;
    unsigned int led_count = 0;
};

struct Rgb8
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

struct EffectState
{
    std::uint8_t effect = 255;
    unsigned int led_count = 0;
    std::uint8_t breath_level = 32;
    bool breath_rising = false;
    unsigned int patrol_slot = 1;
    unsigned int patrol_tail = 0;
    bool patrol_reverse = false;
    std::uint8_t patrol_hold = 0;
    std::uint8_t patrol_hold_reload = 0;
    std::uint8_t patrol_num = 0;
    unsigned int rainbow_slot = 1;
    std::uint8_t rainbow_phase = 0;
    std::uint8_t rainbow_ramp = 0;
    std::vector<Rgb8> pixels;
    std::vector<Rgb8> rainbow_source;
};

class RefreshingComboBox : public QComboBox
{
public:
    using QComboBox::QComboBox;

    std::function<void()> refresh;

    void showPopup() override
    {
        if (refresh)
            refresh();

        QComboBox::showPopup();
    }
};

class SteamSinkRuntime : public QObject
{
public:
    explicit SteamSinkRuntime(ResourceManagerInterface* resource_manager_ptr, QObject* parent = nullptr)
        : QObject(parent)
        , resource_manager(resource_manager_ptr)
    {
        loadConfig();

        effect_timer = new QTimer(this);
        effect_timer->setTimerType(Qt::PreciseTimer);
        connect(effect_timer, &QTimer::timeout, this, [this]() {
            applyLastSnapshot(true, false);
        });

        state_source = createSteamStateSource(this);
        state_source->setStatusHandler([this](QString status) {
            setStatus(std::move(status));
        });
        state_source->setSnapshotHandler([this](const ValveLedsSnapshot& snapshot) {
            receiveSnapshot(snapshot);
        });
        state_source->setUnavailableHandler([this]() {
            sourceUnavailable();
        });
        state_source->start();

        power_state_source = createPowerStateSource(this);
        power_state_source->setResumeHandler([this]() {
            force_direct_mode = true;
            QTimer::singleShot(500, this, [this]() {
                applyLastSnapshot(false, true);
            });
        });
        power_state_source->start();
    }

    ~SteamSinkRuntime() override
    {
        effect_timer->stop();
        power_state_source->stop();
        state_source->stop();
        restoreControllerState();
    }

    std::vector<TargetOption> enumerateTargets()
    {
        std::vector<TargetOption> result;

        if (!resource_manager)
            return result;

        if (!device_detection_complete) {
            resource_manager->WaitForDeviceDetection();
            device_detection_complete = true;
        }

        for (RGBController* controller : resource_manager->GetRGBControllers()) {
            const QString controller_name = controllerLabel(controller);
            const unsigned int controller_leds = controller->colors.size();

            addTarget(result, QString("%1 - whole device (%2 LEDs)").arg(controller_name).arg(controller_leds),
                      controller, 0, controller_leds);

            for (unsigned int zone_index = 0; zone_index < controller->zones.size(); zone_index++) {
                const zone& target_zone = controller->zones[zone_index];
                const QString zone_name = QString::fromStdString(target_zone.name);

                addTarget(result,
                          QString("%1 - %2 (%3 LEDs)")
                              .arg(controller_name, zone_name)
                              .arg(target_zone.leds_count),
                          controller,
                          target_zone.start_idx,
                          target_zone.leds_count);

                for (unsigned int segment_index = 0; segment_index < target_zone.segments.size(); segment_index++) {
                    const segment& target_segment = target_zone.segments[segment_index];
                    const QString segment_name = QString::fromStdString(target_segment.name);

                    addTarget(result,
                              QString("%1 - %2 / %3 (%4 LEDs)")
                                  .arg(controller_name, zone_name, segment_name)
                                  .arg(target_segment.leds_count),
                              controller,
                              target_zone.start_idx + target_segment.start_idx,
                              target_segment.leds_count);
                }
            }
        }

        return result;
    }

    QString configuredTarget() const
    {
        return target_label;
    }

    unsigned int configuredStartLed() const
    {
        return start_led;
    }

    unsigned int configuredRealLedCount() const
    {
        return real_led_count;
    }

    unsigned int requiredLedCount() const
    {
        return real_led_count;
    }

    bool usesFractionalScaling() const
    {
        return real_led_count % SteamLedCount != 0;
    }

    bool configuredReverse() const
    {
        return reverse;
    }

    QString statusText() const
    {
        return status_text;
    }

    void setStatusCallback(std::function<void()> callback)
    {
        status_callback = std::move(callback);
    }

    void setRealLedCount(unsigned int new_real_led_count)
    {
        real_led_count = clampRealLedCount(new_real_led_count);
        normalizeMappingForCurrentLedCount();
        syncControlledController();
        saveConfig();
        resetEffectState();
        applyLastSnapshot(true, true);
    }

    void setMapping(const QString& label, unsigned int start, bool reverse_order)
    {
        target_label = label;
        start_led = start;
        reverse = reverse_order;

        normalizeMappingForCurrentLedCount();
        syncControlledController();
        saveConfig();
        applyLastSnapshot(true, true);
    }

private:
    static unsigned int clampRealLedCount(unsigned int value)
    {
        return std::max(MinRealLedCount, std::min(MaxRealLedCount, value));
    }

    unsigned int integerScale() const
    {
        if (usesFractionalScaling())
            return 1;

        return real_led_count / SteamLedCount;
    }

    static QString controllerLabel(const RGBController* controller)
    {
        QString name = QString::fromStdString(controller->name);
        QString vendor = QString::fromStdString(controller->vendor);

        if (!vendor.isEmpty())
            return QString("%1 %2").arg(vendor, name).trimmed();

        return name;
    }

    void addTarget(std::vector<TargetOption>& targets,
                   QString label,
                   RGBController* controller,
                   unsigned int start_index,
                   unsigned int led_count) const
    {
        if (led_count < requiredLedCount())
            return;

        TargetOption target;
        target.label = label;
        target.controller = controller;
        target.start_index = start_index;
        target.led_count = led_count;
        targets.push_back(target);
    }

    void normalizeMappingForCurrentLedCount()
    {
        if (target_label.isEmpty()) {
            start_led = 0;
            return;
        }

        const unsigned int required_leds = requiredLedCount();

        for (const TargetOption& target : enumerateTargets()) {
            if (target.label != target_label)
                continue;

            if (start_led + required_leds > target.led_count)
                start_led = 0;

            return;
        }

        target_label.clear();
        start_led = 0;
    }

    void loadConfig()
    {
        SettingsManager* settings_manager = resource_manager ? resource_manager->GetSettingsManager() : nullptr;
        if (!settings_manager)
            return;

        const json settings = settings_manager->GetSettings(SettingsKey);

        if (settings.contains("target") && settings["target"].is_string())
            target_label = QString::fromStdString(settings["target"].get<std::string>());
        if (settings.contains("start_led") && settings["start_led"].is_number_unsigned())
            start_led = settings["start_led"].get<unsigned int>();
        if (settings.contains("real_led_count") && settings["real_led_count"].is_number_unsigned())
            real_led_count = clampRealLedCount(settings["real_led_count"].get<unsigned int>());
        if (settings.contains("reverse") && settings["reverse"].is_boolean())
            reverse = settings["reverse"].get<bool>();

        normalizeMappingForCurrentLedCount();
    }

    void saveConfig()
    {
        SettingsManager* settings_manager = resource_manager ? resource_manager->GetSettingsManager() : nullptr;
        if (!settings_manager)
            return;

        json settings;
        settings["target"] = target_label.toStdString();
        settings["start_led"] = start_led;
        settings["real_led_count"] = real_led_count;
        settings["reverse"] = reverse;

        settings_manager->SetSettings(SettingsKey, settings);
        settings_manager->SaveSettings();
    }

    void setStatus(QString status)
    {
        if (status_text == status)
            return;

        status_text = std::move(status);
        if (status_callback)
            status_callback();
    }

    void receiveSnapshot(const ValveLedsSnapshot& snapshot)
    {
        if (snapshot.magic != ValveLedsMagic ||
            snapshot.version != ValveLedsVersion ||
            snapshot.size != sizeof(snapshot)) {
            setStatus("Steam LED source returned an unknown snapshot format.");
            sourceUnavailable();
            return;
        }

        const bool reset_effect = shouldResetEffect(snapshot);
        const bool had_snapshot = have_snapshot;
        const bool was_enabled = have_snapshot && last_snapshot.enabled;
        const bool was_animated = was_enabled && isAnimatedEffect(last_snapshot.effect);

        last_snapshot = snapshot;
        have_snapshot = true;
        updateOutputMode(!had_snapshot || was_enabled);
        if (reset_effect)
            resetEffectState();
        configureEffectTimer();
        setStatus(QString("Received Steam LED state #%1.").arg(snapshot.seq));
        applyLastSnapshot(reset_effect, true);

        if (was_animated && snapshot.enabled && !isAnimatedEffect(snapshot.effect)) {
            QTimer::singleShot(StaticRedrawDelayMs, this, [this]() {
                if (have_snapshot && last_snapshot.enabled &&
                    !isAnimatedEffect(last_snapshot.effect)) {
                    applyLastSnapshot(false, false);
                }
            });
        }
    }

    void sourceUnavailable()
    {
        if (!have_snapshot)
            return;

        const bool was_enabled = last_snapshot.enabled;
        last_snapshot.enabled = false;
        updateOutputMode(was_enabled);
        configureEffectTimer();
        applyLastSnapshot(false, true);
    }

    static bool isAnimatedEffect(std::uint8_t effect)
    {
        return effect == ValveLedsEffectBreath ||
               effect == ValveLedsEffectPatrol ||
               effect == ValveLedsEffectRainbow;
    }

    static bool isSupportedEffect(std::uint8_t effect)
    {
        return effect == ValveLedsEffectOff ||
               effect == ValveLedsEffectManual ||
               effect == ValveLedsEffectNormal ||
               effect == ValveLedsEffectBreath ||
               effect == ValveLedsEffectPatrol ||
               effect == ValveLedsEffectRainbow;
    }

    bool shouldResetEffect(const ValveLedsSnapshot& snapshot) const
    {
        if (!have_snapshot)
            return true;

        if (snapshot.effect != last_snapshot.effect)
            return true;

        if (requiredLedCount() != effect_state.led_count)
            return true;

        if (snapshot.effect == ValveLedsEffectPatrol &&
            snapshot.patrol_num != last_snapshot.patrol_num)
            return true;

        return false;
    }

    void resetEffectState()
    {
        effect_state = {};
        effect_state.led_count = requiredLedCount();
        effect_state.pixels.assign(effect_state.led_count, {});
        effect_state.rainbow_source.assign(effect_state.led_count, {});
        effect_state.patrol_slot = 1;
        effect_state.rainbow_slot = 1;

        if (have_snapshot) {
            effect_state.effect = last_snapshot.effect;
            effect_state.breath_level = std::max<std::uint8_t>(1, last_snapshot.breath_level);
            effect_state.patrol_num = last_snapshot.patrol_num;
        }
    }

    void updateOutputMode(bool send_black_before_release)
    {
        if (!last_snapshot.enabled) {
            release_after_black = send_black_before_release;
            sink_active = release_after_black;
            return;
        }

        release_after_black = false;
        sink_active = true;
    }

    void configureEffectTimer()
    {
        if (!have_snapshot || !last_snapshot.enabled || !sink_active ||
            !isAnimatedEffect(last_snapshot.effect)) {
            effect_timer->stop();
            return;
        }

        const int interval = static_cast<int>((std::min<std::uint8_t>(last_snapshot.delay, 20) + 1) * 5);

        if (effect_timer->interval() != interval)
            effect_timer->setInterval(interval);

        if (!effect_timer->isActive())
            effect_timer->start();
    }

    static std::uint8_t scaleChannel(std::uint8_t value, std::uint8_t scale)
    {
        return static_cast<std::uint8_t>((static_cast<unsigned int>(value) * scale) / 255);
    }

    static std::uint8_t fadeSub(std::uint8_t value, unsigned int amount)
    {
        if (amount >= value)
            return 0;

        return static_cast<std::uint8_t>(value - amount);
    }

    static std::uint8_t clampByte(unsigned int value)
    {
        return static_cast<std::uint8_t>(std::min(255u, value));
    }

    static unsigned int normalizedPatrolNum(std::uint8_t value)
    {
        if (value == 0)
            return 1;

        if (value > 16)
            return 17;

        return value;
    }

    static Rgb8 snapshotSourceColor(const ValveLedsSnapshot& snapshot, unsigned int steam_led)
    {
        const ValveLedsPixel& pixel = snapshot.pixels[steam_led];

        return {
            scaleChannel(pixel.r, pixel.brightness),
            scaleChannel(pixel.g, pixel.brightness),
            scaleChannel(pixel.b, pixel.brightness),
        };
    }

    static Rgb8 scaledColor(Rgb8 color, std::uint8_t scale)
    {
        return {
            scaleChannel(color.r, scale),
            scaleChannel(color.g, scale),
            scaleChannel(color.b, scale),
        };
    }

    static Rgb8 coverageMappedColor(const std::vector<Rgb8>& logical_output,
                                    unsigned int real_led,
                                    unsigned int real_led_count)
    {
        const double start = static_cast<double>(real_led) * logical_output.size() / real_led_count;
        const double end = static_cast<double>(real_led + 1) * logical_output.size() / real_led_count;
        double r = 0;
        double g = 0;
        double b = 0;
        double coverage = 0;

        for (unsigned int logical_led = static_cast<unsigned int>(start);
             logical_led < static_cast<unsigned int>(std::ceil(end));
             logical_led++) {
            if (logical_led >= logical_output.size())
                continue;

            const double overlap = std::max(0.0, std::min(end, static_cast<double>(logical_led + 1)) -
                                                     std::max(start, static_cast<double>(logical_led)));
            const Rgb8 color = logical_output[logical_led];
            r += color.r * overlap;
            g += color.g * overlap;
            b += color.b * overlap;
            coverage += overlap;
        }

        if (coverage == 0)
            return {};

        return {
            static_cast<std::uint8_t>(r / coverage),
            static_cast<std::uint8_t>(g / coverage),
            static_cast<std::uint8_t>(b / coverage),
        };
    }

    Rgb8 sampledSourceColor(unsigned int real_led, unsigned int real_led_count) const
    {
        const unsigned int steam_led = std::min(
            SteamLedCount - 1,
            (real_led * SteamLedCount) / real_led_count);

        return snapshotSourceColor(last_snapshot, steam_led);
    }

    Rgb8 sourceSlotColor(unsigned int slot, unsigned int slot_count) const
    {
        if (slot == 0 || slot > slot_count)
            return {};

        return sampledSourceColor(slot - 1, slot_count);
    }

    static RGBColor toOpenRgbColor(Rgb8 color)
    {
        return ToRGBColor(color.r, color.g, color.b);
    }

    void renderManualOrNormal()
    {
        const unsigned int led_count = requiredLedCount();
        std::vector<Rgb8> logical_source;

        effect_state.pixels.assign(led_count, {});
        logical_source.reserve(SteamLedCount);

        for (unsigned int steam_led = 0; steam_led < SteamLedCount; steam_led++)
            logical_source.push_back(snapshotSourceColor(last_snapshot, steam_led));

        if (usesFractionalScaling()) {
            for (unsigned int real_led = 0; real_led < led_count; real_led++)
                effect_state.pixels[real_led] = coverageMappedColor(logical_source, real_led, led_count);

            return;
        }

        for (unsigned int steam_led = 0; steam_led < SteamLedCount; steam_led++) {
            const Rgb8 color = logical_source[steam_led];

            for (unsigned int i = 0; i < integerScale(); i++) {
                const unsigned int real_led = steam_led * integerScale() + i;
                if (real_led < effect_state.pixels.size())
                    effect_state.pixels[real_led] = color;
            }
        }
    }

    void renderBreathFrame()
    {
        const unsigned int led_count = requiredLedCount();
        const std::uint8_t max_level = std::max<std::uint8_t>(1, last_snapshot.breath_level);

        if (effect_state.pixels.size() != led_count)
            effect_state.pixels.assign(led_count, {});

        if (!effect_state.breath_rising) {
            effect_state.breath_level = static_cast<std::uint8_t>(effect_state.breath_level - 1);

            if (effect_state.breath_level == 0)
                effect_state.breath_rising = true;
        } else {
            effect_state.breath_level++;

            if (effect_state.breath_level >= max_level) {
                effect_state.breath_level = max_level;
                effect_state.breath_rising = false;
            }
        }

        for (unsigned int i = 0; i < led_count; i++) {
            const Rgb8 source = sampledSourceColor(i, led_count);

            effect_state.pixels[i] = {
                static_cast<std::uint8_t>((source.r * effect_state.breath_level) / max_level),
                static_cast<std::uint8_t>((source.g * effect_state.breath_level) / max_level),
                static_cast<std::uint8_t>((source.b * effect_state.breath_level) / max_level),
            };
        }
    }

    void renderPatrolFrame()
    {
        const unsigned int led_count = requiredLedCount();
        const unsigned int trail_len = std::max(1u, (normalizedPatrolNum(last_snapshot.patrol_num) * led_count + SteamLedCount / 2) / SteamLedCount);

        effect_state.pixels.assign(led_count, {});

        const auto set_faded_slot = [this, led_count](unsigned int slot, unsigned int distance) {
            if (slot == 0 || slot > led_count)
                return;

            const Rgb8 source = sourceSlotColor(slot, led_count);
            const unsigned int fade = distance * last_snapshot.color_shift;

            effect_state.pixels[slot - 1] = {
                fadeSub(source.r, fade),
                fadeSub(source.g, fade),
                fadeSub(source.b, fade),
            };
        };

        if (!effect_state.patrol_reverse) {
            if (effect_state.patrol_tail == 0) {
                if (effect_state.patrol_hold != 0) {
                    effect_state.patrol_hold--;
                    return;
                }

                effect_state.patrol_hold = effect_state.patrol_hold_reload;
            }

            if (effect_state.patrol_slot <= led_count) {
                set_faded_slot(effect_state.patrol_slot, 0);
                effect_state.patrol_tail = trail_len;

                for (unsigned int distance = 1; distance < trail_len; distance++) {
                    if (distance < effect_state.patrol_slot)
                        set_faded_slot(effect_state.patrol_slot - distance, distance);
                }

                effect_state.patrol_slot++;
                return;
            }

            if (effect_state.patrol_tail != 0)
                effect_state.patrol_tail--;

            for (unsigned int distance = 0; distance < effect_state.patrol_tail; distance++)
                set_faded_slot(led_count - distance, distance);

            if (effect_state.patrol_tail == 0) {
                effect_state.patrol_slot = 0;
                effect_state.patrol_reverse = true;
            }

            return;
        }

        if (effect_state.patrol_slot <= led_count - 1) {
            set_faded_slot(led_count - effect_state.patrol_slot, 0);
            effect_state.patrol_tail = trail_len;

            for (unsigned int distance = 1; distance < trail_len; distance++) {
                if (effect_state.patrol_slot >= distance)
                    set_faded_slot(led_count + distance - effect_state.patrol_slot, distance);
            }

            effect_state.patrol_slot++;
            return;
        }

        if (effect_state.patrol_tail != 0)
            effect_state.patrol_tail--;

        for (unsigned int distance = 0; distance < effect_state.patrol_tail; distance++)
            set_faded_slot(distance + 1, distance);

        if (effect_state.patrol_tail == 0) {
            effect_state.patrol_slot = 1;
            effect_state.patrol_reverse = false;
        }
    }

    void setRainbowSourceSlot(unsigned int slot, Rgb8 color)
    {
        if (slot == 0 || slot > effect_state.rainbow_source.size())
            return;

        effect_state.rainbow_source[slot - 1] = color;
    }

    void advanceRainbowPhase(std::uint8_t next_phase)
    {
        effect_state.rainbow_phase = next_phase;
        effect_state.rainbow_ramp = 1;
        effect_state.rainbow_slot++;
    }

    static std::int16_t signed16(unsigned int value)
    {
        const std::uint16_t word = static_cast<std::uint16_t>(value);
        return word & 0x8000 ? static_cast<std::int16_t>(word - 0x10000) : static_cast<std::int16_t>(word);
    }

    void renderRainbowFrame()
    {
        const unsigned int led_count = requiredLedCount();

        if (effect_state.pixels.size() != led_count)
            effect_state.pixels.assign(led_count, {});
        if (effect_state.rainbow_source.size() != led_count)
            effect_state.rainbow_source.assign(led_count, {});

        if (effect_state.rainbow_phase > 5)
            return;

        if (effect_state.rainbow_slot > led_count) {
            effect_state.pixels = effect_state.rainbow_source;

            for (unsigned int i = 1; i < effect_state.rainbow_source.size(); i++)
                effect_state.rainbow_source[i - 1] = effect_state.rainbow_source[i];

            effect_state.rainbow_slot = led_count;
            return;
        }

        const std::int16_t ramp = signed16(effect_state.rainbow_ramp * last_snapshot.color_shift);
        const std::uint8_t ramp_byte = static_cast<std::uint8_t>(ramp);

        switch (effect_state.rainbow_phase) {
        case 0:
            if (ramp > 254) {
                advanceRainbowPhase(1);
                return;
            }
            setRainbowSourceSlot(effect_state.rainbow_slot, { 255, ramp_byte, 0 });
            break;

        case 1: {
            const std::int16_t value = static_cast<std::int16_t>(255 - ramp);
            if (value <= 0) {
                advanceRainbowPhase(2);
                return;
            }
            setRainbowSourceSlot(effect_state.rainbow_slot, { static_cast<std::uint8_t>(value), 255, 0 });
            break;
        }

        case 2:
            if (ramp > 254) {
                advanceRainbowPhase(3);
                return;
            }
            setRainbowSourceSlot(effect_state.rainbow_slot, { 0, 255, ramp_byte });
            break;

        case 3: {
            const std::int16_t value = static_cast<std::int16_t>(255 - ramp);
            if (value <= 0) {
                advanceRainbowPhase(4);
                return;
            }
            setRainbowSourceSlot(effect_state.rainbow_slot, { 0, static_cast<std::uint8_t>(value), 255 });
            break;
        }

        case 4:
            if (ramp > 254) {
                advanceRainbowPhase(5);
                return;
            }
            setRainbowSourceSlot(effect_state.rainbow_slot, { ramp_byte, 0, 255 });
            break;

        default: {
            const std::int16_t value = static_cast<std::int16_t>(255 - ramp);
            if (value <= 0) {
                advanceRainbowPhase(0);
                return;
            }
            setRainbowSourceSlot(effect_state.rainbow_slot, { 255, 0, static_cast<std::uint8_t>(value) });
            break;
        }
        }

        effect_state.rainbow_ramp++;
        effect_state.rainbow_slot++;
    }

    void renderEffectFrame(bool advance_animation)
    {
        const unsigned int led_count = requiredLedCount();

        if (effect_state.led_count != led_count)
            resetEffectState();

        if (!last_snapshot.enabled ||
            last_snapshot.effect == ValveLedsEffectOff ||
            !isSupportedEffect(last_snapshot.effect)) {
            effect_state.pixels.assign(led_count, {});
            return;
        }

        switch (last_snapshot.effect) {
        case ValveLedsEffectManual:
        case ValveLedsEffectNormal:
            renderManualOrNormal();
            break;

        case ValveLedsEffectBreath:
            if (advance_animation)
                renderBreathFrame();
            break;

        case ValveLedsEffectPatrol:
            if (advance_animation)
                renderPatrolFrame();
            break;

        case ValveLedsEffectRainbow:
            if (advance_animation) {
                const unsigned int frames = effect_state.rainbow_slot == 1 ? led_count + 1 : 1;

                for (unsigned int frame = 0; frame < frames; frame++)
                    renderRainbowFrame();
            }
            break;

        default:
            effect_state.pixels.assign(led_count, {});
            break;
        }
    }

    bool resolveConfiguredTarget(TargetOption& resolved)
    {
        if (target_label.isEmpty())
            return false;

        for (const TargetOption& target : enumerateTargets()) {
            if (target.label == target_label) {
                resolved = target;
                return true;
            }
        }

        return false;
    }

    void restoreControllerState()
    {
        if (!controlled_controller)
            return;

        const std::size_t color_count = std::min(controlled_controller->colors.size(), original_colors.size());
        std::copy_n(original_colors.begin(), color_count, controlled_controller->colors.begin());

        if (original_mode >= 0 && original_mode < static_cast<int>(controlled_controller->modes.size())) {
            controlled_controller->active_mode = original_mode;
            controlled_controller->DeviceUpdateMode();
        }

        controlled_controller = nullptr;
        original_colors.clear();
    }

    void syncControlledController()
    {
        TargetOption target;
        RGBController* next_controller = resolveConfiguredTarget(target) ? target.controller : nullptr;

        if (next_controller == controlled_controller)
            return;

        restoreControllerState();

        if (next_controller) {
            controlled_controller = next_controller;
            original_mode = next_controller->active_mode;
            original_colors = next_controller->colors;
        }
    }

    void applyLastSnapshot(bool advance_animation, bool check_mode)
    {
        if (!have_snapshot || !sink_active)
            return;

        TargetOption target;
        if (!resolveConfiguredTarget(target))
            return;

        syncControlledController();

        const unsigned int led_count = requiredLedCount();
        const unsigned int target_start = target.start_index + start_led;
        if (!target.controller || target_start + led_count > target.controller->colors.size())
            return;

        if (check_mode) {
            const int previous_mode = target.controller->active_mode;
            target.controller->SetCustomMode();

            if (force_direct_mode || target.controller->active_mode != previous_mode) {
                target.controller->DeviceUpdateMode();
                force_direct_mode = false;
            }
        }

        renderEffectFrame(advance_animation);

        for (unsigned int real_led = 0; real_led < led_count; real_led++) {
            const unsigned int mapped_led = reverse
                ? target_start + (led_count - 1 - real_led)
                : target_start + real_led;

            const Rgb8 color = real_led < effect_state.pixels.size()
                ? scaledColor(effect_state.pixels[real_led], last_snapshot.brightness_scale)
                : Rgb8 {};

            target.controller->SetLED(mapped_led, toOpenRgbColor(color));
        }

        target.controller->UpdateLEDs();

        if (release_after_black) {
            release_after_black = false;
            sink_active = false;
        }
    }

    ResourceManagerInterface* resource_manager = nullptr;
    SteamStateSource* state_source = nullptr;
    PowerStateSource* power_state_source = nullptr;
    QTimer* effect_timer = nullptr;
    std::function<void()> status_callback;
    QString status_text = "Waiting for Steam LED state.";
    QString target_label;
    unsigned int start_led = 0;
    unsigned int real_led_count = SteamLedCount;
    bool reverse = false;
    bool device_detection_complete = false;
    bool have_snapshot = false;
    bool sink_active = false;
    bool release_after_black = false;
    bool force_direct_mode = false;
    RGBController* controlled_controller = nullptr;
    int original_mode = 0;
    std::vector<RGBColor> original_colors;
    EffectState effect_state;
    ValveLedsSnapshot last_snapshot = {};
};

class SteamSinkSettingsWidget : public QWidget
{
public:
    explicit SteamSinkSettingsWidget(SteamSinkRuntime* runtime_ptr, QWidget* parent = nullptr)
        : QWidget(parent)
        , runtime(runtime_ptr)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(10);

        status_label = new QLabel(this);
        root->addWidget(status_label);

        device_status_label = new QLabel(this);
        root->addWidget(device_status_label);

        auto* form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        root->addLayout(form);

        real_led_count = new QSpinBox(this);
        real_led_count->setMinimum(static_cast<int>(MinRealLedCount));
        real_led_count->setMaximum(static_cast<int>(MaxRealLedCount));
        real_led_count->setValue(runtime ? static_cast<int>(runtime->configuredRealLedCount()) : static_cast<int>(MinRealLedCount));
        form->addRow("Real LED Count", real_led_count);

        fractional_label = new QLabel("Fractional scaling is used!", this);
        form->addRow("", fractional_label);

        auto* refreshing_combo = new RefreshingComboBox(this);
        refreshing_combo->setPlaceholderText("Choose target");
        refreshing_combo->refresh = [this]() {
            populateTargets();
        };
        target_combo = refreshing_combo;
        form->addRow("Target", target_combo);

        start_led = new QSpinBox(this);
        start_led->setMinimum(0);
        form->addRow("Start LED", start_led);

        reverse = new QCheckBox("Reverse LED order", this);
        form->addRow("", reverse);

        preview_label = new QLabel(this);
        root->addWidget(preview_label);
        root->addStretch();

        connect(real_led_count, qOverload<int>(&QSpinBox::valueChanged), this, [this]() {
            if (updating_ui || !runtime)
                return;

            runtime->setRealLedCount(static_cast<unsigned int>(real_led_count->value()));
            populateTargets();
        });
        connect(target_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
            updateStartLedRange(index);
            saveCurrentMapping();
        });
        connect(start_led, qOverload<int>(&QSpinBox::valueChanged), this, [this]() {
            updatePreview();
            saveCurrentMapping();
        });
        connect(reverse, &QCheckBox::toggled, this, [this]() {
            updatePreview();
            saveCurrentMapping();
        });

        if (runtime) {
            runtime->setStatusCallback([this]() {
                updateRuntimeStatus();
            });
        }

        populateTargets();
        updateRuntimeStatus();
    }

    ~SteamSinkSettingsWidget() override
    {
        if (runtime)
            runtime->setStatusCallback(nullptr);
    }

    void detachRuntime()
    {
        if (runtime)
            runtime->setStatusCallback(nullptr);

        runtime = nullptr;
        updateRuntimeStatus();
    }

protected:
    void showEvent(QShowEvent* event) override
    {
        QWidget::showEvent(event);
        populateTargets();
    }

private:
    void populateTargets()
    {
        const QString selected_label = target_combo->currentText();
        const QString configured_label = runtime ? runtime->configuredTarget() : QString();
        const unsigned int required_leds = runtime ? runtime->requiredLedCount() : SteamLedCount;

        updating_ui = true;
        if (runtime)
            real_led_count->setValue(static_cast<int>(runtime->configuredRealLedCount()));

        target_combo->clear();
        targets = runtime ? runtime->enumerateTargets() : std::vector<TargetOption>();

        for (const TargetOption& target : targets)
            target_combo->addItem(target.label);

        const bool has_targets = !targets.empty();
        target_combo->setEnabled(has_targets);

        if (has_targets) {
            status_label->setText(QString("Found %1 target%2 with at least %3 LEDs.")
                                      .arg(targets.size())
                                      .arg(targets.size() == 1 ? "" : "s")
                                      .arg(required_leds));

            int index = target_combo->findText(selected_label);
            if (index < 0 && !configured_label.isEmpty())
                index = target_combo->findText(configured_label);

            target_combo->setCurrentIndex(index);
            if (runtime) {
                start_led->setValue(static_cast<int>(runtime->configuredStartLed()));
                reverse->setChecked(runtime->configuredReverse());
            }
        } else {
            status_label->setText(QString("No OpenRGB devices, zones, or segments have at least %1 LEDs.")
                                      .arg(required_leds));
            target_combo->setCurrentIndex(-1);
        }

        updating_ui = false;
        updateStartLedRange(target_combo->currentIndex());
        updateFractionalLabel();
        updatePreview();
    }

    void updateRuntimeStatus()
    {
        device_status_label->setText(runtime ? runtime->statusText() : "Steam sink runtime is unavailable.");
    }

    void updateStartLedRange(int index)
    {
        if (index < 0 || index >= static_cast<int>(targets.size())) {
            start_led->setRange(0, 0);
            start_led->setEnabled(false);
            reverse->setEnabled(false);
            preview_label->clear();
            return;
        }

        const TargetOption& target = targets[index];
        const unsigned int required_leds = runtime ? runtime->requiredLedCount() : SteamLedCount;
        const int max_start = static_cast<int>(target.led_count - required_leds);

        start_led->setEnabled(max_start > 0);
        reverse->setEnabled(true);
        start_led->setRange(0, max_start);
        if (start_led->value() > max_start)
            start_led->setValue(0);
        updatePreview();
    }

    void updatePreview()
    {
        const int index = target_combo->currentIndex();

        if (index < 0 || index >= static_cast<int>(targets.size())) {
            preview_label->clear();
            return;
        }

        const unsigned int required_leds = runtime ? runtime->requiredLedCount() : SteamLedCount;

        preview_label->setText(QString("Steam LEDs 0-%1 map to target LEDs %2-%3%4.")
                                   .arg(SteamLedCount - 1)
                                   .arg(start_led->value())
                                   .arg(start_led->value() + static_cast<int>(required_leds) - 1)
                                   .arg(reverse->isChecked() ? " in reverse order" : ""));
    }

    void updateFractionalLabel()
    {
        fractional_label->setVisible(runtime && runtime->usesFractionalScaling());
    }

    void saveCurrentMapping()
    {
        if (updating_ui || !runtime)
            return;

        const int index = target_combo->currentIndex();
        if (index < 0 || index >= static_cast<int>(targets.size()))
            return;

        runtime->setMapping(targets[index].label,
                            static_cast<unsigned int>(start_led->value()),
                            reverse->isChecked());
    }

    SteamSinkRuntime* runtime = nullptr;
    QSpinBox* real_led_count = nullptr;
    QComboBox* target_combo = nullptr;
    QSpinBox* start_led = nullptr;
    QCheckBox* reverse = nullptr;
    QLabel* status_label = nullptr;
    QLabel* device_status_label = nullptr;
    QLabel* fractional_label = nullptr;
    QLabel* preview_label = nullptr;
    bool updating_ui = false;
    std::vector<TargetOption> targets;
};
}

OpenRGBPluginInfo OpenRGBSteamSinkPlugin::GetPluginInfo()
{
    OpenRGBPluginInfo info;

    info.Name = PluginName;
    info.Description = "Receives Steam front light bar state";
    info.Version = "0.1.0";
    info.Commit = "";
    info.URL = "https://github.com/anna-oake/openrgb-steam-sink";
    info.Location = OPENRGB_PLUGIN_LOCATION_TOP;
    info.Label = "Steam Sink";

    return info;
}

unsigned int OpenRGBSteamSinkPlugin::GetPluginAPIVersion()
{
    return OPENRGB_PLUGIN_API_VERSION;
}

void OpenRGBSteamSinkPlugin::Load(ResourceManagerInterface* resource_manager_ptr)
{
    resource_manager = resource_manager_ptr;
    runtime = new SteamSinkRuntime(resource_manager, this);
}

QWidget* OpenRGBSteamSinkPlugin::GetWidget()
{
    if (!widget)
        widget = new SteamSinkSettingsWidget(static_cast<SteamSinkRuntime*>(runtime));

    return widget;
}

QMenu* OpenRGBSteamSinkPlugin::GetTrayMenu()
{
    return nullptr;
}

void OpenRGBSteamSinkPlugin::Unload()
{
    if (widget)
        static_cast<SteamSinkSettingsWidget*>(widget)->detachRuntime();

    delete runtime;
    runtime = nullptr;
    widget = nullptr;
    resource_manager = nullptr;
}


// Tys#;!!!!!!!!!!!
