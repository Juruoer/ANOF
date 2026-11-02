# 验证 Web 改动

Web 测试分为 Node 组件测试和真实浏览器测试。修改上传恢复逻辑时，先用组件测试检查状态、请求次数和失败处理，再通过私有 HTTPS 入口检查浏览器中的文件选择、Worker、IndexedDB 和 Cookie。两层测试回答的问题不同；模拟 fetch 通过不能代替浏览器实际下载。

工具与源码准备见[构建说明](../BUILDING.md)，页面操作见 [README](README.md)，实现约束见 [DESIGN](DESIGN.md)。以下命令均从 `web/` 执行。已有依赖应与 package-lock 和 toolchain-lock 一致；本页不另设安装版本或自动下载后端。

## 组件检查

准备好锁定的 Node/npm 和 npm 依赖后执行：

```sh
./scripts/npm.sh run tools:check
./scripts/npm.sh run build
./scripts/npm.sh run test:unit
```

`tools:check` 核对运行工具、包版本、许可和锁记录；`build` 先运行 TypeScript 类型检查，再生成浏览器测试使用的 `dist/`。单独检查类型可以用 `./scripts/npm.sh run typecheck`。

[package.json](package.json) 的 `test:unit` 使用 Node 自带测试运行器，选择 `tests/unit/*.test.mjs`，并发数为 1、单测试期限为 30 秒。静态边界测试另设十秒期限，并启动真实的本地 Vite HTTP 服务，因此执行环境需要允许监听回环端口。`support.mjs` 是数据与响应辅助文件，不是单独测试入口。

| 文件 | 检查内容 |
| --- | --- |
| [api.test.mjs](tests/unit/api.test.mjs) | 十进制数与 ETag、响应解码和字节上限、同源请求、准入、期限及未知结果分类 |
| [identity.test.mjs](tests/unit/identity.test.mjs) | 身份引导、秘密不入状态、迟到响应、登出门控、改密与资料冲突 |
| [drive.test.mjs](tests/unit/drive.test.mjs) | 文件与回收站解码、权限和分页、条件写入、查询核对与迟到结果 |
| [admin.test.mjs](tests/unit/admin.test.mjs) | 管理员范围、额度和存储统计、邀请码寿命、条件更新及身份销毁 |
| [transfer.test.mjs](tests/unit/transfer.test.mjs) | Upload/HEAD 绑定、分块与队列、哈希确认、记录故障、未知结果、恢复及终态本地时间 |
| [static-boundary.test.mjs](tests/unit/static-boundary.test.mjs) | Vite 的 API/health 拒绝、Host/CORS 和源码文件隔离 |
| [test-reporting.test.mjs](tests/unit/test-reporting.test.mjs) | 错误脱敏后保留失败与位置，不残留凭据文本 |

这里的 fake API、记录存储和 Worker 用于精确控制失败时机与计数。真实 SHA-256、IDB 结构和跨标签页竞争仍需下一层验证。

## 准备隔离浏览器环境

浏览器测试需要已有的 ANOF、nginx、OpenSSL、NSS certutil 和 Python 3，以及锁定的 Chromium Headless Shell、Firefox 及其 Linux 运行库。ANOF 编译方式见[构建说明](../BUILDING.md)。缺少这些前提会使测试失败，不能当作跳过后通过。

需要下载测试浏览器时，先安排缓存和解压空间，再明确执行下面的安装命令；正常 Web 构建不会触发它：

```sh
./scripts/npm.sh run browsers:install
./scripts/npm.sh run browsers:check
```

`browsers:check` 只验证浏览器版本、启动和基础 JavaScript 能力，不访问产品端点。版本、下载来源和已记录的 Ubuntu 运行库校验值见 [toolchain-lock.json](toolchain-lock.json)。私有 `.tools/browser-libs` 存在时，browser-node 脚本仅为本次进程选择其中的库，不修改系统加载路径。已有系统运行库也可使用。

