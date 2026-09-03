# Cardputer ADV User Demo (kaichen fork)

基于 [m5stack/M5Cardputer-UserDemo](https://github.com/m5stack/M5Cardputer-UserDemo) 的 `CardputerADV` 分支，在原有 Demo 之上新增两个 Grove 外设演示 App：

| App | 外设 | 说明 |
|---|---|---|
| Gamepad | Unit Joystick2 + Unit Dual Button | FC 手柄样式，方向和 A/B 按下高亮 |
| Camera | Unit CamS3 5MP | 通过 Wi-Fi 拉取摄像头画面实时取景 |

上游原版 README 备份在 [`README.upstream.md`](./README.upstream.md)。

## 新增 App

### Gamepad

源码：`main/apps/app_gamepad/`

- 画面是一个 FC 手柄：左侧十字键，右侧红色 A/B 圆键，中间 Select/Start。
- Joystick2（I2C 地址 0x63）：推杆超过死区就点亮对应方向键，摇杆按钮映射为 A。推杆时按钮 RGB 灯亮红。中间还有一个模拟量位置小圆点。顶部显示 16 位原始 X/Y 值。
- Dual Button：蓝键映射为 B，红键映射为 A。
- 一次只接一个 Unit，自动识别、支持热插拔：打开时先在 I2C 上探测 0x63，找到即摇杆模式；否则释放 I2C，把 G1/G2 配成上拉输入进入双键模式，之后每 2 秒重新探测摇杆。
- 两个 Unit 不能通过 Grove Hub 同时使用。Dual Button 每根信号线上有 100nF 去抖电容（原理图 C1/C3），挂在 SCL/SDA 上会把 I2C 边沿拖垮，摇杆会时断时续。
- Home 键退出。

可调常量都在 `app_gamepad.cpp` 顶部：

| 常量 | 默认值 | 用途 |
|---|---|---|
| `JOY_CENTER` | 32768 | 摇杆中心值（16 位 ADC） |
| `JOY_DEAD_ZONE` | 12000 | 方向判定死区 |
| `JOY_INVERT_X` / `JOY_INVERT_Y` | false | 方向反了就改成 true |
| `GROVE_PIN_SCL` / `GROVE_PIN_SDA` | G1 / G2 | 蓝键、红键对应的 Grove 引脚 |

### Camera

源码：`main/apps/app_camera/`

配合 Unit CamS3 5MP 的出厂固件使用。CamS3 上电后自己开一个开放热点 `UnitCamS3-WiFi`（IP 192.168.4.1），Cardputer 连上去后通过 HTTP 接口取 JPEG。

- 打开 App 后自动连接热点，连上后先把摄像头切到 QVGA 320x240，然后后台任务循环拉取 `/api/v1/capture`。
- 画面横向铺满 204 像素画布，上下裁掉少量边缘。左上角显示帧率和帧大小。
- 热点找不到或摄像头连续无响应时显示红色提示，每 5 秒自动重试。
- Home 键退出，退出时断开 Wi-Fi。

注意：

- Grove 线在这里只给 CamS3 供电，画面走 Wi-Fi。先等 CamS3 的 LED 亮起（热点就绪）再进 App。
- 进 Camera 会断开 Cardputer 当前的路由器连接，退出后不会自动连回。需要时用 SetWiFi App 重连。
- Cardputer ADV 没有 PSRAM，单帧缓冲 32KB。如果串口日志出现 `frame too large`，说明摄像头还在输出大图。

## 构建

### 拉取依赖

```bash
python3 ./fetch_repos.py
```

### 工具链

[ESP-IDF v5.4.2](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32s3/index.html)

macOS 安装示例：

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4.2 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

每个新终端先执行：

```bash
source ~/esp/esp-idf/export.sh
```

### 编译

```bash
idf.py build
```

### 烧录

Cardputer ADV 用 USB-C 直连，串口通常是 `/dev/cu.usbmodemXXXX`。

```bash
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

退出 monitor 按 `Ctrl+]`。在 monitor 里按 `Ctrl+T` 再按 `Ctrl+F` 可以直接重新编译并烧录。

## 参考

- Unit Joystick2 协议：https://github.com/m5stack/M5Unit-Joystick2
- Unit CamS3 5MP 出厂固件：https://github.com/m5stack/UnitCamS3-UserDemo/tree/unitcams3-5mp

其余致谢见 [`README.upstream.md`](./README.upstream.md)。
