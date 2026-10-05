#include "board_voice_audio.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "board_voice_audio";
static portMUX_TYPE s_owner_lock = portMUX_INITIALIZER_UNLOCKED;
static bsp_extra_audio_owner_t s_owner = BSP_EXTRA_AUDIO_OWNER_NONE;

static i2s_chan_handle_t s_tx = NULL;
static i2s_chan_handle_t s_rx = NULL;
static const audio_codec_data_if_t* s_data = NULL;
static const audio_codec_gpio_if_t* s_gpio = NULL;
static const audio_codec_ctrl_if_t* s_speaker_ctrl = NULL;
static const audio_codec_ctrl_if_t* s_mic_ctrl = NULL;
static const audio_codec_if_t* s_speaker_codec = NULL;
static const audio_codec_if_t* s_mic_codec = NULL;
static esp_codec_dev_handle_t s_speaker = NULL;
static esp_codec_dev_handle_t s_mic = NULL;
static bool s_speaker_open = false;
static bool s_mic_open = false;
static bool s_stereo_to_tdm = false;
static uint32_t s_rate = 0;

static esp_err_t close_codecs(void) {
    esp_err_t result = ESP_OK;
    if (s_mic_open && s_mic != s_speaker) {
        if (esp_codec_dev_close(s_mic) != ESP_OK) result = ESP_FAIL;
        s_mic_open = false;
    }
    if (s_speaker_open) {
        if (esp_codec_dev_close(s_speaker) != ESP_OK) result = ESP_FAIL;
        s_speaker_open = false;
        if (s_mic == s_speaker) s_mic_open = false;
    }
    s_stereo_to_tdm = false;
    return result;
}

static void delete_audio(void) {
    (void)close_codecs();
    if (s_speaker == s_mic) s_speaker = NULL;
    if (s_speaker_codec == s_mic_codec) s_mic_codec = NULL;
    if (s_speaker_ctrl == s_mic_ctrl) s_mic_ctrl = NULL;
    if (s_mic != NULL) {
        esp_codec_dev_delete(s_mic);
        s_mic = NULL;
    }
    if (s_speaker != NULL) {
        esp_codec_dev_delete(s_speaker);
        s_speaker = NULL;
    }
    if (s_mic_codec != NULL) {
        (void)audio_codec_delete_codec_if(s_mic_codec);
        s_mic_codec = NULL;
    }
    if (s_speaker_codec != NULL) {
        (void)audio_codec_delete_codec_if(s_speaker_codec);
        s_speaker_codec = NULL;
    }
    if (s_mic_ctrl != NULL) {
        (void)audio_codec_delete_ctrl_if(s_mic_ctrl);
        s_mic_ctrl = NULL;
    }
    if (s_speaker_ctrl != NULL) {
        (void)audio_codec_delete_ctrl_if(s_speaker_ctrl);
        s_speaker_ctrl = NULL;
    }
    if (s_gpio != NULL) {
        (void)audio_codec_delete_gpio_if(s_gpio);
        s_gpio = NULL;
    }
    if (s_data != NULL) {
        (void)audio_codec_delete_data_if(s_data);
        s_data = NULL;
    }
    if (s_rx != NULL) {
        (void)i2s_channel_disable(s_rx);
        (void)i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    if (s_tx != NULL) {
        (void)i2s_channel_disable(s_tx);
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    s_rate = 0;
}

static esp_err_t create_es8311(uint32_t rate) {
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    esp_err_t result = i2s_new_channel(&channel, &s_tx, &s_rx);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "I2S unavailable; another app may own audio: %s", esp_err_to_name(result));
        return result;
    }

    i2s_std_config_t i2s = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    i2s.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    result = i2s_channel_init_std_mode(s_tx, &i2s);
    if (result == ESP_OK) result = i2s_channel_init_std_mode(s_rx, &i2s);
    if (result == ESP_OK) result = i2s_channel_enable(s_tx);
    if (result == ESP_OK) result = i2s_channel_enable(s_rx);
    if (result != ESP_OK) return result;

    audio_codec_i2s_cfg_t data_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    s_data = audio_codec_new_i2s_data(&data_cfg);
    if (s_data == NULL) return ESP_ERR_NO_MEM;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) return ESP_ERR_INVALID_STATE;
    s_gpio = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t codec_i2c = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    s_speaker_ctrl = audio_codec_new_i2c_ctrl(&codec_i2c);
    if (s_gpio == NULL || s_speaker_ctrl == NULL) return ESP_ERR_NO_MEM;

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = s_speaker_ctrl,
        .gpio_if = s_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = true,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
        .no_dac_ref = true,
    };
    s_speaker_codec = es8311_codec_new(&codec_cfg);
    if (s_speaker_codec == NULL) return ESP_ERR_NO_MEM;
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_speaker_codec,
        .data_if = s_data,
    };
    s_speaker = esp_codec_dev_new(&dev_cfg);
    if (s_speaker == NULL) return ESP_ERR_NO_MEM;
    s_mic = s_speaker;
    s_rate = rate;
    ESP_LOGI(TAG, "ES8311 microphone ready");
    return ESP_OK;
}

