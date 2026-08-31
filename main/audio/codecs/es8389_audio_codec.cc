#include "es8389_audio_codec.h"

#include <esp_log.h>
#include <driver/i2s_tdm.h>

static const char TAG[] = "Es8389AudioCodec";

Es8389AudioCodec::Es8389AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8389_addr, bool use_mclk, bool use_dac_reference,
    float input_gain_db) {
    duplex_ = true; // 是否双工
    input_reference_ = use_dac_reference; // 采集流中带 DAC 回灌的回声参考
    input_channels_ = use_dac_reference ? 2 : 1; // mic(+ref)
    output_stereo_ = use_dac_reference;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = input_gain_db;
    pa_pin_ = pa_pin;
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = i2c_port,
        .addr = es8389_addr,
        .bus_handle = i2c_master_handle,
    };
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8389_codec_cfg_t es8389_cfg = {};
    es8389_cfg.ctrl_if = ctrl_if_;
    es8389_cfg.gpio_if = gpio_if_;
    es8389_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8389_cfg.pa_pin = pa_pin;
    es8389_cfg.use_mclk = use_mclk;
    es8389_cfg.hw_gain.pa_voltage = 5.0;
    es8389_cfg.hw_gain.codec_dac_voltage = 3.3;
    // false keeps the chip's internal ADCL + DACR routing, i.e. the echo
    // reference. Set it only when the second channel must stay a plain input.
    es8389_cfg.no_dac_ref = !use_dac_reference;
    codec_if_ = es8389_codec_new(&es8389_cfg);

    assert(codec_if_ != NULL);

    esp_codec_dev_cfg_t outdev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&outdev_cfg);
    assert(output_dev_ != NULL);

    esp_codec_dev_cfg_t indev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if_,
        .data_if = data_if_,
    };
    input_dev_ = esp_codec_dev_new(&indev_cfg);
    assert(input_dev_ != NULL);
    esp_codec_set_disable_when_closed(output_dev_, false);
    esp_codec_set_disable_when_closed(input_dev_, false);
    ESP_LOGI(TAG, "Es8389AudioCodec initialized");
}

Es8389AudioCodec::~Es8389AudioCodec() {
    ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(codec_if_);
    audio_codec_delete_ctrl_if(ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void Es8389AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef   I2S_HW_VERSION_2    
                .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    if (!input_reference_) {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    }

    if (input_reference_) {
        // Enabling the codec's DAC reference widens its capture frame from two
        // slots to four — ADC left, ADC right, DAC left, DAC right — which the
        // driver reflects by clocking at sample_rate * bits * 4 instead of * 2.
        // A two-slot standard frame cannot carry that, so capture runs TDM here
        // while playback stays standard; esp-box-3 drives its four-channel ES7210
        // the same way. Espressif's board definition for the S31-Korvo-1 names the
        // slot order: [FL, FR, RE, NA] — mic 1, mic 2, reference, unused.
        i2s_tdm_config_t tdm_cfg = {
            .clk_cfg = {
                .sample_rate_hz = (uint32_t)input_sample_rate_,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .ext_clk_freq_hz = 0,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                .bclk_div = 8,
            },
            .slot_cfg = {
                .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_STEREO,
                .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                                 I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
                .ws_width = I2S_TDM_AUTO_WS_WIDTH,
                .ws_pol = false,
                .bit_shift = true,
                .left_align = false,
                .big_endian = false,
                .bit_order_lsb = false,
                .skip_mask = false,
                .total_slot = I2S_TDM_AUTO_SLOT_NUM,
            },
            // Each direction claims only the data pin it uses. The two shared one
            // std_cfg while both ran two slots, which was harmless; once capture
            // moves to TDM they are separate configurations and must not both
            // declare the other's pin. esp-box-3 splits them the same way.
            .gpio_cfg = {
                .mclk = mclk,
                .bclk = bclk,
                .ws = ws,
                .dout = I2S_GPIO_UNUSED,
                .din = din,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };
        ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));

        // Playback runs TDM too. esp_codec_dev's unit test for this codec's
        // reference path (codec_dev_test, boards/esp32s31/test_board_es8389.c)
        // puts both directions in TDM; leaving playback in standard mode let the
        // shared port be rewritten to 32-bit slots to reach the same frame width,
        // which silently corrupted playback while capture looked correct.
        i2s_tdm_config_t tdm_tx_cfg = tdm_cfg;
        tdm_tx_cfg.gpio_cfg.dout = dout;
        tdm_tx_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
        tdm_tx_cfg.clk_cfg.sample_rate_hz = (uint32_t)output_sample_rate_;
        ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(tx_handle_, &tdm_tx_cfg));
    } else {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    }
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex channels created");
}

void Es8389AudioCodec::SetOutputVolume(int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

void Es8389AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        // With the reference on the codec delivers four slots, and the reference
        // is inserted at slot 1 — the runtime layout is FL,RE,FR,NA, not the
        // FL,FR,RE,NA the board declares as its wiring. esp_codec_dev's own test
        // asserts exactly that string. Slots 0 and 1 are therefore the
        // mic-plus-reference pair the AFE consumes; masking 0 and 2 would hand it
        // two microphones, which is indistinguishable from a dead reference.
        // input_channels_ stays 2 because that is what reaches the processor.
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = (uint8_t)(input_reference_ ? 4 : 1),
            .channel_mask = 0,
            .sample_rate = (uint32_t)input_sample_rate_,
            .mclk_multiple = 0,
        };
        if (input_reference_) {
            fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                              ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(input_dev_, input_gain_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    }
    AudioCodec::EnableInput(enable);
}

void Es8389AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        // Both DAC outputs are wired to their own NS4150 power amplifier, so
        // playback claims the pair. Driving slot 0 alone left the other amplifier
        // fed with silence: the reference tap still saw the audio, so the capture
        // side looked healthy while the board stayed quiet.
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = (uint8_t)(output_stereo_ ? 2 : 1),
            .channel_mask = (uint16_t)(output_stereo_ ? (ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                                                         ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1)) : 0),
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(output_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, output_volume_));
        if (pa_pin_ != GPIO_NUM_NC) {
            gpio_set_level(pa_pin_, 1);
        }
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
        if (pa_pin_ != GPIO_NUM_NC) {
            gpio_set_level(pa_pin_, 0);
        }
    }
    AudioCodec::EnableOutput(enable);
}

int Es8389AudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(input_dev_, (void*)dest, samples * sizeof(int16_t)));
    }
    return samples;
}

int Es8389AudioCodec::Write(const int16_t* data, int samples) {
    if (!output_enabled_) {
        return samples;
    }
    if (!output_stereo_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(output_dev_, (void*)data, samples * sizeof(int16_t)));
        return samples;
    }
    // The stream is mono and the frame carries two DAC slots, one per amplifier,
    // so the same samples go to both.
    if ((int)output_stereo_buffer_.size() < samples * 2) {
        output_stereo_buffer_.resize(samples * 2);
    }
    for (int i = 0; i < samples; i++) {
        output_stereo_buffer_[i * 2] = data[i];
        output_stereo_buffer_[i * 2 + 1] = data[i];
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(
        output_dev_, output_stereo_buffer_.data(), samples * 2 * sizeof(int16_t)));
    return samples;
}