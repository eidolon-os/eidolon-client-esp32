// LiveKit media board bring-up for Waveshare ESP32-S3-Touch-AMOLED-2.06 (ES8311 + ES7210).

#include "livekit_board.h"

#include "board.h"

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <driver/i2s_tdm.h>
#include <esp_audio_dec_default.h>
#include <esp_audio_enc_default.h>
#include <esp_capture_defaults.h>
#include <esp_capture_sink.h>
#include <esp_check.h>
#include <esp_codec_dev_defaults.h>
#include <esp_log.h>
#include <av_render_default.h>

#include "config.h"

#define TAG "EidolonLKBoard"

#define LIVEKIT_I2S_SAMPLE_RATE 16000
#define LIVEKIT_SPEAKER_VOLUME CONFIG_EIDOLON_LIVEKIT_SPEAKER_VOLUME

static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static esp_codec_dev_handle_t s_play_dev;
static esp_codec_dev_handle_t s_rec_dev;

static esp_capture_sink_handle_t s_capturer;
static esp_capture_audio_src_if_t* s_audio_source;
static audio_render_handle_t s_audio_renderer;
static av_render_handle_t s_av_renderer;

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg =
            {
                .sample_rate_hz = LIVEKIT_I2S_SAMPLE_RATE,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .ext_clk_freq_hz = 0,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            },
        .slot_cfg =
            {
                .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_STEREO,
                .slot_mask = I2S_STD_SLOT_BOTH,
                .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
                .ws_pol = false,
                .bit_shift = true,
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false,
            },
        .gpio_cfg =
            {
                .mclk = AUDIO_I2S_GPIO_MCLK,
                .bclk = AUDIO_I2S_GPIO_BCLK,
                .ws = AUDIO_I2S_GPIO_WS,
                .dout = AUDIO_I2S_GPIO_DOUT,
                .din = I2S_GPIO_UNUSED,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "i2s tx std");

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg =
            {
                .sample_rate_hz = LIVEKIT_I2S_SAMPLE_RATE,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .ext_clk_freq_hz = 0,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                .bclk_div = 8,
            },
        .slot_cfg =
            {
                .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_STEREO,
                .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 |
                                                 I2S_TDM_SLOT3),
                .ws_width = I2S_TDM_AUTO_WS_WIDTH,
                .ws_pol = false,
                .bit_shift = true,
                .left_align = false,
                .big_endian = false,
                .bit_order_lsb = false,
                .skip_mask = false,
                .total_slot = I2S_TDM_AUTO_SLOT_NUM,
            },
        .gpio_cfg =
            {
                .mclk = AUDIO_I2S_GPIO_MCLK,
                .bclk = AUDIO_I2S_GPIO_BCLK,
                .ws = AUDIO_I2S_GPIO_WS,
                .dout = I2S_GPIO_UNUSED,
                .din = AUDIO_I2S_GPIO_DIN,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_i2s_rx, &tdm_cfg), TAG, "i2s rx tdm");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "i2s tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "i2s rx enable");
    return ESP_OK;
}

static esp_err_t init_es8311(i2c_master_bus_handle_t i2c_bus)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    const audio_codec_data_if_t* data = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data) {
        return ESP_FAIL;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = AUDIO_CODEC_ES8311_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t* ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl) {
        return ESP_FAIL;
    }

    const audio_codec_gpio_if_t* gpio = audio_codec_new_gpio();
    if (!gpio) {
        return ESP_FAIL;
    }

    es8311_codec_cfg_t codec_cfg = {};
    codec_cfg.ctrl_if = ctrl;
    codec_cfg.gpio_if = gpio;
    codec_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    codec_cfg.pa_pin = AUDIO_CODEC_PA_PIN;
    codec_cfg.use_mclk = true;
    codec_cfg.hw_gain.pa_voltage = 5.0;
    codec_cfg.hw_gain.codec_dac_voltage = 3.3;
    const audio_codec_if_t* codec = es8311_codec_new(&codec_cfg);
    if (!codec) {
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec,
        .data_if = data,
    };
    s_play_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_play_dev) {
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_play_dev, LIVEKIT_SPEAKER_VOLUME);
    return ESP_OK;
}

