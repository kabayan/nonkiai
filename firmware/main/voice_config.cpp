#include "voice_config.h"
#include "display_style.h"
#include "trivia_table.h"
#include "interaction_messages.h"
#include "listen_mode.h"
#include "ble_stream.h"
#include "nk_event_core.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

namespace {

constexpr char kTag[] = "nk-vcfg";
constexpr char kNvsNamespace[] = "nk_voice";
constexpr char kNvsKeyVolume[] = "vol";
constexpr char kNvsKeyName[] = "name";
constexpr char kNvsKeySwapNodTilt[] = "swap_nt";
constexpr char kNvsKeyDisplayStyle[] = "disp_style";
constexpr char kNvsKeyIrPin[] = "ir_pin";
constexpr char kNvsKeyListenMode[] = "listen_mode"; // 旧 "ir_auto" は読まない（stt32 DLS-1082）
constexpr char kDefaultName[] = "のんきちゃん";
constexpr char kLegacyDefaultTap1[] = "タップいち";
constexpr const char *kGreetings[] = {
    "私は%s。遊んでね",
    "%sだよ。こんにちは",
    "%sだよ。よろしくね",
    "%sだよ。会えてうれしい",
    "%sだよ。一緒に遊ぼう",
    "%sだよ。今日もよろしく",
    "%sだよ。仲良くしてね",
    "%sだよ。呼んでくれた？",
    "%sだよ。元気だった？",
    "%sだよ。そばにいるよ",
};
constexpr size_t kGreetingCount = sizeof(kGreetings) / sizeof(kGreetings[0]);
static_assert(kGreetingCount == 10);
constexpr const char *kSleepMessages[] = {
    "むにゃむにゃ、おやすみなさい",
    "ふわあ、ねむくなっちゃった",
    "そろそろねるね。おやすみ",
    "きょうもたのしかったね",
    "いいゆめがみられますように",
    "すやすや、ゆめのなかへ",
    "おふとんで、ぬくぬくしよう",
    "またあした、一緒に遊ぼう",
    "ちょっとひとやすみするね",
    "おやすみ。そばにいてね",
};
constexpr const char *kWakeMessages[] = {
    "おはよう。ああよくねた",
    "ふわあ、おめめがさめたよ",
    "おはよう。いいゆめみたよ",
    "すっきりしたよ。おはよう",
    "おはよう。また遊ぼうね",
    "むにゃ、おきるじかんかな",
    "おはよう。きょうもよろしく",
    "いっぱいねて、げんきだよ",
    "おきたよ。まっててくれた？",
    "おはよう。そばにいてくれたね",
};
constexpr const char *kSleepTalkMessages[] = {
    "むにゃむにゃ、もうたべられないよ",
    "すやすや、いいゆめみてるよ",
    "むにゃ、あともうちょっとだけ",
    "ふわふわのおくもを、とんでるよ",
    "むにゃむにゃ、そこにいるの？",
    "おほしさま、きらきらしてるね",
    "すぴー、すぴー、ねむたいな",
    "ゆめのなかでも、一緒だよ",
    "むにゃ、だいじょうぶだよ",
    "あしたもいっぱい、遊ぼうね",
};
constexpr size_t kSleepMessageCount = sizeof(kSleepMessages) / sizeof(kSleepMessages[0]);
constexpr size_t kWakeMessageCount = sizeof(kWakeMessages) / sizeof(kWakeMessages[0]);
constexpr size_t kSleepTalkMessageCount = sizeof(kSleepTalkMessages) / sizeof(kSleepTalkMessages[0]);
static_assert(kSleepMessageCount == 10 && kWakeMessageCount == 10 && kSleepTalkMessageCount == 10);

static size_t random_message_index(size_t count)
{
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % count);
    uint32_t value;
    do { value = esp_random(); } while (value >= limit);
    return value % count;
}
constexpr uint8_t kDefaultVolume = 100;
static uint8_t s_volume = kDefaultVolume;
static bool s_swap_nod_tilt = true;
static std::atomic<uint8_t> s_ir_pin{0};
static std::atomic<uint8_t> s_listen_mode{NK_LISTEN_MODE_PLUSH};
static char s_name[NK_VOICE_NAME_MAX_LEN] = "のんきちゃん";

