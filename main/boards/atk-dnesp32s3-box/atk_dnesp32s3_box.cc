#include "wifi_board.h"
#include "audio_codec.h"
#include "audio_codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "iot/thing_manager.h"
#include "i2c_device.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>

#define TAG "atk_dnesp32s3_box"

// 自定义 LED 类，始终保持关闭状态
class AlwaysOffLed : public Led {
public:
    AlwaysOffLed() {
        // 初始化时关闭 LED
        TurnOff();
    }
    
    // 无论设备状态如何变化，都保持 LED 关闭
    virtual void OnStateChanged() override {
        TurnOff();
    }
    
private:
    void TurnOff() {
        // 如果有需要，可以在这里添加额外的关闭 LED 的代码
        ESP_LOGI(TAG, "LED is kept off");
    }
};

// 定义 K1 和 K2 按键对应的 XL9555 IO 引脚
#define K1_IO_PIN 4  // XL9555 的 IOQ_4 引脚
#define K2_IO_PIN 3  // XL9555 的 IOQ_3 引脚

// 音量控制参数
#define VOLUME_STEP 5  // 每次按键音量变化步长
#define MIN_VOLUME 0   // 最小音量
#define MAX_VOLUME 100 // 最大音量

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class XL9555 : public I2cDevice {
public:
    XL9555(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        // 寄存器 0x06 和 0x07 是配置寄存器，0 表示输入，1 表示输出
        // 确保引脚 3 和 4 被配置为输入模式 (0)
        WriteReg(0x06, 0x07); // 0x07 = 0000 0111，引脚 3 和 4 为 0 (输入)
        WriteReg(0x07, 0xFE);
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        uint16_t data;
        if (bit < 8) {
            data = ReadReg(0x02);
        } else {
            data = ReadReg(0x03);
            bit -= 8;
        }

        data = (data & ~(1 << bit)) | (level << bit);

        if (bit < 8) {
            WriteReg(0x02, data);
        } else {
            WriteReg(0x03, data);
        }
    }
    
    // 读取输入引脚状态
    bool GetInputState(uint8_t bit) {
        uint8_t data;
        if (bit < 8) {
            data = ReadReg(0x00);
        } else {
            data = ReadReg(0x01);
            bit -= 8;
        }
        
        return (data & (1 << bit)) == 0; // 按键按下时为低电平
    }
    
    // 添加公共方法来获取寄存器值
    uint8_t GetRegValue(uint8_t reg) {
        return ReadReg(reg);
    }
};