浏览器缓存默认在 `.tools/browsers`，npm 缓存默认在 `.tools/npm-cache`；已有的 `PLAYWRIGHT_BROWSERS_PATH` 和 `npm_config_cache` 会保留。Node/npm 选择规则统一见[构建说明](../BUILDING.md#nodejs)，不要为测试额外引入 Windows 工具或依赖下载器。

## 执行真实浏览器测试

先将示例中的占位路径替换为本机已有可执行文件的绝对路径，确认 OpenSSL 的实际位置，然后执行：

```sh
export ANOF_TEST_ANOF=/absolute/path/to/anof
export ANOF_TEST_NGINX=/absolute/path/to/nginx
export ANOF_TEST_OPENSSL=/usr/bin/openssl
export ANOF_TEST_CERTUTIL=/absolute/path/to/certutil
./scripts/npm.sh run build
./scripts/npm.sh run test:e2e
```

[playwright.config.mjs](tests/e2e/playwright.config.mjs) 从 `tests/e2e` 发现 `*.spec.mjs`，使用一个 worker，先 Chromium 后 Firefox，零重试；Firefox project 依赖 Chromium。普通用例期限为 60 秒，传输用例为 120 秒，全局预算按两个引擎所有用例最大期限之和再加一分钟计算。首次失败停止后续执行，未执行的项目不能算通过。

只盘点注册项、不启动产品夹具时，可以运行：

```sh
./scripts/npm.sh run test:e2e -- --list --reporter=list
```

定向诊断可以使用 Playwright 的 `--project` 和 `--grep`。选择 Firefox 默认还会执行它的 Chromium 依赖；只有明确做独立诊断时才加 `--no-deps`，这类执行不构成双引擎完整结果。

### 夹具怎样启动和退出

[fixture.mjs](tests/e2e/fixture.mjs) 启动 [server.py](tests/e2e/server.py)，后者建立私有临时实例、CA 和 nginx HTTPS 入口，静态资源来自已有 `dist/`。每个浏览器上下文使用独立 profile；信任只导入该测试上下文所用的 NSS 库。测试保持 TLS 校验，也检查不信任 CA 的浏览器无法访问同一入口，不向系统或登录用户证书库安装 CA。

页面通过真实 API 操作后端。测试结束时关闭上下文，通知 Python 停止私有进程，检查日志并清理本次临时实例、证书和 profile。启动、浏览器操作与退出都有期限；不启动系统服务，不借用生产实例，不清理共享构建目录。

夹具限流保持开启，需要连续认证的场景按服务端节奏安排；数据库结构场景复用已验证的测试会话，同时用隔离 profile 分离本地数据库。不能通过关限流、加重试或放宽用例期限来隐藏失败。

### 各类浏览器证据

| 文件 | 浏览器边界 |
| --- | --- |
| [account.spec.mjs](tests/e2e/account.spec.mjs) | 私有 CA、实际 Cookie、登录注册改密登出、身份冲突、未知登出与生命周期门控 |
| [admin.spec.mjs](tests/e2e/admin.spec.mjs) | 邀请交接、停用账户的会话失效、管理额度、私有数据隔离、分页与秘密清理 |
| [drive.spec.mjs](tests/e2e/drive.spec.mjs) | 真实目录与文件操作、搜索和回收站、分页、双用户权限及未知写入 |
| [security.spec.mjs](tests/e2e/security.spec.mjs) | Strict Cookie 跨站导航、表单/空 Origin 拒绝、CSP、撤销/到期与未标记写入 |
| [transfer.spec.mjs](tests/e2e/transfer.spec.mjs) | 真 Worker 上传和原生下载、响应丢失恢复、READY 决策、跨标签锁、IDB 容量/结构/损坏、暂停取消、身份失效与本地时钟 |

响应丢失是在 fetch 边界丢弃已取得的响应；Worker 背压、存储写失败、缺 Web Locks、浏览器 Date 偏移都由测试明确注入。它们验证指定失败路径，不覆盖所有网络或磁盘故障。

visibility/pageshow 的受控事件验证自动身份复验及未知登出门控，不代表真实桌面标签切换或实际 BFCache 命中。Cookie 到期同样通过测试驱动控制，不等待真实七天。应保留这些区别，不能把 HTTP 探针或事件注入改写为用户环境实测。

实际下载只在隔离传输上下文开启。测试激活原生链接，由外部运行器读取保存的小文件，核对长度、摘要和文件名；HTML 内容还检查附件不执行。HEAD、Range、If-Range 和条件请求属于另外的 HTTPS 协议探针，不能取代实际下载证据。私有身份或旧 Cookie 被拒绝也不能记成下载成功。

## 失败诊断与资源

[safe-reporter.mjs](tests/e2e/safe-reporter.mjs) 保存 `test-results/e2e/summary.json`，包含项目、用例、状态与重试次数。错误先经 [sanitize-errors.mjs](tests/e2e/sanitize-errors.mjs) 去掉可能含密码或邀请码的原始文本，保留失败类别和源码位置。登录夹具还记录固定的身份引导/登录 HTTP 状态码。原始调用日志、自动页面快照、截图、trace 和视频均不保存；脱敏不改变失败状态。

夹具会检查后端、代理日志是否出现本轮密码、邀请码和观察到的会话秘密。排查时也不要将 Cookie、表单值或请求正文直接写入报告。完整执行以前应保留所需的旧失败摘要，后续执行会覆盖默认摘要文件；单次通过并不能解释此前的偶发失败。

测试使用 Linux 私有临时目录，普通传输为 KiB/MiB 级，1 TiB 边界用合成数值验证。包括缓存、日志和副本在内，每个生成文件逻辑大小不超过 2 GiB，累计生成量低于 50 GiB，普通夹具目标低于 1 GiB；这些是测试资源约束，不是产品文件容量。小文件和 headless 测试不建立大文件吞吐、物理掉电、目标主机服务或干净远程环境验收。