static bool is_unicode_space(uint32_t cp)
{
    return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0x85
        || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A)
        || cp == 0x2028 || cp == 0x2029 || cp == 0x202F
        || cp == 0x205F || cp == 0x3000;
}

// 名前だけは送信・保存前に厳密に検証する。既存の自由な発声テキストの
// 受け付け方は変更しない。
static bool validate_name(const char *name, size_t *out_len)
{
    if (name == nullptr) return false;
    const size_t len = std::strlen(name);
    if (len == 0 || len > NK_VOICE_NAME_MAX_BYTES) return false;

    size_t i = 0;
    size_t chars = 0;
    bool only_space = true;
    while (i < len) {
        const uint8_t b0 = static_cast<uint8_t>(name[i]);
        uint32_t cp = 0;
        size_t width = 0;
        if (b0 <= 0x7F) {
            cp = b0;
            width = 1;
        } else if (b0 >= 0xC2 && b0 <= 0xDF) {
            cp = b0 & 0x1F;
            width = 2;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            cp = b0 & 0x0F;
            width = 3;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            cp = b0 & 0x07;
            width = 4;
        } else {
            return false;
        }
        if (i + width > len) return false;
        for (size_t j = 1; j < width; ++j) {
            const uint8_t bj = static_cast<uint8_t>(name[i + j]);
            if ((bj & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (bj & 0x3F);
        }
        // Reject overlong encodings, surrogate code points, and values outside
        // Unicode. Control characters would make the preview/log ambiguous.
        if ((width == 2 && cp < 0x80) || (width == 3 && cp < 0x800)
            || (width == 4 && cp < 0x10000) || cp > 0x10FFFF
            || (cp >= 0xD800 && cp <= 0xDFFF) || cp < 0x20 || cp == 0x7F
            || (cp >= 0x80 && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029) {
            return false;
        }
        if (!is_unicode_space(cp)) only_space = false;
        if (++chars > NK_VOICE_NAME_MAX_CHARS) return false;
        i += width;
    }
    if (only_space) return false;
    if (out_len != nullptr) *out_len = len;
    return true;
}

// デフォルト設定テーブル
nk_action_voice_config_t s_configs[] = {
    {NK_MOTION_TAP1,           "tap1",           "私はのんきちゃん。遊んでね"},
    {NK_MOTION_TAP2,           "tap2",           "トリビア"},
    {NK_MOTION_TAP3,           "tap3",           "３秒じっとしてね"},
    {NK_MOTION_NOD,            "nod",            "ウンウン、私もそう思うよ。"},
    {NK_MOTION_ROCK,           "rock",           "むにゃむにゃ、おやすみなさい"},
    {NK_MOTION_SPIN180,        "spin180",        "はんてん"},
    {NK_MOTION_TILT_LR,        "tilt_lr",        "あれれ、なにかあったかな？"},
    {NK_MOTION_TURN_LR,        "turn_lr",        "大変だったね。大丈夫だよ。"},
    {NK_MOTION_CALIB_START,    "calib_start",    ""}, // 合図音ピピッがあるため既定は無声
    {NK_MOTION_CALIB_COMPLETE, "calib_complete", "リセットしたよ"},
    {NK_MOTION_CALIB_FAILED,   "calib_failed",   "うまくできなかったよ。もう一度ためしてね"},
    {NK_MOTION_WAKEUP,         "wakeup",         "おはよう。ああよくねた"},
    {NK_MOTION_NONE, "toy_forward", "まえにすすんで！"},
    {NK_MOTION_NONE, "toy_back", "うしろにさがって！"},
    {NK_MOTION_NONE, "toy_right", "みぎにまわって！"},
    {NK_MOTION_NONE, "toy_left", "ひだりにまわって！"},
};

constexpr size_t kConfigCount = sizeof(s_configs) / sizeof(s_configs[0]);

static bool set_self_introduction(void)
{
    char greeting[NK_VOICE_TEXT_MAX_LEN] = {0};
    const int written = std::snprintf(greeting, sizeof(greeting),
                                      "私は%s。遊んでね", s_name);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(greeting)) {
        return false;
    }
    std::strncpy(s_configs[0].text, greeting, sizeof(s_configs[0].text) - 1);
    s_configs[0].text[sizeof(s_configs[0].text) - 1] = '\0';
    return true;
}

static bool build_greeting(const char *name, char *buffer, size_t size)
{
    if (!name || !buffer || size == 0) return false;
    const size_t index = random_message_index(kGreetingCount);
    const int written = std::snprintf(buffer, size, kGreetings[index], name);
    return written > 0 && static_cast<size_t>(written) < size;
}

} // namespace

size_t nk_voice_config_count(void) {
    return kConfigCount;
}

const nk_action_voice_config_t *nk_voice_config_get_at(size_t index) {
    if (index >= kConfigCount) return nullptr;
    return &s_configs[index];
}

bool nk_voice_config_build_greeting(char *buffer, size_t size)
{
    return build_greeting(s_name, buffer, size);
}

const char *nk_voice_config_get_listen_reply(uint8_t intent) {
    constexpr const char *keys[] = {"toy_forward", "toy_back", "toy_right", "toy_left"};
    if (intent >= 4) return nullptr;
    for (const auto &config : s_configs) {
        if (std::strcmp(config.key_name, keys[intent]) == 0) return config.text;
    }
    return nullptr;
}

const char *nk_voice_config_get_text(nk_motion_event_t event) {
    if (event == NK_MOTION_NONE) return nullptr;
    for (size_t i = 0; i < kConfigCount; ++i) {
        if (s_configs[i].event == event) {
            const char *configured = s_configs[i].text;
            static int previous[3] = {-1, -1, -1};
            if (event == NK_MOTION_NOD
                && (std::strcmp(configured, "うなずき") == 0
                    || std::strcmp(configured, kNodMessages[0]) == 0)) {
                return kNodMessages[nk_choose_message(10, previous[0], esp_random)];
            }
            if (event == NK_MOTION_TILT_LR
                && (std::strcmp(configured, "かしげ") == 0
                    || std::strcmp(configured, kTiltMessages[0]) == 0)) {
                return kTiltMessages[nk_choose_message(10, previous[1], esp_random)];
            }
            if (event == NK_MOTION_TURN_LR
                && (std::strcmp(configured, "みまわし") == 0
                    || std::strcmp(configured, kTurnMessages[0]) == 0)) {
                return kTurnMessages[nk_choose_message(10, previous[2], esp_random)];
            }
            if (event == NK_MOTION_ROCK
                && (std::strcmp(configured, "ねかしつけ") == 0
                    || std::strcmp(configured, kSleepMessages[0]) == 0)) {
                return kSleepMessages[random_message_index(kSleepMessageCount)];
            }
            if (event == NK_MOTION_WAKEUP
                && (std::strcmp(configured, "おきたよ") == 0
                    || std::strcmp(configured, kWakeMessages[0]) == 0)) {
                return kWakeMessages[random_message_index(kWakeMessageCount)];
            }
            if (event == NK_MOTION_TAP2
                && (std::strcmp(s_configs[i].text, "トリビア") == 0
                    || std::strcmp(s_configs[i].text, "タップに") == 0)) {
                size_t index;
                while (!nk_trivia_index(esp_random(), &index)) {}
                ESP_LOGI(kTag, "[TRIVIA] id=%u text='%s'", (unsigned)(index + 1), kTriviaTexts[index]);
                return kTriviaTexts[index];
            }
            // Only expand the standard introduction. Explicit custom text,
            // including silence, remains compatible with the detailed editor.
            // Extract its name so NAME_SET alone does not change playback;
            // NAME_SAVE commits the name to this introduction as before.
            constexpr char prefix[] = "私は";
            constexpr char suffix[] = "。遊んでね";
            const char *text = s_configs[i].text;
            const size_t len = std::strlen(text);
            if (event == NK_MOTION_TAP1 && len > sizeof(prefix) + sizeof(suffix) - 2
                && std::strncmp(text, prefix, sizeof(prefix) - 1) == 0
                && std::strcmp(text + len - (sizeof(suffix) - 1), suffix) == 0) {
                const size_t name_len = len - (sizeof(prefix) - 1) - (sizeof(suffix) - 1);
                if (name_len <= NK_VOICE_NAME_MAX_BYTES) {
                    char name[NK_VOICE_NAME_MAX_LEN] = {};
                    std::memcpy(name, text + sizeof(prefix) - 1, name_len);
                    if (validate_name(name, nullptr)) {
                        // Used synchronously by the IMU event producer; the
                        // voice player copies the text into its own queue.
                        static char greeting[NK_VOICE_TEXT_MAX_LEN];
                        if (build_greeting(name, greeting, sizeof(greeting))) {
                            return greeting;
                        }
                    }
                }
            }
            return s_configs[i].text;
        }
    }
    return "";
}

const char *nk_voice_config_get_sleep_talk(void) {
    return kSleepTalkMessages[random_message_index(kSleepTalkMessageCount)];
}

esp_err_t nk_voice_config_set_text(nk_motion_event_t event, const char *text) {
    if (text == nullptr || event == NK_MOTION_NONE) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < kConfigCount; ++i) {
        if (s_configs[i].event == event) {
            std::strncpy(s_configs[i].text, text, sizeof(s_configs[i].text) - 1);
            s_configs[i].text[sizeof(s_configs[i].text) - 1] = '\0';
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nk_voice_config_set_text_by_key(const char *key, const char *text) {
    if (key == nullptr || text == nullptr) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < kConfigCount; ++i) {
        if (std::strcmp(s_configs[i].key_name, key) == 0) {
            std::strncpy(s_configs[i].text, text, sizeof(s_configs[i].text) - 1);
            s_configs[i].text[sizeof(s_configs[i].text) - 1] = '\0';
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void nk_voice_config_reset_defaults(void) {
    nk_action_voice_config_t defaults[] = {
        {NK_MOTION_TAP1,           "tap1",           "私はのんきちゃん。遊んでね"},
        {NK_MOTION_TAP2,           "tap2",           "トリビア"},
        {NK_MOTION_TAP3,           "tap3",           "３秒じっとしてね"},
        {NK_MOTION_NOD,            "nod",            "ウンウン、私もそう思うよ。"},
        {NK_MOTION_ROCK,           "rock",           "むにゃむにゃ、おやすみなさい"},
        {NK_MOTION_SPIN180,        "spin180",        "はんてん"},
        {NK_MOTION_TILT_LR,        "tilt_lr",        "あれれ、なにかあったかな？"},
        {NK_MOTION_TURN_LR,        "turn_lr",        "大変だったね。大丈夫だよ。"},
        {NK_MOTION_CALIB_START,    "calib_start",    ""},
        {NK_MOTION_CALIB_COMPLETE, "calib_complete", "リセットしたよ"},
        {NK_MOTION_CALIB_FAILED,   "calib_failed",   "うまくできなかったよ。もう一度ためしてね"},
        {NK_MOTION_WAKEUP,         "wakeup",         "おはよう。ああよくねた"},
        {NK_MOTION_NONE, "toy_forward", "まえにすすんで！"},
        {NK_MOTION_NONE, "toy_back", "うしろにさがって！"},
        {NK_MOTION_NONE, "toy_right", "みぎにまわって！"},
        {NK_MOTION_NONE, "toy_left", "ひだりにまわって！"},
    };
    for (size_t i = 0; i < kConfigCount; ++i) {
        std::strcpy(s_configs[i].text, defaults[i].text);
    }
    std::strncpy(s_name, kDefaultName, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    s_swap_nod_tilt = true;
    s_listen_mode.store(NK_LISTEN_MODE_PLUSH);
}

esp_err_t nk_voice_config_save(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open write failed: %s", esp_err_to_name(err));
        return err;
    }

    // Keep the first storage error. Returning only the final commit result used
    // to turn an earlier nvs_set_* failure into a false success notification.
    esp_err_t first_error = ESP_OK;
    for (size_t i = 0; i < kConfigCount; ++i) {
        err = nvs_set_str(handle, s_configs[i].key_name, s_configs[i].text);
        if (err != ESP_OK) {
            if (first_error == ESP_OK) first_error = err;
            ESP_LOGW(kTag, "nvs_set_str failed for %s: %s", s_configs[i].key_name, esp_err_to_name(err));
        }
    }

    err = nvs_set_str(handle, kNvsKeyName, s_name);
    if (err != ESP_OK) {
        if (first_error == ESP_OK) first_error = err;
        ESP_LOGW(kTag, "nvs_set_str failed for %s: %s", kNvsKeyName, esp_err_to_name(err));
    }

    err = nvs_set_u8(handle, kNvsKeyVolume, s_volume);
    if (err != ESP_OK) {
        if (first_error == ESP_OK) first_error = err;
        ESP_LOGW(kTag, "nvs_set_u8 failed for %s: %s", kNvsKeyVolume, esp_err_to_name(err));
    }
    err = nvs_set_u8(handle, kNvsKeySwapNodTilt, s_swap_nod_tilt ? 1 : 0);
    if (err != ESP_OK) {
        if (first_error == ESP_OK) first_error = err;
        ESP_LOGW(kTag, "nvs_set_u8 failed for %s: %s", kNvsKeySwapNodTilt, esp_err_to_name(err));
    }
    err = nvs_set_u8(handle, kNvsKeyListenMode, s_listen_mode.load());
    if (err != ESP_OK) {
        if (first_error == ESP_OK) first_error = err;
        ESP_LOGW(kTag, "nvs_set_u8 failed for %s: %s", kNvsKeyListenMode, esp_err_to_name(err));
    }
    err = nvs_set_u8(handle, kNvsKeyIrPin, s_ir_pin.load());
    if (err != ESP_OK) {
        if (first_error == ESP_OK) first_error = err;
        ESP_LOGW(kTag, "nvs_set_u8 failed for %s: %s", kNvsKeyIrPin, esp_err_to_name(err));
    }
    if (first_error == ESP_OK) {
        first_error = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGI(kTag, "voice config saved to NVS (status=%s)", esp_err_to_name(first_error));
    return first_error;
}

esp_err_t nk_voice_config_save_volume(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open write failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, kNvsKeyVolume, s_volume);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_set_u8 failed for %s: %s", kNvsKeyVolume, esp_err_to_name(err));
    } else {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGI(kTag, "voice volume saved to NVS (status=%s)", esp_err_to_name(err));
    return err;
}

esp_err_t nk_voice_config_load(void) {
    nk_voice_config_reset_defaults();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(kTag, "nvs_open read returned %s; using defaults", esp_err_to_name(err));
        // A first boot has no namespace yet. Defaults are a valid loaded state
        // for the UI, so do not report that expected condition as a failure.
        nk_ble_notify_ir_pin();
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }

    bool tap1_present = false;
    for (size_t i = 0; i < kConfigCount; ++i) {
        char buf[NK_VOICE_TEXT_MAX_LEN] = {0};
        size_t required_size = sizeof(buf);
        err = nvs_get_str(handle, s_configs[i].key_name, buf, &required_size);
        if (err == ESP_OK) {
            if (i == 0) tap1_present = true;
            std::strncpy(s_configs[i].text, buf, sizeof(s_configs[i].text) - 1);
            s_configs[i].text[sizeof(s_configs[i].text) - 1] = '\0';
        }
    }

    bool name_present = false;
    char name_buf[NK_VOICE_NAME_MAX_LEN] = {0};
    size_t name_size = sizeof(name_buf);
    err = nvs_get_str(handle, kNvsKeyName, name_buf, &name_size);
    if (err == ESP_OK) {
        size_t name_len = 0;
        if (validate_name(name_buf, &name_len)) {
            std::memcpy(s_name, name_buf, name_len + 1);
            name_present = true;
        } else {
            ESP_LOGW(kTag, "invalid name in NVS; using default");
        }
    }

    uint8_t loaded_vol = kDefaultVolume;
    if (nvs_get_u8(handle, kNvsKeyVolume, &loaded_vol) == ESP_OK) {
        s_volume = loaded_vol;
    }
    uint8_t loaded_swap = 1;
    if (nvs_get_u8(handle, kNvsKeySwapNodTilt, &loaded_swap) == ESP_OK) {
        s_swap_nod_tilt = loaded_swap != 0;
    }
    uint8_t loaded_mode = NK_LISTEN_MODE_PLUSH;
    if (nvs_get_u8(handle, kNvsKeyListenMode, &loaded_mode) != ESP_OK || !nk_listen_mode_valid(loaded_mode))
        loaded_mode = NK_LISTEN_MODE_PLUSH;
    s_listen_mode.store(loaded_mode);
    uint8_t loaded_pin = s_ir_pin.load();
    if (nvs_get_u8(handle, kNvsKeyIrPin, &loaded_pin) != ESP_OK)
        loaded_pin = s_ir_pin.load();
    else if (loaded_pin > 1)
        loaded_pin = 0;
    s_ir_pin.store(loaded_pin);
    nvs_close(handle);

    // Standard action label from older firmware enables the built-in catalog.
    // Empty or independently edited text remains an explicit customisation.
    if (std::strcmp(s_configs[1].text, "タップに") == 0) {
        std::strcpy(s_configs[1].text, "トリビア");
    }
    for (auto &config : s_configs) {
        if (config.event == NK_MOTION_TAP3 && std::strcmp(config.text, "タップさん") == 0) std::strcpy(config.text, "３秒じっとしてね");
        if (config.event == NK_MOTION_NOD && std::strcmp(config.text, "うなずき") == 0) std::strcpy(config.text, "ウンウン、私もそう思うよ。");
        if (config.event == NK_MOTION_TILT_LR && std::strcmp(config.text, "かしげ") == 0) std::strcpy(config.text, "あれれ、なにかあったかな？");
        if (config.event == NK_MOTION_TURN_LR && std::strcmp(config.text, "みまわし") == 0) std::strcpy(config.text, "大変だったね。大丈夫だよ。");
        if (config.event == NK_MOTION_CALIB_COMPLETE && std::strcmp(config.text, "せいこう") == 0) std::strcpy(config.text, "リセットしたよ");
        if (config.event == NK_MOTION_CALIB_FAILED && std::strcmp(config.text, "しっぱい") == 0) std::strcpy(config.text, "うまくできなかったよ。もう一度ためしてね");
        if (config.event == NK_MOTION_ROCK && std::strcmp(config.text, "ねかしつけ") == 0) {
            std::strcpy(config.text, kSleepMessages[0]);
        } else if (config.event == NK_MOTION_WAKEUP && std::strcmp(config.text, "おきたよ") == 0) {
            std::strcpy(config.text, kWakeMessages[0]);
        }
    }

    // Firmware before the name setting stored tap1's initial action label.
    // Migrate only that exact legacy value; a different text is an explicit
    // customisation and must remain untouched. The migration is in RAM and is
    // persisted by the next normal SAVE or NAME_SAVE operation.
    if (!name_present && (!tap1_present || std::strcmp(s_configs[0].text, kLegacyDefaultTap1) == 0)) {
        if (!set_self_introduction()) {
            ESP_LOGE(kTag, "failed to build default self introduction");
        }
    }
    nk_ble_notify_ir_pin();
    ESP_LOGI(kTag, "voice config loaded from NVS");
    return ESP_OK;
}

esp_err_t nk_voice_config_init(void) {
    // 起動時にロードを試行。NVS に無ければデフォルト値を維持
    nk_voice_config_load();
    return ESP_OK;
}

bool nk_voice_config_get_swap_nod_tilt(void) { return s_swap_nod_tilt; }
void nk_voice_config_set_swap_nod_tilt(bool enabled) { s_swap_nod_tilt = enabled; }

uint8_t nk_voice_config_get_ir_pin(void) { return s_ir_pin.load(); }
bool nk_voice_config_set_ir_pin(uint8_t pin)
{
    if (pin > 1) return false;
    s_ir_pin.store(pin);
    return true;
}

bool nk_voice_config_change_ir_pin(uint8_t pin)
{
    const uint8_t previous_pin = s_ir_pin.load();
    if (!nk_voice_config_set_ir_pin(pin)) return false;
    const esp_err_t err = nk_voice_config_save();
    if (err != ESP_OK) {
        s_ir_pin.store(previous_pin);
        ESP_LOGW(kTag, "IR pin save failed: %s", esp_err_to_name(err));
    }
    nk_ble_notify_ir_pin();
    return err == ESP_OK;
}

uint8_t nk_voice_config_get_listen_mode(void) { return s_listen_mode.load(); }
bool nk_voice_config_set_listen_mode(uint8_t mode)
{
    if (!nk_listen_mode_valid(mode)) return false;
    s_listen_mode.store(mode);
    return true;
}

bool nk_voice_config_change_listen_mode(uint8_t mode)
{
    if (!nk_voice_config_set_listen_mode(mode)) return false;
    const esp_err_t err = nk_voice_config_save();
    if (err != ESP_OK) ESP_LOGW(kTag, "listen mode save failed: %s", esp_err_to_name(err));
    nk_ble_notify_listen_mode();
    nk::event_mode(mode);
    return true;
}

esp_err_t nk_voice_config_to_json(char *buffer, size_t max_len) {
    if (buffer == nullptr || max_len == 0) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    for (size_t i = 0; i < kConfigCount; ++i) {
        cJSON_AddStringToObject(root, s_configs[i].key_name, s_configs[i].text);
    }

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (printed == nullptr) return ESP_FAIL;

    if (std::strlen(printed) >= max_len) {
        std::free(printed);
        return ESP_ERR_NO_MEM;
    }

    std::strncpy(buffer, printed, max_len - 1);
    buffer[max_len - 1] = '\0';
    std::free(printed);
    return ESP_OK;
}

esp_err_t nk_voice_config_from_json(const char *json_str) {
    if (json_str == nullptr) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) return ESP_ERR_INVALID_ARG;

    for (size_t i = 0; i < kConfigCount; ++i) {
        cJSON *item = cJSON_GetObjectItem(root, s_configs[i].key_name);
        if (item && cJSON_IsString(item) && (item->valuestring != nullptr)) {
            std::strncpy(s_configs[i].text, item->valuestring, sizeof(s_configs[i].text) - 1);
            s_configs[i].text[sizeof(s_configs[i].text) - 1] = '\0';
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

uint8_t nk_voice_config_get_volume(void) {
    return s_volume;
}

void nk_voice_config_set_volume(uint8_t volume) {
    s_volume = volume;
}

const char *nk_voice_config_get_name(void)
{
    return s_name;
}

esp_err_t nk_voice_config_set_name(const char *name)
{
    size_t name_len = 0;
    if (!validate_name(name, &name_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (name_len > NK_VOICE_NAME_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    std::memcpy(s_name, name, name_len);
    s_name[name_len] = '\0';
    return ESP_OK;
}

esp_err_t nk_voice_config_save_name(void)
{
    if (!validate_name(s_name, nullptr)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!set_self_introduction()) {
        return ESP_ERR_INVALID_SIZE;
    }
    return nk_voice_config_save();
}

// This key is independent of the full voice-settings save operation.
uint8_t nk_voice_config_get_display_style(void) {
    nvs_handle_t h;
    uint8_t style = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u8(h, kNvsKeyDisplayStyle, &style);
        nvs_close(h);
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW(kTag, "disp_style read failed: %s", esp_err_to_name(err));
    return nk_display_style_valid(style);
}
void nk_voice_config_set_display_style(uint8_t style) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, kNvsKeyDisplayStyle, nk_display_style_valid(style));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGW(kTag, "disp_style write failed: %s", esp_err_to_name(err));
}
