// StackChan body sensor drivers + polling task.
//
// Si12T bring-up, register use and the channel order follow M5Stack's
// StackChan firmware (https://github.com/m5stack/StackChan, firmware/main/hal/
// drivers/Si12T/Si12T.cpp and hal/hal_head_touch.cpp); the reference reset
// follows M5Stack's StackChan-BSP (src/drivers/Si12T/Si12T.cpp). Both are
// Copyright (c) 2026 M5Stack Technology CO LTD, MIT License. The code below is
// a re-implementation on the ESP-IDF i2c_master API, not a copy.
//
// LTR-553 and BMI270 are written from their datasheets; the LTR-553 setup was
// cross-checked against M5Stack's M5CoreS3 LTR5XX driver (MIT). The BMI270
// configuration blob is Bosch's (BSD-3-Clause, see bmi270_config.inc).

#include "pet_sensors.h"

#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>

#define TAG "PetSensors"

using eidolon::pet::HeadTouchSample;
using eidolon::pet::ImuSample;
using eidolon::pet::LightSample;

namespace {
#include "bmi270_config.inc"
static_assert(sizeof(kBmi270ConfigFile) == 8192, "BMI270 config blob must be 8 KB");

// Task cadence: one 25 ms tick for the touch pad (a quick double tap lifts
// the finger for well under 100 ms); slower reads are dividers of it.
constexpr uint32_t kTickMs = 25;
constexpr uint32_t kImuEvery = 2;        // 20 Hz; the FIFO keeps every sample anyway
constexpr uint32_t kProximityEvery = 4;  // 10 Hz
constexpr uint32_t kLuxEvery = 20;       // 2 Hz
constexpr uint32_t kDiagnosticsMs = 30000;
constexpr uint32_t kTaskStack = 4096;
// Above the network/crypto tasks: this task holds the shared I2C bus lock during
// each transfer, and if it is starved mid-transfer (a DTLS handshake keeps core 1
// busy for 100+ ms) every other bus user times out behind it. It blocks for the
// whole transfer and uses microseconds of CPU per 25 ms tick.
constexpr UBaseType_t kTaskPriority = 10;
constexpr BaseType_t kTaskCore = 1;

// I2C: short timeouts so a wedged device never stalls the bus users behind us;
// after a run of failures the device is backed off and re-initialised.
constexpr int kI2cTimeoutMs = 20;
constexpr uint8_t kFailuresBeforeBackoff = 5;
constexpr uint32_t kBackoffMinMs = 500;
constexpr uint32_t kBackoffMaxMs = 10000;
constexpr uint32_t kRecalibrateMinIntervalMs = 10000;

// Si12T (0x68). IRQ is not wired on StackChan: polled only.
constexpr uint8_t kSi12tAddr = 0x68;
constexpr uint32_t kSi12tSclHz = 100000;
constexpr uint8_t kSi12tSens1 = 0x02;  // 0x02..0x06: one byte per channel pair
constexpr uint8_t kSi12tCtrl1 = 0x08;
constexpr uint8_t kSi12tCtrl2 = 0x09;
constexpr uint8_t kSi12tRefRst1 = 0x0A;
constexpr uint8_t kSi12tRefRst2 = 0x0B;
constexpr uint8_t kSi12tChHold1 = 0x0C;
constexpr uint8_t kSi12tChHold2 = 0x0D;
constexpr uint8_t kSi12tCalHold1 = 0x0E;
constexpr uint8_t kSi12tCalHold2 = 0x0F;
constexpr uint8_t kSi12tOutput1 = 0x10;  // 2 bits per channel, channel 1 in bits 1:0
constexpr uint8_t kSi12tSensitivity = 0x33;  // type low, level 3 (factory setting)

// LTR-553ALS (0x23).
constexpr uint8_t kLtrAddr = 0x23;
constexpr uint32_t kLtrSclHz = 400000;
constexpr uint8_t kLtrAlsContr = 0x80;
constexpr uint8_t kLtrPsContr = 0x81;
constexpr uint8_t kLtrPsLed = 0x82;
constexpr uint8_t kLtrPsNPulses = 0x83;
constexpr uint8_t kLtrPsMeasRate = 0x84;
constexpr uint8_t kLtrAlsMeasRate = 0x85;
constexpr uint8_t kLtrPartId = 0x86;
constexpr uint8_t kLtrAlsDataCh1 = 0x88;  // CH1 lo/hi, then CH0 lo/hi; read in this order
constexpr uint8_t kLtrPsData = 0x8D;      // lo, then hi (bits 2:0)
constexpr uint8_t kLtrExpectedPartId = 0x92;
constexpr float kLtrAlsGain = 8.0f;         // ALS_CONTR gain bits 011
constexpr float kLtrAlsIntegration = 1.0f;  // 100 ms, in units of 100 ms

// BMI270 (0x69 on CoreS3).
constexpr uint8_t kBmiAddr = 0x69;
constexpr uint32_t kBmiSclHz = 400000;
constexpr uint8_t kBmiChipId = 0x00;
constexpr uint8_t kBmiInternalStatus = 0x21;
constexpr uint8_t kBmiFifoLength0 = 0x24;
constexpr uint8_t kBmiFifoData = 0x26;
constexpr uint8_t kBmiAccConf = 0x40;
constexpr uint8_t kBmiAccRange = 0x41;
constexpr uint8_t kBmiFifoConfig1 = 0x49;
constexpr uint8_t kBmiInitCtrl = 0x59;
constexpr uint8_t kBmiInitAddr0 = 0x5B;
constexpr uint8_t kBmiInitData = 0x5E;
constexpr uint8_t kBmiPwrConf = 0x7C;
constexpr uint8_t kBmiPwrCtrl = 0x7D;
constexpr uint8_t kBmiCmd = 0x7E;
constexpr uint8_t kBmiExpectedChipId = 0x24;
constexpr uint8_t kBmiCmdSoftReset = 0xB6;
constexpr uint8_t kBmiCmdFifoFlush = 0xB0;
constexpr uint8_t kBmiAccConfValue = 0xA8;   // filter_perf=1, bwp=normal, ODR 100 Hz
constexpr uint8_t kBmiAccRange4g = 0x01;
constexpr uint8_t kBmiFifoAccHeaderless = 0x40;
constexpr size_t kBmiConfigChunk = 64;       // even, well inside one I2C transaction
constexpr float kBmiLsbPerG = 8192.0f;       // +-4 g, 16 bit
constexpr uint32_t kBmiOdrPeriodMs = 10;
constexpr size_t kBmiFrameBytes = 6;

uint64_t NowMs() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

// pdMS_TO_TICKS rounds down (to 0 at 100 Hz for short waits) and a one-tick
// delay may end almost at once: round up, then add the partial tick.
void DelayAtLeastMs(uint32_t ms)
{
    const uint64_t ticks = (static_cast<uint64_t>(ms) * configTICK_RATE_HZ + 999) / 1000;
    vTaskDelay(static_cast<TickType_t>(ticks) + 1);
}

// LTR-553 appendix formula: piecewise on the IR ratio CH1/(CH0+CH1).
// No window factor: the CoreS3 glass attenuates, so absolute lux reads low.
float Ltr553Lux(uint16_t ch0, uint16_t ch1)
{
    const float c0 = ch0, c1 = ch1;
    const float sum = c0 + c1;
    if (sum <= 0) return 0;
    const float ratio = c1 / sum;
    float lux = 0;
    if (ratio < 0.45f) lux = 1.7743f * c0 + 1.1059f * c1;
    else if (ratio < 0.64f) lux = 4.2785f * c0 - 1.9548f * c1;
    else if (ratio < 0.85f) lux = 0.5926f * c0 + 0.1185f * c1;
    return std::max(lux, 0.0f) / kLtrAlsGain / kLtrAlsIntegration;
}

int16_t Le16(const uint8_t* p) { return static_cast<int16_t>(static_cast<uint16_t>(p[0] | (p[1] << 8))); }
}  // namespace