static esp_err_t init_es7210(i2c_master_bus_handle_t i2c_bus)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_i2s_rx,
    };
    const audio_codec_data_if_t* data = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data) {
        return ESP_FAIL;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = AUDIO_CODEC_ES7210_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t* ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl) {
        return ESP_FAIL;
    }

    es7210_codec_cfg_t codec_cfg = {};
    codec_cfg.ctrl_if = ctrl;
    codec_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    const audio_codec_if_t* codec = es7210_codec_new(&codec_cfg);
    if (!codec) {
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec,
        .data_if = data,
    };
    s_rec_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_rec_dev) {
        return ESP_FAIL;
    }
    esp_codec_dev_set_in_gain(s_rec_dev, 30.0);
    return ESP_OK;
}

static esp_err_t build_capturer(void)
{
    esp_capture_audio_aec_src_cfg_t aec_cfg = {
        .record_handle = s_rec_dev,
        .channel = 4,
        .channel_mask = 1 | 2,
    };
    s_audio_source = esp_capture_new_audio_aec_src(&aec_cfg);
    if (!s_audio_source) {
        return ESP_FAIL;
    }

    esp_capture_cfg_t cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src = s_audio_source,
    };
    return esp_capture_open(&cfg, &s_capturer);
}

static esp_err_t build_renderer(void)
{
    i2s_render_cfg_t i2s_cfg = {
        .play_handle = s_play_dev,
    };
    s_audio_renderer = av_render_alloc_i2s_render(&i2s_cfg);
    if (!s_audio_renderer) {
        return ESP_FAIL;
    }

    av_render_cfg_t render_cfg = {
        .audio_render = s_audio_renderer,
        .audio_raw_fifo_size = 8 * 4096,
        .audio_render_fifo_size = 100 * 1024,
        .allow_drop_data = false,
    };
    s_av_renderer = av_render_open(&render_cfg);
    if (!s_av_renderer) {
        return ESP_FAIL;
    }

    av_render_audio_frame_info_t frame_info = {};
    frame_info.sample_rate = LIVEKIT_I2S_SAMPLE_RATE;
    frame_info.channel = 2;
    frame_info.bits_per_sample = 16;
    av_render_set_fixed_frame_info(s_av_renderer, &frame_info);
    return ESP_OK;
}

extern "C" esp_err_t eidolon_livekit_board_init(void)
{
    if (s_capturer != nullptr) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t i2c_bus = Board::GetInstance().GetSharedI2cBus();
    if (i2c_bus == nullptr) {
        ESP_LOGE(TAG, "Board does not expose shared I2C bus");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_audio_enc_register_default();
    esp_audio_dec_register_default();

    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "i2s");
    ESP_RETURN_ON_ERROR(init_es8311(i2c_bus), TAG, "es8311");
    ESP_RETURN_ON_ERROR(init_es7210(i2c_bus), TAG, "es7210");
    ESP_RETURN_ON_ERROR(build_capturer(), TAG, "capturer");
    ESP_RETURN_ON_ERROR(build_renderer(), TAG, "renderer");

    ESP_LOGI(TAG, "LiveKit board media ready @ %d Hz", LIVEKIT_I2S_SAMPLE_RATE);
    return ESP_OK;
}

extern "C" esp_capture_handle_t eidolon_livekit_board_get_capturer(void)
{
    return s_capturer;
}

extern "C" av_render_handle_t eidolon_livekit_board_get_renderer(void)
{
    return s_av_renderer;
}

extern "C" void eidolon_livekit_board_deinit(void)
{
    if (s_av_renderer) {
        av_render_close(s_av_renderer);
        s_av_renderer = nullptr;
    }
    s_audio_renderer = nullptr;
    if (s_capturer) {
        esp_capture_close(s_capturer);
        s_capturer = nullptr;
    }
    s_audio_source = nullptr;
    if (s_play_dev) {
        esp_codec_dev_close(s_play_dev);
        esp_codec_dev_delete(s_play_dev);
        s_play_dev = nullptr;
    }
    if (s_rec_dev) {
        esp_codec_dev_close(s_rec_dev);
        esp_codec_dev_delete(s_rec_dev);
        s_rec_dev = nullptr;
    }
    if (s_i2s_tx) {
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = nullptr;
    }
    if (s_i2s_rx) {
        i2s_channel_disable(s_i2s_rx);
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = nullptr;
    }
}
