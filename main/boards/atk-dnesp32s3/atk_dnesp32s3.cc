#include "wifi_board.h"
#include "es8388_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "iot/thing_manager.h"
#include "led/single_led.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>

#define TAG "atk_dnesp32s3"

// 定义 K1 和 K2 按键对应的 XL9555 IO 引脚
#define K1_IO_PIN 5  // XL9555 的 IOQ_5 引脚
#define K2_IO_PIN 4  // XL9555 的 IOQ_4 引脚

// 音量控制参数
#define VOLUME_STEP 5  // 每次按键音量变化步长
#define MIN_VOLUME 0   // 最小音量
#define MAX_VOLUME 100 // 最大音量

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class XL9555 : public I2cDevice {
public:
    XL9555(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x06, 0x03);
        WriteReg(0x07, 0xF0);
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
};

// 自定义 XL9555 按键类
class XL9555Button {
private:
    XL9555* xl9555_;
    uint8_t io_pin_;
    bool last_state_ = false;
    TimerHandle_t debounce_timer_ = nullptr;
    
    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_click_;
    
    static void DebounceTimerCallback(TimerHandle_t timer) {
        XL9555Button* button = static_cast<XL9555Button*>(pvTimerGetTimerID(timer));
        button->CheckButtonState();
    }
    
    void CheckButtonState() {
        bool current_state = xl9555_->GetInputState(io_pin_);
        
        if (current_state != last_state_) {
            last_state_ = current_state;
            
            if (current_state) { // 按键按下
                if (on_press_down_) {
                    on_press_down_();
                }
            } else { // 按键释放
                if (on_press_up_) {
                    on_press_up_();
                }
                if (on_click_) {
                    on_click_();
                }
            }
        }
    }
    
public:
    XL9555Button(XL9555* xl9555, uint8_t io_pin) : xl9555_(xl9555), io_pin_(io_pin) {
        // 创建按键检测定时器，每 50ms 检测一次按键状态
        debounce_timer_ = xTimerCreate("btn_timer", pdMS_TO_TICKS(50), pdTRUE, this, DebounceTimerCallback);
        xTimerStart(debounce_timer_, 0);
    }
    
    ~XL9555Button() {
        if (debounce_timer_) {
            xTimerStop(debounce_timer_, 0);
            xTimerDelete(debounce_timer_, 0);
        }
    }
    
    void OnPressDown(std::function<void()> callback) {
        on_press_down_ = callback;
    }
    
    void OnPressUp(std::function<void()> callback) {
        on_press_up_ = callback;
    }
    
    void OnClick(std::function<void()> callback) {
        on_click_ = callback;
    }
};

class atk_dnesp32s3 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    XL9555Button* k1_button_ = nullptr;
    XL9555Button* k2_button_ = nullptr;
    LcdDisplay* display_;
    XL9555* xl9555_;
    int current_volume_ = 70; // 默认音量

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
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

    // Initialize spi peripheral
    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = LCD_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = LCD_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
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
        
        // 初始化 K1 按键 (音量+)
        k1_button_ = new XL9555Button(xl9555_, K1_IO_PIN);
        k1_button_->OnClick([this]() {
            IncreaseVolume();
        });
        
        // 初始化 K2 按键 (音量-)
        k2_button_ = new XL9555Button(xl9555_, K2_IO_PIN);
        k2_button_->OnClick([this]() {
            DecreaseVolume();
        });
    }
    
    // 增加音量
    void IncreaseVolume() {
        current_volume_ += VOLUME_STEP;
        if (current_volume_ > MAX_VOLUME) {
            current_volume_ = MAX_VOLUME;
        }
        
        ESP_LOGI(TAG, "Volume increased to: %d", current_volume_);
        GetAudioCodec()->SetOutputVolume(current_volume_);
        
        // 可以在这里添加显示音量的代码
        if (display_) {
            // 显示音量信息
            // display_->ShowVolume(current_volume_);
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
        
        // 可以在这里添加显示音量的代码
        if (display_) {
            // 显示音量信息
            // display_->ShowVolume(current_volume_);
        }
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        ESP_LOGD(TAG, "Install panel IO");
        // 液晶屏控制IO初始化
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = LCD_CS_PIN;
        io_config.dc_gpio_num = LCD_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 20 * 1000 * 1000;
        io_config.trans_queue_depth = 7;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io);

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel);
        
        esp_lcd_panel_reset(panel);
        xl9555_->SetOutputState(8, 1);
        xl9555_->SetOutputState(2, 0);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY); 
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    {
                                        .text_font = &font_puhui_20_4,
                                        .icon_font = &font_awesome_20_4,
                                        .emoji_font = font_emoji_64_init(),
                                    });
    }

    // 物联网初始化，添加对 AI 可见设备 
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
    }

public:
    atk_dnesp32s3() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        InitializeIot();
    }
    
    ~atk_dnesp32s3() {
        if (k1_button_) {
            delete k1_button_;
        }
        if (k2_button_) {
            delete k2_button_;
        }
        if (xl9555_) {
            delete xl9555_;
        }
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8388AudioCodec audio_codec(
            i2c_bus_, 
            I2C_NUM_0, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_NC, 
            AUDIO_CODEC_ES8388_ADDR
        );
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(atk_dnesp32s3);
