# 命令入口与远程客户端

`anof` 使用 CLI11 解析本地 init／serve 与远程客户端命令。客户端经 HTTP 访问服务，不读取实例数据库。已有实例的本机开发 HTTP 服务可这样启动：

```sh
cmake --build cmake-build-debug --target anof --parallel 2
./cmake-build-debug/apps/anof/anof serve --root /absolute/path/to/instance
./cmake-build-debug/apps/anof/anof serve --help
```

`serve --root` 必填，必须满足[实例路径与权限契约](../../anof/infrastructure/InstanceStartupContract.md)。serve 不创建实例或凭据；新实例通过 init 创建，远程命令通过 profile 绑定服务，再以 login 获得并保存 Session。配置必须显式设置 `development_http=true`，仅监听 `127.0.0.1`；该监听配置不用于远程部署。

[Main.cc](Main.cc) 负责命令解析；[Serve.cc](Serve.cc) 创建真实锁、TOML 和 SQLite 维护适配，调用 prepareInstance，再把持锁结果移交 InstanceServer。数据库维护在监听前完成，业务连接由 metadata worker 独立打开，密码资源在独立 worker 初始化；两者就绪后才启动 HTTP。根依赖包括 CLI11；服务目标链接 process_support、instance_runtime 和 infrastructure，不链接测试播种器。离线初始化通过 `anof_init_support` 和 `anof_instance_setup` 链接同一应用与基础设施实现。

## 远程命令

客户端库接口、HTTP／文件安全边界见 [client 维护说明](../../anof/client/README.md)。[ClientCommands](ClientCommands.h) 注册命令、验证参数并调度类型化 API；[ClientOutput](ClientOutput.h) 只从受检 DTO 构造输出。`anof_client_commands` 链接 client、terminal_support、CLI11 和 JSON，不依赖服务端实现。最终可执行文件仍因 init／serve 包含服务端能力。

以下 `anof` 指本次构建的可执行文件。公共选项必须放在子命令之前；远程调用必须显式选择 profile，没有默认账户。profile 管理使用位置 NAME，不接受 `--profile`；init／serve 不接受客户端公共选项。

```sh
anof --config-dir /absolute/client-config profile add admin --server http://127.0.0.1:8080 --development-http
anof --config-dir /absolute/client-config --profile admin login --login admin
anof --config-dir /absolute/client-config --profile admin --json invite create --max-uses 2
anof --config-dir /absolute/client-config profile add alice --server http://127.0.0.1:8080 --development-http
anof --config-dir /absolute/client-config --profile alice register --login alice --display-name 'Alice'
anof --config-dir /absolute/client-config --profile alice login --login alice
anof --config-dir /absolute/client-config --profile alice spaces
anof --config-dir /absolute/client-config --profile alice logout
```

register 从终端读取邀请码和两次新密码，成功不自动登录；login 读取一次密码，已有本地凭据时拒绝覆盖。所有秘密仅来自 `/dev/tty`，无参数、环境变量或 stdin 入口。profile 锁覆盖输入、HTTP、保存及输出。邀请码仅在 invite create 成功输出一次；登录输出不含 token。个人凭据以0600明文保存在相应 profile，显示／枚举经过脱敏。

| 命令 | 输入与结果 |
|---|---|
| profile add/show/remove NAME、profile list | 添加绑定、查看／枚举脱敏资料、本地删除；remove 不撤销 Session |
| register、login | `--login` 必填；register 还需 `--display-name` |
| logout、me、spaces | 撤销并清除凭据、查看当前用户、列出空间／根 UUID／额度 |
| invite create | `--max-uses` 默认1，`--valid-for-seconds` 默认604800 |
| invite list | `--limit` 默认100、最大200，`--cursor` 接前一页结果 |
| invite revoke ID | 撤销指定邀请 UUID |
| ls NODE_ID、stat NODE_ID | UUID 查询；ls 支持同样的 limit/cursor，单次只请求一页 |
| account rename | `--display-name NAME --if-match ETAG`，只改本人 |
| account password | `--if-match ETAG`；终端读取一次旧密码、两次新密码，成功撤销全部会话 |
| user list、user show | 管理员；list 接受 limit/cursor，show 必填 `--id USER_ID` |
| user enable/disable | 管理员；`--id USER_ID --if-match ETAG` |
| quota show | 管理员；`--space-id SPACE_ID [--user-id USER_ID]`，省略 user-id 表示空间额度 |
| quota set | 同上，另必填 `--bytes N --nodes N --if-match ETAG`，两项均为0..INT64_MAX规范十进制 |
| mkdir | 必填 `--space SPACE_ID --parent NODE_ID --name NAME`；不解释文件路径 |

