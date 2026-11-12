# StarDesk Wayland ScreenCast 性能修复记录

## 现象
远程桌面（StarDesk 主机端，KDE Wayland，NVIDIA）帧率只有约 3 FPS，且日志反复出现
`Screenshot call-fail: Did not receive a reply`，回退到 `Screenshot polling mode active (prefetch)`。
视频/音频流本身已连通（H.264 + Opus），输入（EIS）也已生效，问题只在“画面采集”这一环。

## 根因
`examples/stardesk/src/platform/wayland/Capturer_wayland.cpp` 中解析
`org.freedesktop.portal.ScreenCast` 的 `Start` 返回时写错了结构。

Portal 规范里 `streams` 的类型是 `a(ua{sv})`：每个 stream 是
`(uint32 node_id, 字典 元数据)`。**`node_id` 是结构体的第 1 个字段（一个 uint32）**。

旧代码把它当成结构体里一个名为 `"node_id"` 的字典键值去查找：

```cpp
dbus_message_iter_next(&s0);                       // 跳过 node_id
if (arg_type == DBUS_TYPE_ARRAY) { 遍历字典找 "node_id" ... }
```

而 KDE 返回的字典只有 `position / size / source_type`，没有 `"node_id"` 键，
于是 `nodeIdStr` 始终为空 → 判定 `Start timed out` → 永久回退到截图轮询（约 3 FPS）。

## 修复
1. 按规范从 stream 结构体第 1 个字段读取 `node_id`（`uint32`），字典键 `"node_id"` 仅作兼容兜底。
2. 为 `org.freedesktop.portal.Request.Response` 注册 D-Bus match 规则并 flush，否则 libdbus 默认丢弃信号。
3. 修正 `SelectSources`/`Start` 把 `std::string*` 当 `const char**` 传给 D-Bus 的写法。

## 验证
远程干净重启服务 + 本地自动连接，日志出现：
```
[capture] ScreenCast node_id=83
[capture] ScreenCast stream active (portal)
[host] capture diag: got=920 fail=18     // 约 60 FPS（PipeWire + EGL DMA-BUF 直采 1440x900）
```
不再出现 `Screenshot polling` 回退，输入/音频正常。保持纯 Wayland（无 X11、无 SDL）。

## 远程构建/运行要点
- 远程：`shxu@192.168.150.232`，SSH 密码 `123456`（不是 StarDesk 应用密码）。
- StarDesk 应用密码（远程 `~/.stardesk.conf`）：`21236728`，端口 `7456`。
- 远程构建：`cmake --build ~/projects-main/dui/build-wayland --target stardesk -j 4`
  （纯 Wayland，`DUI_ENABLE_WAYLAND=ON`；修改后需 `touch` 源文件再 build 才会重编）。
- 远程服务：`systemctl --user restart stardesk.service`（已 enable-linger）。
- 本地连接：`STARDESK_AUTO_HOST=192.168.150.232 STARDESK_AUTO_PORT=7456
  STARDESK_AUTO_PASSWORD=<远程密码> STARDESK_AUTO_RES=1 STARDESK_AUTO_FPS=60 STARDESK_CODEC=1 ./bin/stardesk`
- 注意：服务一旦回退到截图模式不会自动重试，调试 ScreenCast 时需**先重启远程服务再连接本地**。
- 远程日志：`journalctl --user -u stardesk.service`；本地日志：`/tmp/stardesk-client-remote.log`。
