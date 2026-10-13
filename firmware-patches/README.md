# 固件补丁备份（managed_components 被 gitignore，组件更新会覆盖，需手动重打）

## esp_tcp.cc（对应 78/esp-ml307 组件，StackChan知识库问答方案 v3.1 第 13.4 节）
第 13 章「设备连接不上」根因补丁，三处：
1. `EspTcp::Connect` 检查 `xTaskCreate` 返回值——原实现不检查，内部 RAM 不足时
   接收任务静默创建失败，连接变只发不收（101 响应没人读 → 10 秒握手超时死循环）。
   失败时打印 free internal heap 并快速中止，上层自动重试自愈。
2. `DoDisconnect` 先 `shutdown(fd, SHUT_RDWR)` 再 `close()`（close 不会唤醒阻塞在
   recv 上的任务）。
3. socket 设 `SO_RCVTIMEO=1s`，接收任务轮询 `connected_` 标志退出。

**重打方法**：组件更新/重新拉取后，用本目录的 `esp_tcp.cc` 覆盖
`managed_components/78__esp-ml307/src/esp/esp_tcp.cc`，重新 `idf.py build`。
注意核对组件版本差异（本备份基于当前锁定版本）。