账户写条件取自 me／user show／quota show 返回的完整强 ETag。shell 中以单引号包裹标签，例如 `--if-match '"user-10000000-0000-4000-8000-000000000001-1"'`；不会自动查询新标签或在412后重试。普通输出包含可复制的 ETag，JSON在meta.etag中提供。改密参数、profile及凭据检查先于终端提示，只有确认204后清除当前profile凭据；其他profile不改动，旧会话由服务器拒绝。拒绝或未知结果保留凭据，须显式核对／重新登录；清除失败与输出失败分别报告真实本地状态，退出5。

普通输出逐项显示字段、精确数量及分页状态，并转义控制字符。`--json` 成功只输出一行 `{ok:true,data,meta}` 到 stdout；失败只输出一行 `{ok:false,error}` 到 stderr。提示写入 `/dev/tty`；`--help` 始终是可读帮助，不访问配置、终端或网络。meta 包含 profile 及可用的 http_status/request_id/etag，数值量与 revision 保持协议的十进制字符串。

客户端退出码与下方本地命令独立：0成功；2参数／秘密输入非法；3合法应用拒绝或缺少凭据；4读取的传输／协议错误；5本地状态／输出失败；6写结果未知；130读取／输入被中断。远端未知结果不自动重试。登录保存失败会报告已确认的远端成功；登出即使远端失败仍尝试清除本地，其他401不清除凭据。

客户端在同步命令期间安装可恢复的信号处理器，SIGINT／SIGTERM／SIGHUP／SIGQUIT／SIGTSTP 设置 lock-free 取消标志，由 libcurl 进度回调观察；不会启动服务端停止线程。终端读取仍使用屏蔽集和 signalfd，退出时恢复终端并保留取消状态。SIGPIPE 在客户端作用域被忽略，输出失败返回5并保留已确认的远端及本地状态；部分输出已经写出时无法撤回，调用者须同时检查退出码。退出后恢复原信号策略。

## 创建实例与初始化收尾

参数解析及离线初始化已接入显式内容预算和磁盘余量；预算写入 SQLite，余量写入 TOML，三组内容目录在格式发布前创建并同步。

新实例的 root 必须已存在、为空、由运行用户所有且权限为 0700；路径不接受符号链接或 `..`。例如：

```sh
mkdir -m 700 /absolute/path/to/instance
./cmake-build-debug/apps/anof/anof init --root /absolute/path/to/instance \
  --admin-login admin --admin-display-name 'Administrator' --development-http \
  --content-budget-bytes 1099511627776 \
  --private-bytes 1099511627776 --private-nodes 100000 \
  --public-bytes 1099511627776 --public-nodes 100000 \
  --contribution-bytes 1099511627776 --contribution-nodes 100000
```

`--content-budget-bytes` 必填，范围 0～INT64_MAX；它与逻辑额度独立。`--disk-safety-min-bytes` 默认 1073741824，`--disk-safety-percent` 默认 5、范围 0～100；均使用规范十进制，`--finish` 禁止携带这些新建参数。

三组额度必须显式提供，bytes 为 0～INT64_MAX、nodes 为 1～INT64_MAX 的规范十进制整数；额度只是元数据限制，不预分配相应磁盘空间。`--max-users` 默认 10，范围 1～10000，包含管理员。显示名和登录名必填；开发 HTTP 必须显式启用，仅绑定回环地址。

密码只从 `/dev/tty` 无回显读取两次；不接受密码参数、环境变量、文件或 stdin 管道。无控制终端、确认不一致、非法 UTF-8、NUL、少于 12 或超过 1024 字节均拒绝，不裁剪空白。终端设置在成功、输入失败或可捕获的中断后恢复；输入阶段 SIGINT/SIGTERM/SIGHUP/SIGQUIT/SIGTSTP 中止输入。SIGKILL 等不可捕获终止不在恢复保证内。