static esp_err_t create_audio(uint32_t rate) {
    if (s_data != NULL) return s_rate == rate ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (rate < 8000 || rate > 48000) return ESP_ERR_INVALID_ARG;
#if CONFIG_BSP_BOARD_LCD_2_8
    esp_err_t created = create_es8311(rate);
    if (created != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 initialization failed: %s", esp_err_to_name(created));
        delete_audio();
    }
    return created;
#else
    if (s_data != NULL) return s_rate == rate ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (rate < 8000 || rate > 48000) return ESP_ERR_INVALID_ARG;

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    esp_err_t result = i2s_new_channel(&channel, &s_tx, &s_rx);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "I2S unavailable; another app may own audio: %s", esp_err_to_name(result));
        return result;
    }

    i2s_std_config_t tx = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    // Full-duplex TX and RX share BCLK/WS. Two 32-bit TX slots match the
    // four 16-bit ES7210 TDM RX slots: both frames are 64 BCLK ticks.
    // The ES8311 still receives 16 valid audio bits within each TX slot.
    tx.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    tx.slot_cfg.ws_width = 32;
    i2s_tdm_config_t rx = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_TDM_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = I2S_GPIO_UNUSED,
            .din = BSP_I2S_DSIN,
        },
    };
    tx.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    rx.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    rx.clk_cfg.bclk_div = 8;
    rx.slot_cfg.total_slot = 4;

    result = i2s_channel_init_std_mode(s_tx, &tx);
    if (result == ESP_OK) result = i2s_channel_init_tdm_mode(s_rx, &rx);
    if (result == ESP_OK) result = i2s_channel_enable(s_tx);
    if (result == ESP_OK) result = i2s_channel_enable(s_rx);
    if (result != ESP_OK) goto fail;

    audio_codec_i2s_cfg_t data_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    s_data = audio_codec_new_i2s_data(&data_cfg);
    if (s_data == NULL) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        result = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    s_gpio = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t speaker_i2c = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    audio_codec_i2c_cfg_t mic_i2c = {
        .port = BSP_I2C_NUM,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    s_speaker_ctrl = audio_codec_new_i2c_ctrl(&speaker_i2c);
    s_mic_ctrl = audio_codec_new_i2c_ctrl(&mic_i2c);
    if (s_gpio == NULL || s_speaker_ctrl == NULL || s_mic_ctrl == NULL) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t speaker_cfg = {
        .ctrl_if = s_speaker_ctrl,
        .gpio_if = s_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .hw_gain = gain,
    };
    es7210_codec_cfg_t mic_cfg = {
        .ctrl_if = s_mic_ctrl,
        .mic_selected = BSP_EXTRA_ES7210_PHYSICAL_CONNECTED_MIC_MASK,
    };
    s_speaker_codec = es8311_codec_new(&speaker_cfg);
    s_mic_codec = es7210_codec_new(&mic_cfg);
    if (s_speaker_codec == NULL || s_mic_codec == NULL) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }
    esp_codec_dev_cfg_t speaker_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_speaker_codec,
        .data_if = s_data,
    };
    esp_codec_dev_cfg_t mic_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_mic_codec,
        .data_if = s_data,
    };
    s_speaker = esp_codec_dev_new(&speaker_dev_cfg);
    s_mic = esp_codec_dev_new(&mic_dev_cfg);
    if (s_speaker == NULL || s_mic == NULL) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_rate = rate;
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Voice audio initialization failed: %s", esp_err_to_name(result));
    delete_audio();
    return result;