StackChanSensors::StackChanSensors()
{
    portMUX_INITIALIZE(&lock_);
    touch_.name = "Si12T";
    touch_.addr = kSi12tAddr;
    touch_.scl_hz = kSi12tSclHz;
    touch_.configure = &StackChanSensors::ConfigureSi12t;
    light_.name = "LTR-553";
    light_.addr = kLtrAddr;
    light_.scl_hz = kLtrSclHz;
    light_.configure = &StackChanSensors::ConfigureLtr553;
    imu_.name = "BMI270";
    imu_.addr = kBmiAddr;
    imu_.scl_hz = kBmiSclHz;
    imu_.configure = &StackChanSensors::ConfigureBmi270;
}

bool StackChanSensors::Init(i2c_master_bus_handle_t bus)
{
    bus_ = bus;
    for (Device* d : {&touch_, &light_, &imu_}) {
        if (!AddDevice(*d)) {
            ESP_LOGW(TAG, "%s not found at 0x%02x, skipped", d->name, d->addr);
            continue;
        }
        if (!(this->*(d->configure))()) {
            ESP_LOGW(TAG, "%s at 0x%02x did not configure, skipped", d->name, d->addr);
            RemoveDevice(*d);
            continue;
        }
        ESP_LOGI(TAG, "%s ready at 0x%02x", d->name, d->addr);
    }
    return has_touch() || has_imu() || has_light();
}