[InitOptions.cc](InitOptions.cc) 验证参数，[Init.cc](Init.cc) 调用初始化，[TerminalPassword.cc](TerminalPassword.cc) 管理终端和秘密输入，随后调用同步离线初始化装配。新建密码策略为 Argon2id v19、64 MiB、2 次迭代、parallelism=1；启动维护、业务打开、Bootstrap 和密码验证共用接纳上限。该策略未作物理机性能校准。runtime 已实现独立有界密码 worker 和来源／登录名限流；login 命令通过同一 HTTP 接口认证。

初始化接口要求 schema/format 3，原子引导管理员、公共及私有空间、根和额度；随后发布格式标记并清理 pending。不会签发 Session 或邀请码。成功输出 `ANOF_INITIALIZED`，退出码 0，服务需另行执行 serve。参数/密码输入错误为 2；已存在、锁竞争、初始化/收尾失败为 3，并输出固定 stage/code，不回显秘密或配置。

引导已提交但尚未完成文件发布时使用：

```sh
./cmake-build-debug/apps/anof/anof init --root /absolute/path/to/instance --finish
```

`--finish` 仅可搭配 `--root`，不接收管理员、密码或额度输入。它检查同一当前版本的 pending、配置与已提交引导，然后发布/确认格式并清理 pending；不重设密码或重建用户。缺少 pending、未提交引导或版本不符会拒绝，不自动修补或删除现场。提交未知/发布未知不自动重试，也不宣称回滚。完成后再次 finish 会因没有 pending 而拒绝。

## 启动、停止及输出

程序在线程创建之前屏蔽 SIGINT／SIGTERM，完成实例准备、crypto 和 worker 初始化后才启动 HTTP。启动成功在 stdout 输出并刷新一行：

```text
ANOF_READY address=127.0.0.1 port=8080
```

配置 port=0 时，该行包含内核分配的实际端口。stdout 也可能包含 muduox 日志，不能将第一行输出直接当作就绪通知。就绪行只说明当前已完成启动，持续健康状态由 `/health/ready` 提供。

所属 loop 每 50ms 非阻塞读取停止信号。首次收到停止信号输出 `ANOF_STOPPING`，关闭业务准入并请求 HTTP 停止；重复信号不延长宽限、不强制终止运行中的数据库任务。HTTP 和 metadata 两侧屏障完成后退出 loop，在回调栈外销毁服务对象，最后释放 lease。服务和 loop 销毁后输出 `ANOF_STOPPED`；退出前恢复原信号屏蔽集。

同步维护期间不强行中断 SQLite；维护返回后检查已收到的信号，可在创建 worker 前直接退出。HTTP 宽限不是整个进程的强制退出期限；底层 worker I/O 未结束时继续持锁等待，由外部服务管理器制定独立终止策略。

可报告错误写入 stderr，形式为 `ANOF_ERROR stage=<固定阶段> code=<稳定分类>`。不输出配置内容、路径或异常 what()。命令行错误提示使用 `--help`。退出码如下：

| 退出码 | 含义 |
|---|---|
| 0 | 帮助输出或正常信号停止，包括监听前取消 |
| 2 | 命令或参数语法错误 |
| 3 | 实例准备失败，例如锁竞争、非法配置、缺失／损坏元数据 |
| 4 | 信号桥接初始化、适配构造、服务装配或启动失败，以及未分类入口异常 |
| 5 | 服务期间信号读取或通知异常 |

监听建立使用 muduox 的可恢复入口。端口占用输出 `ANOF_ERROR stage=startup code=CONFLICT`；权限不足为 `FORBIDDEN`，临时资源不足为 `RETRYABLE`，其余监听错误为 `INTERNAL_ERROR`，均退出 4，不自动重试或换端口。启动失败不会输出 `ANOF_READY`；等待 worker 退出和服务清理、析构实例并释放锁后输出 `ANOF_STOPPED`。日志输出沿用 muduox 默认行为，尚无独立的有界访问日志 worker。

## 停止信号组件

`anof_process_support` 提供 [ShutdownSignals](ShutdownSignals.h)，不安装异步信号处理器。`create()` 保存原屏蔽集并创建非阻塞、close-on-exec 的 signalfd；新 worker 继承信号屏蔽集。进程只能存在一个该对象，重复创建返回 Conflict。

