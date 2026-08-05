# Gush 下一步计划

## P0 - 稳定性与数据正确性

- [x] 修复多文件下载写入逻辑
  - 下载器按 `TorrentInfo::files` 创建目录和文件，block 偏移映射到一个或多个目标文件。

- [x] 增加 piece SHA1 校验
  - 每个 piece 写入完成前按 `.torrent` 中的 20 字节哈希校验，失败时重置该 piece 的请求/接收状态。

- [x] 修复下载完成统计口径
  - 进度与 downloaded bytes 以实际文件总长度为准（`downloadedBytes_ / totalLength`），
    不再按 piece 数估算，最后一个短 piece 不会导致显示偏差。

- [x] 收紧输出路径安全
  - `sanitizePath`/`sanitizeFileName` 过滤 `..`、绝对路径、空路径与平台危险字符。

## P1 - 网络协议与下载效率

- [ ] 增加 DHT peer discovery
  - README 已说明 magnet 依赖 tracker，纯 hash magnet 成功率有限。
  - 先实现 BEP 5 基础节点查询，再接入 magnet 元数据下载。

- [ ] 实现 PEX 支持
  - 支持从已连接 peer 获取更多 peer，减少对 tracker 的依赖。

- [x] 改进 tracker 策略
  - `TrackerManager`：指数退避（30s 起，上限 30min）、成功优先级（上次成功的 tracker 优先）、
    每周期尝试上限（默认 3 次），避免死 tracker 拖慢下载循环。

- [x] 优化 piece selection
  - rarest-first + partial completion 优先级保持不变；
  - 增加 endgame mode：剩余 block 数 <= 64 时允许重复请求（每个 block 最多 2 份），
    收到任一副本后向其他 peer 发送 cancel 并清理 pending 请求。

- [x] 增加 peer 健康管理
  - 定期淘汰坏 peer（失败请求 >= 5 且无成功、或连接 > 60s 无数据）；
  - 被淘汰/连接失败的 peer 进入 5 分钟冷却名单，避免热重连。

## P2 - 测试覆盖

- [x] 补 Downloader 单元/集成测试
  - `tests/test_downloader.cpp`：本地 mock peer（socket）验证 handshake 分片接收、
    半包 block 重组、block 请求、piece SHA1 校验、单文件写盘、多文件跨边界写盘。

- [~] 补 PeerConnection 半包/粘包测试
  - 接收侧半包重组已由 Downloader 集成测试覆盖（mock peer 分片发送）；
  - 仍缺：PeerConnection 独立测试（keep-alive、超大消息、invalid block offset）。

- [ ] 恢复并修正多文件 torrent 测试
  - `tests/test_torrent.cpp` 中已有被注释的多文件测试。
  - 改成稳定构造 bencode fixture 后启用。

- [ ] 增加 metadata downloader 测试
  - 覆盖 BEP 9 extension handshake、metadata 分片拼接、hash mismatch 和 peer reject。

- [x] 增加 TrackerManager 单元测试
  - 覆盖退避、成功复位、优先级排序、去重和超时恢复。

- [x] 增加 sanitizer CI
  - `.github/workflows/ci.yml`：Release 构建 + ASan/UBSan Debug 构建，均跑 `ctest`。

## P3 - 工程化与用户体验

- [x] 增加 `.gitignore`
  - 忽略 `build/`、编译产物、临时下载文件和编辑器缓存。

- [x] 增加 CLI 参数
  - `--no-tracker-refresh`
  - `--max-peers <n>`
  - `-o/--download-dir <dir>`
  - `-v/--verbose`
  - `-h/--help`

- [x] 引入日志级别
  - `utils::log`（Error/Warn/Info/Debug），默认 Info，`--verbose` 打开 Debug；
  - downloader/tracker_list/metadata 的散落输出已收口。

- [x] 优化 README
  - 明确当前真实能力和限制，增加 CLI 选项表、测试命令和项目结构。

- [x] 增加 GitHub Actions
  - Linux Debug/Release 构建 + 运行 `ctest --output-on-failure` + ASan/UBSan。

- [x] 修复下载核心 bug（本轮 ASan + 集成测试发现）
  - `fopen(..., "wb")` 只写模式导致 `verifyPiece` 的 fread 永远失败（UB），
    所有 piece 校验必然失败、下载永远无法完成 → 改为 `"w+b"`。
  - piece hash 失败时清理 `pendingRequests_` 会使当前迭代器失效，随后双重
    erase 导致崩溃 → 先消费当前请求再清理。
  - `updateSpeedStats` 的采样值被下载循环每 100ms 覆盖，速度恒为 0 →
    idle 检测改用独立局部快照。

## 推荐执行顺序

1. ~~添加 `.gitignore` 和 CI，固定基础工程质量门槛。~~
2. ~~完成 piece SHA1 校验，防止错误数据落盘。~~
3. ~~修复单文件/多文件写入抽象，并恢复多文件测试。~~
4. ~~增加 Downloader/PeerConnection 本地协议测试。~~（Downloader/Peer 测试仍待补）
5. 再推进 DHT、PEX、endgame mode 等协议能力。（endgame 已完成，DHT/PEX 待做）