void StackChanSensors::Start(Sink sink)
{
    if (task_ != nullptr) return;
    if (!has_touch() && !has_imu() && !has_light()) {
        ESP_LOGW(TAG, "no body sensors, polling task not started");
        return;
    }
    sink_ = std::move(sink);
    events_.reserve(32);
    if (xTaskCreatePinnedToCore([](void* arg) { static_cast<StackChanSensors*>(arg)->Run(); }, "pet_sensors",
                                kTaskStack, this, kTaskPriority, &task_, kTaskCore) != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "failed to start polling task");
    }
}

void StackChanSensors::SuppressMotionArtifacts(uint64_t until_ms)
{
    portENTER_CRITICAL(&lock_);
    pending_suppress_ = std::max(pending_suppress_, until_ms);
    portEXIT_CRITICAL(&lock_);
}

void StackChanSensors::IgnoreLightUntil(uint64_t until_ms)
{
    portENTER_CRITICAL(&lock_);
    pending_ignore_ = std::max(pending_ignore_, until_ms);
    portEXIT_CRITICAL(&lock_);
}

// ---- I2C plumbing -----------------------------------------------------------

bool StackChanSensors::AddDevice(Device& d)
{
    if (i2c_master_probe(bus_, d.addr, kI2cTimeoutMs) != ESP_OK) return false;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = d.addr;
    cfg.scl_speed_hz = d.scl_hz;
    if (i2c_master_bus_add_device(bus_, &cfg, &d.dev) != ESP_OK) {
        d.dev = nullptr;
        return false;
    }
    return true;
}

void StackChanSensors::RemoveDevice(Device& d)
{
    if (d.dev != nullptr) i2c_master_bus_rm_device(d.dev);
    d.dev = nullptr;
}

bool StackChanSensors::Write(Device& d, const uint8_t* data, size_t len)
{
    return i2c_master_transmit(d.dev, data, len, kI2cTimeoutMs) == ESP_OK;
}

bool StackChanSensors::WriteReg(Device& d, uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return Write(d, buf, sizeof(buf));
}

bool StackChanSensors::ReadRegs(Device& d, uint8_t reg, uint8_t* out, size_t len)
{
    return i2c_master_transmit_receive(d.dev, &reg, 1, out, len, kI2cTimeoutMs) == ESP_OK;
}

// ---- Device bring-up --------------------------------------------------------

