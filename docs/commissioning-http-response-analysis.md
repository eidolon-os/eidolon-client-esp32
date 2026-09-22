# 配网 HTTP 响应超时：离线分析与候选修复

状态：代码改动与离线验证完成；原实机故障根因仍未最终确认，未部署本轮改动。

## 已有证据与不能推出的结论

2026-09-21 三轮实机连接中，两轮在结果请求发生超时。设备已完成扫描，
HTTP endpoint 被调用，HTTP_SERVER_EVENT_SENT_DATA 有记录；手机没有完成
responseHeadersEnd。另一轮成功，但一个小响应延迟约 6.4 秒。

IDF 5.5.4 的 httpd_resp_send 在 send 返回成功后发布 SENT_DATA。send 成功
只表示 TCP 接受了数据，不能证明完整报文已经离开发送队列或到达手机。
手机 ss 中 Send-Q=0 仅说明手机发送方向的数据已被确认，不能证明设备发送
方向没有待确认数据。现有日志不能区分 ACK 丢失、设备下行丢包与无线兼容性；
也不能仅凭缺少 responseHeadersEnd 断言手机已收到部分响应头。

## 可离线重现的机制

同一 SDK 的 httpd_resp_send 分别写入基本响应头、结束空行和正文。默认
accepted TCP socket 未设置 TCP_NODELAY。lwIP 在前一小段尚未被确认时，
Nagle 会暂存后续小段；函数仍可返回成功，手机却无法解析完整响应头。

测试直接编译当前 SDK 的 lwIP TCP 核心，使用无 radio、无 RTOS 的虚拟 netif。
在连接窗口足够且对端不发送 ACK 的条件下，按 IDF 的三个写入边界发送：

| 策略 | 输出包 | 仍有待发送尾部 | 已输出完整 HTTP 头 |
| --- | ---: | --- | --- |
| 默认 Nagle | 1 | 是 | 否 |
| TCP_NODELAY | 3 | 否 | 是 |

这是对 ACK 依赖机制的确定性复现，不是对原实机 15 秒超时的完整复现。
尚未证明原现场存在持续的 ACK 丢失；普通 delayed ACK 的短延迟也不能自行
解释全部 6.4 秒或 15 秒现象。

## 改动边界

- 配网 HTTP server 的 open_fn 为每个新 socket 设置 TCP_NODELAY。
- 设置失败由 HTTP server 拒绝该 socket，不继续使用未应用策略的连接。
- 保留官方 HTTP、Security2 和 TCP 重传机制；不自动重放加密请求，不延长期限。
- 日志将 response sent 改为 response queued，避免再次误解为交付证据。
- 撤下源码中尚未验证的 HT20 实验，避免同时改变多个变量。设备上此前的
  实验固件没有被替换；本轮没有烧录或安装，也没有占用串口/ADB。

TCP_NODELAY 的代价是可能发送更多小 TCP 段，不能解决所有无线丢包。
它是有源码依据、可离线验证的传输策略修正；原故障是否消除仍待未来一次
明确授权的实机验证。本报告不将其标记为配网稳定性验收通过。

## 验证

- `IDF_PATH=<firmware SDK> bash tests/run_commissioning_http_socket_tests.sh`：
  真实主机 socket 选项和无效 socket 拒绝测试通过；SDK lwIP 对照通过。
- Android 原生 61 项测试全部通过，包括分段响应头与 169 字节分段正文、
  保持 TCP 连接时按 Content-Length 完成、响应头/正文不完整时超时并关闭、
  只完成一次及既有取消/访问隔离测试。
- StackChan 隔离固件构建通过。未混入音频任务改动，未安装/烧录。

本地证据：/private/tmp/provisioning-offline-tcp-tests.log、
/private/tmp/provisioning-offline-native-tests.log、
/private/tmp/provisioning-nodelay-build.log。
