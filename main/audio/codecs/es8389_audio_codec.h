#ifndef _ES8389_AUDIO_CODEC_H
#define _ES8389_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/i2c.h>
#include <driver/gpio.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <mutex>
#include <vector>

class Es8389AudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* ctrl_if_ = nullptr;
    const audio_codec_if_t* codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    gpio_num_t pa_pin_ = GPIO_NUM_NC;
    // Set when playback feeds both DAC outputs; Write() then has to widen
    // the mono stream to match.
    bool output_stereo_ = false;
    std::vector<int16_t> output_stereo_buffer_;
    std::mutex data_if_mutex_;

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    // use_dac_reference: the ES8389 can add a copy of the DAC output to its
    // capture stream, giving the AFE a real echo reference alongside the mics.
    // The flag tells the rest of the firmware that the stream carries one, so
    // the capture path is built as 1 mic + 1 reference ("MR").
    Es8389AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        gpio_num_t pa_pin, uint8_t es8389_addr, bool use_mclk = true,
        bool use_dac_reference = false,
        // Analog mic gain in dB. 40 saturates a close-mic board driven at test
        // volume; Espressif's own BSP for the S31-Korvo-1 uses 30.
        float input_gain_db = 40.0f);
    virtual ~Es8389AudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif // _ES8389_AUDIO_CODEC_H