bool StackChanSensors::ConfigureSi12t()
{
    // Factory si12t_setup() order: channels + reference calibration enabled,
    // software reset, CTRL1, sensitivity type low / level 3 everywhere. The
    // Si12T is register-compatible with ADSemiconductor's TSM12 (spec v2.2
    // §8.2.2-8.2.3), whose map the factory values decode against.
    // The factory left the chip low-power: CTRL2 sleep on (0x0F then 0x07) and
    // CTRL1 0x22 = auto fast/slow scan, response 2+2 periods; the spec warns
    // sleep makes the response longer. On the head that merged quick taps:
    // 11-16, three deliberate double taps read as one 250-300 ms press each,
    // five fast pats as one 1.15 s "hold", while single taps were clean. So
    // sleep off (CTRL2 0x0B then 0x03) and fast scan only (CTRL1 0xA2: MS=1,
    // FTC and the 4-period response kept). Costs tens of microamps.
    static const uint8_t kSequence[][2] = {
        {kSi12tRefRst1, 0x00}, {kSi12tRefRst2, 0x00}, {kSi12tChHold1, 0x00}, {kSi12tChHold2, 0x00},
        {kSi12tCalHold1, 0x00}, {kSi12tCalHold2, 0x00}, {kSi12tCtrl2, 0x0B}, {kSi12tCtrl2, 0x03},
        {kSi12tCtrl1, 0xA2},
        {kSi12tSens1, kSi12tSensitivity}, {kSi12tSens1 + 1, kSi12tSensitivity}, {kSi12tSens1 + 2, kSi12tSensitivity},
        {kSi12tSens1 + 3, kSi12tSensitivity}, {kSi12tSens1 + 4, kSi12tSensitivity},
    };
    for (const auto& w : kSequence) {
        if (!WriteReg(touch_, w[0], w[1])) return false;
    }
    uint8_t sens = 0;
    if (!ReadRegs(touch_, kSi12tSens1, &sens, 1)) return false;
    if (sens != kSi12tSensitivity) ESP_LOGW(TAG, "Si12T sensitivity reads 0x%02x, wrote 0x%02x", sens, kSi12tSensitivity);
    return true;
}

bool StackChanSensors::ResetSi12tReference()
{
    // Pulsing REF_RST forces each channel's baseline to the current reading
    // (StackChan-BSP SI12T_Reset_Reference). Only safe with no hand on the pad,
    // which is why it runs only for the "all zones stuck at 3" case.
    return WriteReg(touch_, kSi12tRefRst1, 0xFF) && WriteReg(touch_, kSi12tRefRst2, 0x0F) &&
           WriteReg(touch_, kSi12tRefRst1, 0x00) && WriteReg(touch_, kSi12tRefRst2, 0x00);
}

bool StackChanSensors::ConfigureLtr553()
{
    uint8_t id = 0;
    if (!ReadRegs(light_, kLtrPartId, &id, 1)) return false;
    if (id != kLtrExpectedPartId) {
        ESP_LOGW(TAG, "LTR-553 PART_ID 0x%02x, expected 0x%02x", id, kLtrExpectedPartId);
        if ((id >> 4) != (kLtrExpectedPartId >> 4)) return false;  // not an LTR-55x at all
    }
    static const uint8_t kSequence[][2] = {
        {kLtrPsLed, 0x7F},        // 60 kHz, 100 % duty, 100 mA (datasheet default)
        {kLtrPsNPulses, 0x01},    // 1 pulse (default); thresholds assume this
        {kLtrPsMeasRate, 0x00},   // 50 ms: the 10 Hz poll always sees a fresh value
        {kLtrAlsMeasRate, 0x03},  // 100 ms integration, 500 ms repeat (2 Hz poll)
        {kLtrAlsContr, 0x0D},     // gain 8x (0.125..8k lux), active
        {kLtrPsContr, 0x02},      // proximity active
    };
    for (const auto& w : kSequence) {
        if (!WriteReg(light_, w[0], w[1])) return false;
    }
    return true;
}

