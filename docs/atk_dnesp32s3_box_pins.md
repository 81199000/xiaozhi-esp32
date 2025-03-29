# ATK-DNESP32S3-BOX 开发板引脚说明

## 触摸屏信息

ATK-DNESP32S3-BOX 开发板配备了一个电容式触摸屏，与 ST7789 LCD 显示屏集成在一起。

### 显示屏硬件信息

- **显示屏控制器**：ST7789 LCD 控制器
- **显示分辨率**：320x240 像素
- **通信接口**：8080 并行接口 (8 位数据线)
- **显示方向**：默认配置为交换 XY 轴，镜像 X 轴

### 显示屏 IO 引脚连接

| 功能 | ESP32-S3 引脚 |
|------|--------------|
| LCD_CS | GPIO_NUM_1 |
| LCD_DC | GPIO_NUM_2 |
| LCD_RD | GPIO_NUM_41 |
| LCD_WR | GPIO_NUM_42 |
| LCD_RST | GPIO_NUM_NC (未连接) |
| LCD_D0 | GPIO_NUM_40 |
| LCD_D1 | GPIO_NUM_39 |
| LCD_D2 | GPIO_NUM_38 |
| LCD_D3 | GPIO_NUM_12 |
| LCD_D4 | GPIO_NUM_11 |
| LCD_D5 | GPIO_NUM_10 |
| LCD_D6 | GPIO_NUM_9 |
| LCD_D7 | GPIO_NUM_46 |

### 触摸屏信息

根据代码分析，ATK-DNESP32S3-BOX 开发板可能使用了电容触摸屏，但当前代码中没有直接实现触摸功能。触摸屏控制器可能是 GT911 或类似型号，通过 I2C 接口与 ESP32-S3 通信。

### 测试触摸屏是否可用

要测试触摸屏是否可用，需要添加触摸屏驱动程序。以下是一个基本的实现方案：

1. **添加触摸屏驱动依赖**：
   在项目的 `CMakeLists.txt` 中添加：
   ```cmake
   idf_component_register(
       ...
       REQUIRES esp_lcd_touch esp_lcd_touch_gt911
       ...
   )
   ```

2. **初始化触摸屏**：
   ```cpp
   // 在 atk_dnesp32s3_box 类中添加
   private:
       esp_lcd_touch_handle_t touch_handle_ = nullptr;
       
       void InitializeTouchScreen() {
           esp_lcd_touch_config_t tp_cfg = {
               .x_max = DISPLAY_WIDTH,
               .y_max = DISPLAY_HEIGHT,
               .rst_gpio_num = GPIO_NUM_NC, // 根据实际连接修改
               .int_gpio_num = GPIO_NUM_NC, // 根据实际连接修改
               .levels = {
                   .reset = 0,
                   .interrupt = 0,
               },
               .flags = {
                   .swap_xy = DISPLAY_SWAP_XY,
                   .mirror_x = DISPLAY_MIRROR_X,
                   .mirror_y = DISPLAY_MIRROR_Y,
               },
               .process_coordinates = NULL,
               .interrupt_callback = NULL,
           };
           
           esp_lcd_panel_io_handle_t tp_io_handle = NULL;
           esp_lcd_panel_io_i2c_config_t tp_io_config = {
               .dev_addr = 0x5D, // GT911 默认地址，可能需要修改
               .control_phase_bytes = 1,
               .lcd_cmd_bits = 16,
               .lcd_param_bits = 8,
               .dc_bit_offset = 0,
           };
           
           ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)i2c_bus_, &tp_io_config, &tp_io_handle));
           ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &touch_handle_));
       }
   
   public:
       // 在构造函数中调用
       atk_dnesp32s3_box() : boot_button_(BOOT_BUTTON_GPIO) {
           InitializeI2c();
           InitializeATK_ST7789_80_Display();
           InitializeTouchScreen(); // 添加这一行
           // ... 其他初始化代码 ...
       }
   ```

3. **创建触摸检测任务**：
   ```cpp
   private:
       TaskHandle_t touch_task_handle_ = nullptr;
       
       static void TouchTaskFunction(void* arg) {
           atk_dnesp32s3_box* board = static_cast<atk_dnesp32s3_box*>(arg);
           while (true) {
               board->CheckTouch();
               vTaskDelay(pdMS_TO_TICKS(20)); // 每20毫秒检测一次触摸状态
           }
       }
       
       void CheckTouch() {
           uint16_t touch_x[1];
           uint16_t touch_y[1];
           uint8_t touch_cnt = 0;
           
           esp_lcd_touch_read_data(touch_handle_);
           
           if (esp_lcd_touch_get_coordinates(touch_handle_, touch_x, touch_y, NULL, &touch_cnt, 1) == ESP_OK) {
               if (touch_cnt > 0) {
                   ESP_LOGI(TAG, "触摸点: x=%d, y=%d", touch_x[0], touch_y[0]);
                   
                   // 在触摸点绘制一个点或圆形
                   if (display_) {
                       // 这里可以添加绘制代码
                   }
               }
           }
       }
   ```

### 触摸屏驱动

ESP32-S3 的触摸屏驱动通常需要以下组件：

1. **ESP-IDF 组件**：
   - `esp_lcd_touch` - ESP LCD 触摸驱动框架
   - `esp_lcd_touch_gt911` - GT911 触摸控制器驱动

