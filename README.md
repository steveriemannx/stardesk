# StarDesk 远程控制

基于 dui(纯代码模式)的局域网远程控制软件,类似 ToDesk / 向日葵。所有界面与素材均为代码绘制(无 XML、无图片资源、无 CEF/Web)。

## 功能

- **屏幕共享**:复制(镜像主屏)/ 拓展(多显示器拼接一个画布)两种模式
- **画面传输**:64×64 瓦片增量检测 + PNG 压缩(dui 自带的 libwebp 无编码器,阶段一用 PNG),支持 原始 / 720p / 1080p 分辨率与 24 / 30 / 60 fps
- **键盘鼠标控制**:macOS CGEventPost(需辅助功能权限)/ Windows SendInput / Linux·FreeBSD XTest
- **观看模式**:只观看画面,不注入输入(主机端强制)
- **文件传输**:独立 TCP 通道 + 会话令牌,拖拽或文件对话框选择,双向传输,进度显示
- **连接认证**:SHA-256 挑战应答(密码不明文传输);自动接受或每次手动确认(弹窗)
- **局域网发现**:UDP 广播 beacon,设备列表一键填入
- **亮/暗主题 + 8 种语言**(简体中文、繁體中文、English、日本語、한국어、Deutsch、Français、Русский)
- **远程窗口全屏**(按钮 / ESC 退出)、适应窗口 / 原始大小、帧率与延迟显示

## 构建

StarDesk 是独立工程(本目录)。构建时需要一份 dui 源码树,用 CMake 变量
`DUI_SRC_ROOT_DIR` 指定;默认指向同级的 `../dui`(最新 dui main:原生后端
+ UTF-8 字符串 API,已不含 SDL 后端)。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DDUI_SRC_ROOT_DIR=/Users/steveriemannx/projects-main/dui
cmake --build build --target stardesk -j 8
open build/bin/stardesk.app
```

指定目录里的 dui 会作为子项目一起构建(只构建库,不构建 dui 的 examples / tests /
CEF);首次配置会构建 Skia,耗时较长,之后是增量构建。

如果要把本目录放回 dui 树里构建,直接拷成 `<dui>/examples/stardesk`(不要传
`DUI_SRC_ROOT_DIR`,它会自动识别 `../..` 为 dui 源码树)。

### 平台要求

| 平台 | 要求 |
|------|------|
| macOS | **14.0+**(ScreenCaptureKit);屏幕录制权限(画面)、辅助功能权限(远程输入)。当前验证平台 |
| Windows | 7 / 10 / 11(原生 Win32);源码保留,构建尚未验证 |
| Linux / FreeBSD | 原生 X11/Wayland 后端;源码保留,构建尚未验证 |

macOS 权限:系统设置 → 隐私与安全性 → 屏幕录制 / 辅助功能,勾选 StarDesk(或运行它的终端)。
未授予屏幕录制权限时,主机自动切换到内置「测试画面」以验证链路,并在状态栏提示。

## 使用

1. 启动两台机器上的 StarDesk(同一局域网 / 热点)。
2. **被控端**:左侧面板查看本机密码;可切换「自动接受 / 每次手动确认」与「复制 / 拓展」共享模式。
3. **控制端**:右侧「远程控制」标签,输入对端 IP 与密码(或点击发现列表中的设备),选择 控制/观看、分辨率、帧率,点「连接」。
4. 连接成功后进入远程窗口:适应窗口 / 原始大小 / 全屏 / 发送文件 / 断开。
5. 「文件」标签:一个「发送文件」按钮打开发送窗口(拖拽或浏览);接收的文件默认存到 `~/Downloads/StarDesk/`(可在配置文件修改)。

## 配置

配置文件 `~/.stardesk.conf`(UTF-8,key=value):

```
password=12345678      # 连接密码(界面可随机生成)
port=7456              # 主端口(文件端口 = 主端口 + 1)
manual_accept=0        # 1 = 每次手动确认
extend_screen=0        # 1 = 拓展(多屏拼接)
receive_dir=           # 文件接收目录(空 = ~/Downloads/StarDesk)
dark_theme=1
language=0             # 0-7,对应 8 种内置语言
want_fps=30            # 控制端默认帧率
want_res=0             # 0=原始 1=720p 2=1080p
device_name=           # 设备名(默认主机名)
headless=0             # 1 = 被控端无窗口运行，自动接受连接
```

### 无头被控端

没有显示器的 Linux/FreeBSD 机器可使用无头模式启动被控端：

```bash
STARDESK_HEADLESS=1 ./bin/stardesk
```

也可以在 `~/.stardesk.conf` 中设置 `headless=1`，再通过 systemd、rc.d
或其他启动管理器开机启动。该模式不创建本地控制窗口，但仍监听端口并自动
接受连接；控制端使用 `auto_host`、`auto_port` 和 `auto_password` 可在启动
后自动连接。

“无头”指没有物理显示器，不是没有桌面会话。远程端必须先启动真实桌面或
虚拟显示会话（例如 Xvfb/Xorg 虚拟输出，或已登录的 Wayland/Portal 会话），
桌面应用照常运行，StarDesk 从该会话采集真实画面。没有可用显示会话时无法
查看真实桌面，内置测试画面只能用于验证网络链路。

## 协议简述

二进制帧 `[magic "SDK1"][type u8][len u32][payload]`(小端)。认证:
`Hello → Challenge{nonce} → Auth{SHA256(nonce‖password)} → AuthResult{ok, filePort, token}`。
画面流:`ScreenInit{宽高}` + `ScreenTile{x,y,w,h,png}` + `ScreenEnd`(帧结束标记,客户端仅在收到后渲染,避免快速滑动时瓦片拼接)+ `CursorPos{归一化坐标}`。
输入:`InputEvent{kind, 归一化坐标/键码/修饰键}`。文件通道:独立 TCP(端口+1),
首帧 `FileAuth{token}`,随后 `FileStart/FileAccept/FileChunk/FileEnd/FileDone/FileAbort`。
心跳 3s Ping/Pong,15s 无响应断开。画面流阶段一不加密(局域网使用,README 说明)。

## 已知限制(阶段一)

- 画面流不加密;Linux 仅 X11;macOS 需 14+;一次一个控制会话
- PNG 编码对彩色界面带宽较高(瓦片增量已大幅缓解);后续可接入完整 libwebp/JPEG
- 采集:SCStream 连续流(60fps,镜像模式);多显示器拓展模式仍为单帧采集(约 5 fps)
- 全屏大幅变化(如视频)时 PNG 编码 + 带宽会限制帧率(1080p 局域网下约 30fps 内)
- 多显示器「拓展」模式在 macOS 上为并集画布(副屏布局差异未做坐标补偿)

## 迁移到独立仓库

本目录是一个独立的 git 仓库(嵌套在 dui 的 examples/ 下),不依赖 dui 的构建之外
的任何资源文件。迁移:

```bash
cd examples/stardesk
git remote add origin git@github.com:<your>/stardesk.git
git push -u origin main
```

独立构建时需提供 dui 与 Skia 的库和头文件(把 `DUI_SRC_ROOT_DIR` 指向 dui 仓库根目录即可)。