bool StackChanSensors::ConfigureBmi270()
{
    uint8_t id = 0;
    if (!ReadRegs(imu_, kBmiChipId, &id, 1)) return false;
    if (id != kBmiExpectedChipId) {
        ESP_LOGW(TAG, "BMI270 CHIP_ID 0x%02x, expected 0x%02x", id, kBmiExpectedChipId);
        return false;
    }
    if (!WriteReg(imu_, kBmiCmd, kBmiCmdSoftReset)) return false;
    DelayAtLeastMs(2);
    // Advanced power save must be off for the burst upload.
    if (!WriteReg(imu_, kBmiPwrConf, 0x00)) return false;
    esp_rom_delay_us(500);
    if (!WriteReg(imu_, kBmiInitCtrl, 0x00)) return false;

    // INIT_ADDR is in 16-bit words: low 4 bits in INIT_ADDR_0, the rest in
    // INIT_ADDR_1 (Bosch bmi2.c set_config_file). A chunk that fails is
    // retried, since the bus is shared with the codec and touch screen.
    for (size_t off = 0; off < sizeof(kBmi270ConfigFile); off += kBmiConfigChunk) {
        const size_t n = std::min(kBmiConfigChunk, sizeof(kBmi270ConfigFile) - off);
        const uint8_t addr[3] = {kBmiInitAddr0, static_cast<uint8_t>((off / 2) & 0x0F),
                                 static_cast<uint8_t>((off / 2) >> 4)};
        uint8_t chunk[1 + kBmiConfigChunk];
        chunk[0] = kBmiInitData;
        std::memcpy(chunk + 1, kBmi270ConfigFile + off, n);
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            ok = Write(imu_, addr, sizeof(addr)) && Write(imu_, chunk, n + 1);
        }
        if (!ok) {
            ESP_LOGW(TAG, "BMI270 config upload failed at byte %u", static_cast<unsigned>(off));
            return false;
        }
    }
    if (!WriteReg(imu_, kBmiInitCtrl, 0x01)) return false;

    // The feature engine reports "init ok" (message 0x1) within ~20 ms.
    uint8_t status = 0;
    bool initialised = false;
    for (int i = 0; i < 15 && !initialised; ++i) {
        DelayAtLeastMs(10);
        initialised = ReadRegs(imu_, kBmiInternalStatus, &status, 1) && (status & 0x0F) == 0x01;
    }
    if (!initialised) {
        ESP_LOGW(TAG, "BMI270 INTERNAL_STATUS 0x%02x after config load", status);
        return false;
    }

    // Accelerometer only, 100 Hz normal mode, +-4 g, into the FIFO without
    // frame headers; then leave power save off so the FIFO keeps streaming.
    static const uint8_t kSequence[][2] = {
        {kBmiPwrCtrl, 0x04},
        {kBmiAccConf, kBmiAccConfValue},
        {kBmiAccRange, kBmiAccRange4g},
        {kBmiFifoConfig1, kBmiFifoAccHeaderless},
        {kBmiPwrConf, 0x02},  // fifo_self_wakeup, adv_power_save off
        {kBmiCmd, kBmiCmdFifoFlush},
    };
    for (const auto& w : kSequence) {
        if (!WriteReg(imu_, w[0], w[1])) return false;
    }
    uint8_t acc_conf = 0;
    if (ReadRegs(imu_, kBmiAccConf, &acc_conf, 1) && acc_conf != kBmiAccConfValue) {
        ESP_LOGW(TAG, "BMI270 ACC_CONF reads 0x%02x, wrote 0x%02x", acc_conf, kBmiAccConfValue);
    }
    return true;
}

// ---- Polling ----------------------------------------------------------------

void StackChanSensors::Run()
{
    TickType_t last_wake = xTaskGetTickCount();
    uint64_t last_diagnostics = NowMs();
    for (uint32_t tick = 0;; ++tick) {
        // Behind schedule (a re-init, a long bus wait): restart the cadence
        // instead of firing a burst of catch-up polls.
        if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kTickMs)) == pdFALSE) last_wake = xTaskGetTickCount();
        const uint64_t now = NowMs();
        ApplyWindows();
        events_.clear();

        Poll(touch_, now, [&] { return PollTouch(now); });
        if (tick % kImuEvery == 0) Poll(imu_, now, [&] { return PollImu(now); });
        const bool proximity = tick % kProximityEvery == 0;
        const bool lux = tick % kLuxEvery == 0;
        if (proximity || lux) Poll(light_, now, [&] { return PollLight(now, proximity, lux); });

        if (sink_) {
            for (const auto& e : events_) sink_(e);
        }
        if (now - last_diagnostics >= kDiagnosticsMs) {
            last_diagnostics = now;
            LogDiagnostics();
        }
    }
}

void StackChanSensors::ApplyWindows()
{
    portENTER_CRITICAL(&lock_);
    const uint64_t suppress = pending_suppress_;
    const uint64_t ignore = pending_ignore_;
    portEXIT_CRITICAL(&lock_);
    if (suppress != applied_suppress_) {
        applied_suppress_ = suppress;
        // IMU only. A touch reaction moves the head; suppressing the pad as well
        // would swallow the second tap of every double tap and every rapid pat.
        motion_rec_.SuppressUntil(suppress);
    }
    if (ignore != applied_ignore_) {
        applied_ignore_ = ignore;
        light_rec_.IgnoreUntil(ignore);
    }
}