2. **驱动初始化流程**：
   - 初始化 I2C 总线 (已在 `InitializeI2c()` 中完成)
   - 配置触摸屏参数
   - 创建触摸检测任务

### 功能扩展方法

要为触摸屏添加新功能，可以考虑以下方法：

1. **基于 LVGL 的 UI 开发**：
   - 创建自定义 UI 组件
   - 实现触摸手势识别
   - 添加多点触控支持

2. **触摸屏手势识别**：
   ```cpp
   typedef enum {
       GESTURE_NONE,
       GESTURE_SWIPE_LEFT,
       GESTURE_SWIPE_RIGHT,
       GESTURE_SWIPE_UP,
       GESTURE_SWIPE_DOWN,
       GESTURE_TAP,
       GESTURE_DOUBLE_TAP,
   } touch_gesture_t;
   
   // 记录上一次触摸点
   static uint16_t last_x = 0;
   static uint16_t last_y = 0;
   static int64_t last_touch_time = 0;
   
   touch_gesture_t detect_gesture(uint16_t x, uint16_t y) {
       int64_t current_time = esp_timer_get_time() / 1000;
       touch_gesture_t gesture = GESTURE_NONE;
       
       // 计算移动距离
       int dx = x - last_x;
       int dy = y - last_y;
       
       // 判断手势类型
       if (abs(dx) > 50 && abs(dx) > abs(dy)) {
           gesture = (dx > 0) ? GESTURE_SWIPE_RIGHT : GESTURE_SWIPE_LEFT;
       } else if (abs(dy) > 50 && abs(dy) > abs(dx)) {
           gesture = (dy > 0) ? GESTURE_SWIPE_DOWN : GESTURE_SWIPE_UP;
       } else if (abs(dx) < 10 && abs(dy) < 10) {
           if (current_time - last_touch_time < 300) {
               gesture = GESTURE_DOUBLE_TAP;
           } else {
               gesture = GESTURE_TAP;
           }
       }
       
       // 更新上一次触摸信息
       last_x = x;
       last_y = y;
       last_touch_time = current_time;
       
       return gesture;
   }
   ```

3. **多点触控支持**：
   GT911 支持多点触控，可以通过读取多个触摸点来实现多点触控功能：
   ```cpp
   #define MAX_TOUCH_POINTS 5
   
   uint16_t touch_x[MAX_TOUCH_POINTS];
   uint16_t touch_y[MAX_TOUCH_POINTS];
   uint8_t touch_cnt = 0;
   
   esp_lcd_touch_read_data(touch_handle_);
   esp_lcd_touch_get_coordinates(touch_handle_, touch_x, touch_y, NULL, &touch_cnt, MAX_TOUCH_POINTS);
   
   for (int i = 0; i < touch_cnt; i++) {
       ESP_LOGI(TAG, "触摸点 %d: x=%d, y=%d", i, touch_x[i], touch_y[i]);
   }
   ```

## XL9555 扩展芯片引脚定义

XL9555 是一个 I2C 接口的 16 位 I/O 扩展芯片，在 ATK-DNESP32S3-BOX 开发板上用于扩展 GPIO 引脚。

### 寄存器说明

| 寄存器地址 | 功能描述 |
|------------|----------|
| 0x00       | 输入端口 0 (引脚 0-7) |
| 0x01       | 输入端口 1 (引脚 8-15) |
| 0x02       | 输出端口 0 (引脚 0-7) |
| 0x03       | 输出端口 1 (引脚 8-15) |
| 0x06       | 配置端口 0 (0=输入, 1=输出) |
| 0x07       | 配置端口 1 (0=输入, 1=输出) |

### 按键映射

| 按键名称 | XL9555 引脚 | 功能 |
|----------|-------------|------|
| K1       | 引脚 4 (IOQ_4) | 音量增加 |
| K2       | 引脚 3 (IOQ_3) | 音量减小 |

### 其他引脚用途

| XL9555 引脚 | 用途 |
|-------------|------|
| 引脚 7      | 输出高电平 (通过 `SetOutputState(7, 1)` 设置) |

## 按键检测逻辑

按键检测使用边缘检测方式，检测按键从释放到按下的变化：

```cpp
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
```

## XL9555 初始化配置

XL9555 的初始化配置如下：

```cpp
// 寄存器 0x06 和 0x07 是配置寄存器，0 表示输入，1 表示输出
// 确保引脚 3 和 4 被配置为输入模式 (0)
WriteReg(0x06, 0x07); // 0x07 = 0000 0111，引脚 3 和 4 为 0 (输入)
WriteReg(0x07, 0xFE);
```

这里 0x07 = 0000 0111 表示：
- 引脚 0, 1, 2 配置为输出模式 (1)
- 引脚 3, 4 配置为输入模式 (0)
- 引脚 5, 6, 7 配置为输出模式 (1)

## 注意事项

1. XL9555 的按键检测逻辑中，按键按下时为低电平，释放时为高电平，但 `GetInputState` 方法已经进行了电平转换，返回 true 表示按键按下，false 表示按键释放。

2. 按键检测任务每 100 毫秒执行一次，这个间隔可以根据需要调整。

3. 如果需要添加新的按键，需要在 XL9555 的初始化配置中将对应的引脚配置为输入模式 (0)。 