class atk_dnesp32s3_box : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t xl9555_handle_;
    Button boot_button_;
    LcdDisplay* display_;
    XL9555* xl9555_;
    int current_volume_ = 70; // 默认音量
    
    // 按键状态
    bool k1_last_state_ = false;
    bool k2_last_state_ = false;
    
    // 按键检测任务
    TaskHandle_t button_task_handle_ = nullptr;
    
    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = GPIO_NUM_48,
            .scl_io_num = GPIO_NUM_45,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // Initialize XL9555
        xl9555_ = new XL9555(i2c_bus_, 0x20);
    }

    void InitializeATK_ST7789_80_Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        /* 配置RD引脚 */
        gpio_config_t gpio_init_struct;
        gpio_init_struct.intr_type = GPIO_INTR_DISABLE;
        gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT;
        gpio_init_struct.pin_bit_mask = 1ull << LCD_NUM_RD;
        gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&gpio_init_struct);
        gpio_set_level(LCD_NUM_RD, 1);

        esp_lcd_i80_bus_handle_t i80_bus = NULL;
        esp_lcd_i80_bus_config_t bus_config = {
            .dc_gpio_num = LCD_NUM_DC,
            .wr_gpio_num = LCD_NUM_WR,
            .clk_src = LCD_CLK_SRC_DEFAULT,
            .data_gpio_nums = {
                GPIO_LCD_D0,
                GPIO_LCD_D1,
                GPIO_LCD_D2,
                GPIO_LCD_D3,
                GPIO_LCD_D4,
                GPIO_LCD_D5,
                GPIO_LCD_D6,
                GPIO_LCD_D7,
            },
            .bus_width = 8,
            .max_transfer_bytes = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
            .psram_trans_align = 64,
            .sram_trans_align = 4,
        };
        ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

        esp_lcd_panel_io_i80_config_t io_config = {
            .cs_gpio_num = LCD_NUM_CS,
            .pclk_hz = (10 * 1000 * 1000),
            .trans_queue_depth = 10,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .dc_levels = {
                .dc_idle_level = 0,
                .dc_cmd_level = 0,
                .dc_dummy_level = 0,
                .dc_data_level = 1,
            },
            .flags = {
                .swap_color_bytes = 0,
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = LCD_NUM_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        esp_lcd_panel_set_gap(panel, 0, 0);
        uint8_t data0[] = {0x00};
        uint8_t data1[] = {0x65};
        esp_lcd_panel_io_tx_param(panel_io, 0x36, data0, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0x3A, data1, 1);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    {
                                        .text_font = &font_puhui_20_4,
                                        .icon_font = &font_awesome_20_4,
                                        #if CONFIG_USE_WECHAT_MESSAGE_STYLE
                                            .emoji_font = font_emoji_32_init(),
                                        #else
                                            .emoji_font = DISPLAY_HEIGHT >= 240 ? font_emoji_64_init() : font_emoji_32_init(),
                                        #endif
                                    });
    }

    // 按键检测任务函数
    static void ButtonTaskFunction(void* arg) {
        atk_dnesp32s3_box* board = static_cast<atk_dnesp32s3_box*>(arg);
        while (true) {
            board->CheckButtons();
            vTaskDelay(pdMS_TO_TICKS(100)); // 每100毫秒检测一次按键状态
        }
    }
    
    // 检测按键状态
    void CheckButtons() {
        // 检测 K1 按键 (音量+)
        bool k1_current_state = xl9555_->GetInputState(K1_IO_PIN);
        if (k1_current_state && !k1_last_state_) {
            // K1 按下
            IncreaseVolume();
        }
        k1_last_state_ = k1_current_state;
        
        // 检测 K2 按键 (音量-)
        bool k2_current_state = xl9555_->GetInputState(K2_IO_PIN);
        if (k2_current_state && !k2_last_state_) {
            // K2 按下
            DecreaseVolume();
        }
        k2_last_state_ = k2_current_state;
    }
    
    // 调试函数：打印所有 XL9555 引脚状态
    void DebugPrintAllPinStates() {
        static uint8_t last_port0 = 0xFF;
        static uint8_t last_port1 = 0xFF;
        
        // 读取两个端口的状态
        uint8_t port0 = xl9555_->GetRegValue(0x00);
        uint8_t port1 = xl9555_->GetRegValue(0x01);
        
        // 只有当状态变化时才打印
        if (port0 != last_port0 || port1 != last_port1) {
            ESP_LOGI(TAG, "XL9555 引脚状态变化:");
            ESP_LOGI(TAG, "端口0 (引脚 0-7): 0x%02X", port0);
            ESP_LOGI(TAG, "端口1 (引脚 8-15): 0x%02X", port1);
            
            // 打印每个引脚的状态
            ESP_LOGI(TAG, "引脚状态 (0=高电平, 1=低电平):");
            for (int i = 0; i < 8; i++) {
                bool state = (port0 & (1 << i)) == 0;
                ESP_LOGI(TAG, "引脚 %d: %s", i, state ? "按下" : "释放");
            }
            for (int i = 0; i < 8; i++) {
                bool state = (port1 & (1 << i)) == 0;
                ESP_LOGI(TAG, "引脚 %d: %s", i + 8, state ? "按下" : "释放");
            }
            
            // 更新上次状态
            last_port0 = port0;
            last_port1 = port1;
        }
    }

    void InitializeButtons() {
        // 初始化 BOOT 按键
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
        });
        boot_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        boot_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });
        
        // 创建按键检测任务
        xTaskCreate(ButtonTaskFunction, "button_task", 4096, this, 5, &button_task_handle_);
        
        // 显示初始音量
        if (display_) {
            char volume_text[32];
            snprintf(volume_text, sizeof(volume_text), "初始音量: %d%%", current_volume_);
            display_->ShowNotification(volume_text, 2000);
        }
    }
    
    // 生成音量条图标
    std::string GenerateVolumeBar(int volume) {
        const int max_bars = 10;
        int filled_bars = (volume * max_bars) / 100;
        
        std::string volume_bar = "[";
        for (int i = 0; i < max_bars; i++) {
            if (i < filled_bars) {
                volume_bar += "■"; // 已填充部分
            } else {
                volume_bar += "□"; // 未填充部分
            }
        }
        volume_bar += "]";
        
        return volume_bar;
    }
    
    // 增加音量
    void IncreaseVolume() {
        current_volume_ += VOLUME_STEP;
        if (current_volume_ > MAX_VOLUME) {
            current_volume_ = MAX_VOLUME;
        }
        
        ESP_LOGI(TAG, "Volume increased to: %d", current_volume_);
        GetAudioCodec()->SetOutputVolume(current_volume_);
        
        // 在屏幕上显示音量百分比和音量条
        if (display_) {
            std::string volume_bar = GenerateVolumeBar(current_volume_);
            char volume_text[64];
            snprintf(volume_text, sizeof(volume_text), "音量: %d%%\n%s", current_volume_, volume_bar.c_str());
            display_->ShowNotification(volume_text, 2000); // 显示2秒
        }
    }
    
    // 减小音量
    void DecreaseVolume() {
        current_volume_ -= VOLUME_STEP;
        if (current_volume_ < MIN_VOLUME) {
            current_volume_ = MIN_VOLUME;
        }
        
        ESP_LOGI(TAG, "Volume decreased to: %d", current_volume_);
        GetAudioCodec()->SetOutputVolume(current_volume_);
        
        // 在屏幕上显示音量百分比和音量条
        if (display_) {
            std::string volume_bar = GenerateVolumeBar(current_volume_);
            char volume_text[64];
            snprintf(volume_text, sizeof(volume_text), "音量: %d%%\n%s", current_volume_, volume_bar.c_str());
            display_->ShowNotification(volume_text, 2000); // 显示2秒
        }
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
    }

    // 设置屏幕为黑色背景，文字为灰白色
    void SetScreenBlack() {
        if (display_ == nullptr) {
            return;
        }
        
        // 获取活动屏幕
        lv_obj_t* screen = lv_screen_active();
        if (screen == nullptr) {
            ESP_LOGE(TAG, "Failed to get active screen");
            return;
        }
        
        // 设置屏幕背景颜色为黑色
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        
        // 设置所有子对象的文本颜色为灰白色
        lv_color_t text_color = lv_color_make(200, 200, 200); // 灰白色
        lv_color_t border_color = lv_color_make(50, 50, 50);  // 深灰色
        
        // 遍历屏幕上的所有对象，设置文本颜色
        uint32_t child_cnt = lv_obj_get_child_cnt(screen);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t* child = lv_obj_get_child(screen, i);
            if (child) {
                // 设置子对象背景为黑色
                lv_obj_set_style_bg_color(child, lv_color_black(), 0);
                
                // 设置子对象文本颜色为灰白色
                lv_obj_set_style_text_color(child, text_color, 0);
                
                // 设置子对象边框颜色为深灰色
                lv_obj_set_style_border_color(child, border_color, 0);
                
                // 递归设置子对象的子对象
                SetObjectColors(child, text_color, border_color);
            }
        }
        
        ESP_LOGI(TAG, "Screen set to black background with gray-white text and dark gray border");
    }
    
    // 递归设置对象及其子对象的颜色
    void SetObjectColors(lv_obj_t* obj, lv_color_t text_color, lv_color_t border_color) {
        if (obj == nullptr) {
            return;
        }
        
        // 设置当前对象的背景颜色、文本颜色和边框颜色
        lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
        lv_obj_set_style_text_color(obj, text_color, 0);
        lv_obj_set_style_border_color(obj, border_color, 0);
        
        // 递归设置所有子对象
        uint32_t child_cnt = lv_obj_get_child_cnt(obj);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t* child = lv_obj_get_child(obj, i);
            if (child) {
                SetObjectColors(child, text_color, border_color);
            }
        }
    }

public:
    atk_dnesp32s3_box() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeATK_ST7789_80_Display();
        // 设置 XL9555 的输出状态
        xl9555_->SetOutputState(7, 0); // 将引脚 7 设置为低电平，关闭 LED 灯
        InitializeButtons();
        InitializeIot();
        
        // 设置屏幕为黑色
        SetScreenBlack();
    }
    
    ~atk_dnesp32s3_box() {
        if (button_task_handle_) {
            vTaskDelete(button_task_handle_);
            button_task_handle_ = nullptr;
        }
        if (xl9555_) {
            delete xl9555_;
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        static ATK_NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
    }
    
    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Led* GetLed() override {
        static AlwaysOffLed led;
        return &led;
    }
};

DECLARE_BOARD(atk_dnesp32s3_box);