template <typename ReadFn>
void StackChanSensors::Poll(Device& d, uint64_t now, ReadFn&& read)
{
    if (d.dev == nullptr || now < d.next_try_ms) return;
    if (d.needs_reinit) {
        // After a failure run the chip may have browned out or reset.
        if (!(this->*(d.configure))()) {
            Fail(d, now);
            return;
        }
        d.needs_reinit = false;
    }
    if (read()) {
        Succeed(d);
    } else {
        Fail(d, now);
    }
}

void StackChanSensors::Succeed(Device& d)
{
    if (d.backoff_ms != 0) ESP_LOGI(TAG, "%s recovered", d.name);
    d.failures = 0;
    d.backoff_ms = 0;
}

void StackChanSensors::Fail(Device& d, uint64_t now)
{
    ++d.errors;
    if (d.failures < UINT8_MAX) ++d.failures;
    if (d.failures < kFailuresBeforeBackoff) return;
    const bool first = d.backoff_ms == 0;
    const uint32_t previous = d.backoff_ms;
    d.backoff_ms = first ? kBackoffMinMs : std::min(d.backoff_ms * 2, kBackoffMaxMs);
    d.next_try_ms = now + d.backoff_ms;
    d.needs_reinit = true;
    if (d.backoff_ms != previous) {  // quiet once at the cap; the diagnostics line keeps counting
        ESP_LOGW(TAG, "%s: %u I2C failures in a row, retry in %" PRIu32 " ms", d.name,
                 static_cast<unsigned>(d.failures), d.backoff_ms);
    }
    if (first && &d == &touch_) {
        // No samples for a while: drop the open press rather than leave it held.
        touch_rec_.SuppressUntil(now + 1);
    }
}

bool StackChanSensors::PollTouch(uint64_t now)
{
    uint8_t raw = 0;
    if (!ReadRegs(touch_, kSi12tOutput1, &raw, 1)) return false;
    last_touch_ = raw;
    // Channel 3 (bits 5:4) is the front, by the forehead; channel 1 (bits 1:0)
    // the back. M5Stack's two sources disagree: the factory firmware weights
    // channel 1 as the front, the StackChan-BSP reverses the array
    // (_intensities[2 - i]). The real head sides with the BSP (11-16: taps by
    // the forehead read as the back 10 times out of 11, and a forehead-to-back
    // stroke read as back-to-front).
    HeadTouchSample s;
    s.t_ms = now;
    s.front = (raw >> 4) & 0x03;
    s.middle = (raw >> 2) & 0x03;
    s.back = raw & 0x03;
    touch_rec_.Update(s, events_);
    if (touch_rec_.presses() != logged_presses_) {
        // One line per finished press: how far apart and how firm a hand's
        // touches are is what the tap thresholds are tuned on. At most one
        // press ends per sample.
        logged_presses_ = touch_rec_.presses();
        const eidolon::pet::TouchPressSummary& p = touch_rec_.last_press();
        ESP_LOGI(TAG, "touch press gap=%ld dur=%" PRIu32 " peak f/m/b=%u/%u/%u",
                 p.gap_ms == UINT32_MAX ? -1L : static_cast<long>(p.gap_ms), p.duration_ms,
                 static_cast<unsigned>(p.peak[0]), static_cast<unsigned>(p.peak[1]),
                 static_cast<unsigned>(p.peak[2]));
    }

    if (touch_rec_.NeedsRecalibration() &&
        (last_recal_ms_ == 0 || now - last_recal_ms_ >= kRecalibrateMinIntervalMs)) {
        last_recal_ms_ = now;
        if (ResetSi12tReference()) {
            ++recalibrations_;
            touch_rec_.ClearRecalibration();
            ESP_LOGW(TAG, "Si12T stuck at full on all zones, reference reset (%" PRIu32 ")", recalibrations_);
        }
    }
    return true;
}