#endif
}

bsp_extra_audio_owner_t bsp_extra_audio_session_get_owner(void) {
    portENTER_CRITICAL(&s_owner_lock);
    const bsp_extra_audio_owner_t owner = s_owner;
    portEXIT_CRITICAL(&s_owner_lock);
    return owner;
}

const char* bsp_extra_audio_owner_name(bsp_extra_audio_owner_t owner) {
    return owner == BSP_EXTRA_AUDIO_OWNER_XIAOZHI ? "Xiaozhi" :
           owner == BSP_EXTRA_AUDIO_OWNER_VIBE ? "Vibe" :
           owner == BSP_EXTRA_AUDIO_OWNER_NONE ? "None" : "Invalid";
}

esp_err_t bsp_extra_audio_session_acquire(bsp_extra_audio_owner_t owner) {
    if (owner != BSP_EXTRA_AUDIO_OWNER_XIAOZHI && owner != BSP_EXTRA_AUDIO_OWNER_VIBE) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_owner_lock);
    const bool available = s_owner == BSP_EXTRA_AUDIO_OWNER_NONE;
    if (available) s_owner = owner;
    portEXIT_CRITICAL(&s_owner_lock);
    return available ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t bsp_extra_audio_session_release(bsp_extra_audio_owner_t owner) {
    if (owner != BSP_EXTRA_AUDIO_OWNER_XIAOZHI && owner != BSP_EXTRA_AUDIO_OWNER_VIBE) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_owner_lock);
    const bool owned = s_owner == owner;
    portEXIT_CRITICAL(&s_owner_lock);
    if (!owned) return ESP_ERR_INVALID_STATE;
    delete_audio();
    portENTER_CRITICAL(&s_owner_lock);
    s_owner = BSP_EXTRA_AUDIO_OWNER_NONE;
    portEXIT_CRITICAL(&s_owner_lock);
    return ESP_OK;
}

esp_err_t bsp_extra_codec_set_voice_fs(uint32_t rate, uint32_t bits,
                                        uint8_t channels, uint16_t slot_mask,
                                        uint16_t mic_mask) {
#if CONFIG_BSP_BOARD_LCD_2_8
    (void)channels;
    (void)slot_mask;
    (void)mic_mask;
    if (bsp_extra_audio_session_get_owner() == BSP_EXTRA_AUDIO_OWNER_NONE || bits != 16) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t opened = create_audio(rate);
    if (opened != ESP_OK) return opened;
    opened = close_codecs();
    if (opened != ESP_OK) return opened;
    esp_codec_dev_sample_info_t format = {
        .sample_rate = rate,
        .channel = 2,
        .bits_per_sample = 16,
    };
    opened = esp_codec_dev_open(s_speaker, &format);
    if (opened != ESP_OK) return opened;
    s_speaker_open = true;
    s_mic_open = true;
    opened = esp_codec_dev_set_out_mute(s_speaker, true);
    if (opened == ESP_OK) opened = esp_codec_dev_set_in_gain(s_mic, 24.0f);
    if (opened != ESP_OK) {
        (void)close_codecs();
        return opened;
    }
    s_stereo_to_tdm = true;
    return ESP_OK;
#else
    if (bsp_extra_audio_session_get_owner() == BSP_EXTRA_AUDIO_OWNER_NONE ||
        bits != 16 || channels != 4 || slot_mask != 0x0F || mic_mask != 0x07) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = create_audio(rate);
    if (result != ESP_OK) return result;
    result = close_codecs();
    if (result != ESP_OK) return result;
    esp_codec_dev_sample_info_t output = {
        .sample_rate = rate,
        .channel = 2,
        .bits_per_sample = 16,
    };
    esp_codec_dev_sample_info_t input = {
        .sample_rate = rate,
        .channel = 4,
        .channel_mask = slot_mask,
        .bits_per_sample = 16,
    };
    result = esp_codec_dev_open(s_speaker, &output);
    if (result != ESP_OK) return result;
    s_speaker_open = true;
    result = esp_codec_dev_set_out_mute(s_speaker, false);
    if (result == ESP_OK) result = esp_codec_dev_set_out_vol(s_speaker, 80);
    if (result == ESP_OK) result = esp_codec_dev_open(s_mic, &input);
    if (result != ESP_OK) {
        (void)close_codecs();
        return result;
    }
    s_mic_open = true;
    result = esp_codec_dev_set_in_channel_gain(s_mic, mic_mask, 24.0f);
    if (result != ESP_OK) (void)close_codecs();
    return result;
#endif
}

esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits, i2s_slot_mode_t channels) {
    if (bsp_extra_audio_session_get_owner() == BSP_EXTRA_AUDIO_OWNER_NONE ||
        bits != 16 || channels != I2S_SLOT_MODE_STEREO) return ESP_ERR_INVALID_ARG;
    esp_err_t result = create_audio(rate);
    if (result != ESP_OK) return result;
    result = close_codecs();
    if (result != ESP_OK) return result;
    esp_codec_dev_sample_info_t output = {
        .sample_rate = rate,
        .channel = 2,
        .bits_per_sample = 16,
    };
    result = esp_codec_dev_open(s_speaker, &output);
    if (result == ESP_OK) s_speaker_open = true;
    if (result == ESP_OK) result = esp_codec_dev_set_out_mute(s_speaker, false);
    if (result == ESP_OK) result = esp_codec_dev_set_out_vol(s_speaker, 80);
    if (result != ESP_OK) (void)close_codecs();
    return result;
}

esp_err_t bsp_extra_codec_dev_stop(void) {
    return close_codecs();
}

esp_err_t bsp_extra_codec_mute_set(bool muted) {
    if (!s_speaker_open) return ESP_ERR_INVALID_STATE;
    return esp_codec_dev_set_out_mute(s_speaker, muted);
}

esp_err_t bsp_extra_i2s_read(void* buffer, size_t length, size_t* bytes_read, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (bytes_read != NULL) *bytes_read = 0;
    if (buffer == NULL || !s_mic_open) return ESP_ERR_INVALID_STATE;
    if (s_stereo_to_tdm) {
        if (length == 0 || (length % (4 * sizeof(int16_t))) != 0) return ESP_ERR_INVALID_SIZE;
        const size_t frames = length / (4 * sizeof(int16_t));
        int16_t *out = buffer;
        size_t done = 0;
        while (done < frames) {
            const size_t chunk = frames - done > 128 ? 128 : frames - done;
            int16_t stereo[128 * 2];
            const esp_err_t result = esp_codec_dev_read(s_mic, stereo, chunk * 2 * sizeof(int16_t));
            if (result != ESP_OK) return result;
            for (size_t i = 0; i < chunk; ++i) {
                out[(done + i) * 4] = stereo[i * 2];
                out[(done + i) * 4 + 1] = 0;
                out[(done + i) * 4 + 2] = 0;
                out[(done + i) * 4 + 3] = 0;
            }
            done += chunk;
        }
        if (bytes_read != NULL) *bytes_read = length;
        return ESP_OK;
    }
    const esp_err_t result = esp_codec_dev_read(s_mic, buffer, length);
    if (result == ESP_OK && bytes_read != NULL) *bytes_read = length;
    return result;
}

esp_err_t bsp_extra_i2s_write(void* buffer, size_t length, size_t* bytes_written, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (bytes_written != NULL) *bytes_written = 0;
    if (buffer == NULL || !s_speaker_open) return ESP_ERR_INVALID_STATE;
    const esp_err_t result = esp_codec_dev_write(s_speaker, buffer, length);
    if (result == ESP_OK && bytes_written != NULL) *bytes_written = length;
    return result;
}