`poll()` 一次最多读取两条记录，返回 true 后保持为 true；读取失败返回 InternalError，跨线程调用返回 InvalidArgument。它不直接调用 runtime。必须在所有 worker 退出后由创建线程销毁，关闭 fd 并恢复原屏蔽集；恢复后到达的信号遵循原进程策略。它不能与其他 SIGINT／SIGTERM 消费者并用，也不能补救此前启动且未屏蔽信号的线程。

## 测试入口

[信号测试](tests/ShutdownSignals_unittest.cc) 检查信号读取、重复注册、屏蔽集恢复、线程继承和跨线程误用。[进程集成测试](../../tests/integration/ServeProcess_unittest.cc) 启动真实可执行文件，覆盖帮助／错误参数、实际 HTTP、无默认认证、第二进程拒绝、准备失败、SIGINT／SIGTERM 停止、活动连接清理及重启；独立子进程禁用 core dump 后验证现有端口冲突行为。[实例服务测试](../../tests/integration/InstanceServer_unittest.cc) 还通过真实 lease 和 worker gate 检查初始化取消与析构释放顺序。

```sh
cmake --build cmake-build-debug --target anof_serve_process_integration_unittest anof_instance_server_integration_unittest --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R 'anof_(serve_process|instance_server)_integration_unittest' -j 1
```

测试只使用私有临时实例和小型数据库，不提供生产初始化捷径，也不证明物理机掉电持久性或组合背压行为。

[初始化参数测试](tests/Init_unittest.cc)、[终端密码测试](tests/TerminalPassword_unittest.cc) 分别检查受检数量及真实 PTY 的无回显、确认、超限与中断恢复。[初始化集成测试](../../tests/integration/InitializeInstance_unittest.cc) 检查真实密码、初始额度、准备门禁及发布失败后的 finish。初始化用例的逐阶段失败/异常覆盖见 [InitializeInstance_unittest.cc](../../anof/application/maintenance/tests/InitializeInstance_unittest.cc)。

[客户端进程验收](../../tests/integration/ClientProcess_unittest.cc) 通过真实可执行文件完成 init／serve、管理员邀请、两个独立用户注册登录、分页、私有空间隔离、额度拒绝、会话撤销和重启持久化。另以回环受控 peer 验证中断分类、真实 rename 失败、登录保存后的 EPIPE；PTY 验证秘密无回显、锁在提示前生效、超限／确认失败及终端恢复。

```sh
cmake --build cmake-build-debug --target anof_client_process_integration_unittest --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_client_process_integration_unittest$' -j 1
```

这些测试不证明公网部署、TLS 反向代理配置、大文件吞吐或断电持久性。


## 上传、下载与续传命令

[TransferCommands](TransferCommands.h) 使用独立客户端库，不读取服务端数据库。公共选项仍放在子命令前，
远端命令必须指定profile；源和输出为本地绝对路径，服务器目标使用空间／节点ID。

```sh
anof --profile family upload /home/me/photo.jpg --space SPACE_ID --parent PARENT_ID --name photo.jpg
anof --profile family upload resume TRANSFER_ID
anof --profile family upload list --limit 100
anof --profile family upload show UPLOAD_ID
anof --profile family upload complete UPLOAD_ID
anof --profile family upload cancel UPLOAD_ID
anof --profile family download NODE_ID --output /home/me/saved.jpg
anof --profile family download resume TRANSFER_ID
anof --profile family storage show
anof --profile family storage set --content-budget-bytes 1073741824 --if-match '"storage-INSTANCE_ID-REVISION"'
```

upload complete可同时指定--parent和--name修正目标；202只代表接纳。高层upload结果包含published、upload_id、
transfer_id、state及node_id，不将后台处理视为已发布。续传失败诊断显示transfer_id，记录位于配置根的transfers目录。
未知写结果退出6并要求稍后显式resume，不自动重发或重新创建会话。源文件变更拒绝恢复。
下载只接受不存在的输出目标，保留受控part及记录用于恢复；成功前校验全文件SHA-256，不根据Content-Disposition选择路径。
本地保存／输出错误退出5，下载网络／协议／摘要错误退出4；发布不确定时保留现场，不覆盖或清理用户文件。

[传输进程测试](../../tests/integration/TransferClientProcess_unittest.cc) 通过真实可执行文件验证部分上传的跨进程恢复、
下载未确认尾部截断及Range恢复、服务重启、空文件、源变更和发布记录恢复。文件安全和故障语义另见
[客户端说明](../../anof/client/README.md)。