bool StackChanSensors::PollImu(uint64_t now)
{
    uint8_t len_raw[2] = {};
    if (!ReadRegs(imu_, kBmiFifoLength0, len_raw, sizeof(len_raw))) return false;
    const size_t len = static_cast<size_t>(len_raw[0]) | (static_cast<size_t>(len_raw[1] & 0x3F) << 8);
    if (len > sizeof(fifo_)) {
        // Far behind (bus stall, re-init): the frames' times are lost anyway.
        ++imu_overflows_;
        return WriteReg(imu_, kBmiCmd, kBmiCmdFifoFlush);
    }
    const size_t frames = len / kBmiFrameBytes;
    if (frames == 0) return true;
    if (!ReadRegs(imu_, kBmiFifoData, fifo_, frames * kBmiFrameBytes)) return false;

    // The newest frame was sampled just before now; earlier ones one ODR
    // period apart. Clamp so times never run backwards across reads.
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* f = fifo_ + i * kBmiFrameBytes;
        // Headerless dummy frame (Bosch: 0x01 0x7F 0x00 0x80 ...): no data.
        if (f[0] == 0x01 && f[1] == 0x7F && f[2] == 0x00 && f[3] == 0x80) continue;
        uint64_t t = now - (frames - 1 - i) * kBmiOdrPeriodMs;
        if (t <= last_imu_t_) t = last_imu_t_ + 1;
        last_imu_t_ = t;
        ImuSample s;
        s.t_ms = t;
        s.ax = Le16(f) / kBmiLsbPerG;
        s.ay = Le16(f + 2) / kBmiLsbPerG;
        s.az = Le16(f + 4) / kBmiLsbPerG;
        motion_rec_.Update(s, events_);
    }
    return true;
}

bool StackChanSensors::PollLight(uint64_t now, bool proximity, bool lux)
{
    LightSample s;
    s.t_ms = now;
    if (proximity) {
        uint8_t b[2] = {};
        if (!ReadRegs(light_, kLtrPsData, b, sizeof(b))) return false;
        s.proximity = static_cast<uint16_t>(b[0] | ((b[1] & 0x07) << 8));
        s.proximity_valid = true;
        last_prox_ = s.proximity;
    }
    if (lux) {
        uint8_t b[4] = {};
        if (!ReadRegs(light_, kLtrAlsDataCh1, b, sizeof(b))) return false;
        const uint16_t ch1 = static_cast<uint16_t>(b[0] | (b[1] << 8));
        const uint16_t ch0 = static_cast<uint16_t>(b[2] | (b[3] << 8));
        s.lux = Ltr553Lux(ch0, ch1);
        s.lux_valid = true;
        last_lux_ = s.lux;
    }
    light_rec_.Update(s, events_);
    return true;
}

bool StackChanSensors::GravityMg(int& x, int& y, int& z) const
{
    if (!has_imu()) return false;
    const auto g = motion_rec_.gravity();
    x = static_cast<int>(std::lround(g.x * 1000.0f));
    y = static_cast<int>(std::lround(g.y * 1000.0f));
    z = static_cast<int>(std::lround(g.z * 1000.0f));
    return x != 0 || y != 0 || z != 0;
}

void StackChanSensors::LogDiagnostics()
{
    // Integers only: keeps float formatting off this task's small stack.
    const int accel_mg = has_imu() ? static_cast<int>(std::lround(motion_rec_.magnitude() * 1000.0f)) : -1;
    int gx = 0, gy = 0, gz = 0;
    GravityMg(gx, gy, gz);
    ESP_LOGI(TAG,
             "touch f/m/b=%u/%u/%u prox=%u lux=%ld |a|=%dmg g=%d/%d/%d held=%d tilted=%d recal=%" PRIu32
             " err touch/imu/light=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " fifo_ovf=%" PRIu32,
             static_cast<unsigned>((last_touch_ >> 4) & 0x03), static_cast<unsigned>((last_touch_ >> 2) & 0x03),
             static_cast<unsigned>(last_touch_ & 0x03), static_cast<unsigned>(last_prox_),
             std::lround(last_lux_), accel_mg, gx, gy, gz, motion_rec_.held() ? 1 : 0, motion_rec_.tilted() ? 1 : 0,
             recalibrations_, touch_.errors, imu_.errors, light_.errors, imu_overflows_);
}
