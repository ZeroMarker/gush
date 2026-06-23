# Gush 下一步计划

## P0 - 稳定性与数据正确性

- [ ] 修复多文件下载写入逻辑
  - 当前下载器按 `torrent_.name` 写单个输出文件，和 README 中的多文件解析能力不匹配。
  - 需要按 `TorrentInfo::files` 创建目录和文件，并把 piece/block 偏移映射到一个或多个目标文件。

- [ ] 增加 piece SHA1 校验
  - 当前 `verifyPiece()` 已声明但下载完成路径没有做完整校验。
  - 每个 piece 写入完成前应按 `.torrent` 中的 20 字节哈希校验，失败时重置该 piece 的请求/接收状态。

- [ ] 修复下载完成统计口径
  - 当前按完成 piece 数计算进度，最后一个 piece 可能小于标准 piece length。
  - 进度和 downloaded bytes 应以实际文件总长度为准，避免显示超过或低估。

- [ ] 收紧输出路径安全
  - 清理 torrent 文件名和多文件路径中的 `..`、绝对路径、空路径和平台危险字符。
  - 避免恶意 torrent 写出保存目录。

## P1 - 网络协议与下载效率

- [ ] 增加 DHT peer discovery
  - README 已说明 magnet 依赖 tracker，纯 hash magnet 成功率有限。
  - 先实现 BEP 5 基础节点查询，再接入 magnet 元数据下载。

- [ ] 实现 PEX 支持
  - 支持从已连接 peer 获取更多 peer，减少对 tracker 的依赖。

- [ ] 改进 tracker 策略
  - 给 tracker 加失败退避、成功优先级和并发联系上限。
  - 避免每次空闲时线性扫全部 tracker。

- [ ] 优化 piece selection
  - 现有 rarest-first 比较基础。
  - 增加 endgame mode，下载末尾允许重复请求慢块并及时 cancel。

- [ ] 增加 peer 健康管理
  - 按超时、坏块、choke 时间和吞吐量淘汰低质量 peer。
  - 限制重复连接和连接失败重试频率。

## P2 - 测试覆盖

- [ ] 补 Downloader 单元/集成测试
  - 使用本地 socket pair 或 mock peer 验证 block 请求、超时重试、piece 完成和写盘。

- [ ] 补 PeerConnection 半包/粘包测试
  - 覆盖 partial send/recv、keep-alive、超大消息、invalid block offset。

- [ ] 恢复并修正多文件 torrent 测试
  - `tests/test_torrent.cpp` 中已有被注释的多文件测试。
  - 改成稳定构造 bencode fixture 后启用。

- [ ] 增加 metadata downloader 测试
  - 覆盖 BEP 9 extension handshake、metadata 分片拼接、hash mismatch 和 peer reject。

- [ ] 增加 sanitizer CI
  - Debug CI 跑 ASan/UBSan。
  - 网络相关测试先保持本地 deterministic，不依赖公网 tracker。

## P3 - 工程化与用户体验

- [ ] 增加 `.gitignore`
  - 忽略 `build/`、编译产物、临时下载文件和编辑器缓存。

- [ ] 增加 CLI 参数
  - `--no-tracker-refresh`
  - `--max-peers`
  - `--download-dir`
  - `--verbose`

- [ ] 引入日志级别
  - 将散落的 `std::cout`/`std::cerr` 收口到简单 logger。
  - 默认输出保持简洁，debug 模式输出 tracker/peer 细节。

- [ ] 优化 README
  - 明确当前真实能力和限制。
  - 增加构建依赖安装示例、测试命令和已知问题。

- [ ] 增加 GitHub Actions
  - Linux Debug/Release 构建。
  - 运行 `ctest --output-on-failure`。

## 推荐执行顺序

1. 添加 `.gitignore` 和 CI，固定基础工程质量门槛。
2. 完成 piece SHA1 校验，防止错误数据落盘。
3. 修复单文件/多文件写入抽象，并恢复多文件测试。
4. 增加 Downloader/PeerConnection 本地协议测试。
5. 再推进 DHT、PEX、endgame mode 等协议能力。
