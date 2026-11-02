# 命令参考

下面的 `anof` 指已构建或安装的程序。首次安装见 [部署说明](../../deploy/README.md)，某个命令的完整参数可以用
`anof COMMAND --help` 查看。

远程命令操作服务器上的文件和账户，使用 `--profile` 选择连接。本地命令直接维护服务器实例，使用 `--root` 选择目录，两类选项不要混用。

## 远程命令

先保存服务地址，再登录。以下目录由你自行选择，示例域名请换成实际地址：

```sh
anof --config-dir /absolute/client-config profile add family --server https://cloud.example.test
anof --config-dir /absolute/client-config --profile family login --login admin
anof --config-dir /absolute/client-config --profile family --json me
anof --config-dir /absolute/client-config --profile family spaces
```

`--config-dir`、`--profile` 和 `--json` 放在子命令之前。profile 管理命令用位置参数 NAME，不用 `--profile`。不指定配置目录时，使用
`$XDG_CONFIG_HOME/anof`，其次是 `$HOME/.config/anof`。

密码和邀请码通过终端输入，不放在命令参数或环境变量中。只在本机开发时，才对 `profile add` 使用回环 HTTP 地址并加上
`--development-http`。

| 命令                                       | 输入与结果                                                                         |
|--------------------------------------------|------------------------------------------------------------------------------------|
| profile add/show/remove NAME、profile list | 添加绑定、查看/枚举脱敏资料、本地删除；remove 不撤销 Session                       |
| register、login                            | `--login` 必填；register 还需 `--display-name`                                     |
| logout、me、spaces                         | 撤销并清除凭据、查看当前用户、列出空间/根 UUID/额度                                |
| invite create                              | `--max-uses` 默认1，`--valid-for-seconds` 默认604800                               |
| invite list                                | `--limit` 默认100、最大200，`--cursor` 接前一页结果                                |
| invite revoke ID                           | 撤销指定邀请 UUID                                                                  |
| ls NODE_ID、stat NODE_ID                   | UUID 查询；ls 支持同样的 limit/cursor，单次只请求一页                              |
| account rename                             | `--display-name NAME --if-match ETAG`，只改本人                                    |
| account password                           | `--if-match ETAG`；终端读取一次旧密码、两次新密码，成功撤销全部会话                |
| user list、user show                       | 管理员；list 接受 limit/cursor，show 必填 `--id USER_ID`                           |
| user enable/disable                        | 管理员；`--id USER_ID --if-match ETAG`                                             |
| quota show                                 | 管理员；`--space-id SPACE_ID [--user-id USER_ID]`，省略 user-id 表示空间额度       |
| quota set                                  | 同上，另必填 `--bytes N --nodes N --if-match ETAG`，两项均为0..INT64_MAX规范十进制 |
| mkdir                                      | 必填 `--space SPACE_ID --parent NODE_ID --name NAME`；不解释文件路径               |

### 修改前先取得 ETag

改名、改密和调整额度等操作需要 `--if-match`。ETag 表示你读取到的那一版资源，可以从 `me`、`user show`、`quota show`
或节点查询取得。JSON 输出放在 `meta.etag`，文本输出也可直接复制。

标签中的双引号是值的一部分，在 shell 中可以用单引号包住整个标签，例如：

```sh
anof --profile family account rename --display-name 'New name' \
  --if-match '"user-10000000-0000-4000-8000-000000000001-1"'
```

请替换成实际标签。收到 412 表示条件已不匹配，重新读取后再决定是否修改；客户端不会自动换标签重试。改密成功会撤销所有会话并清除当前
profile 的凭据，需要重新登录。

## 文件管理命令

服务器文件使用节点、空间或回收站条目的 UUID，不使用服务器磁盘路径。下面命令都放在公共选项之后：

| 命令                                                                | 行为                                      |
|---------------------------------------------------------------------|-------------------------------------------|
| `rename NODE_ID --name NAME --if-match TAG`                         | 重命名                                    |
| `mv NODE_ID --parent ID [--name NAME] --if-match TAG`               | 同空间移动，可同时改名                    |
| `cp NODE_ID --space ID --parent ID --name NAME --if-match TAG`      | 单文件复制，仍扣逻辑额度                  |
| `rm NODE_ID --if-match TAG`                                         | 原子移入回收站，输出Trash ID/ETag，仍计费 |
| `search --space ID --query TEXT [--limit N --cursor TEXT]`          | 一页字面搜索，不自动分页                  |
| `trash list --space ID [--root-node ID --limit N --cursor TEXT]`    | 一页回收站条目                            |
| `trash show TRASH_ID`                                               | 条目、历史父目录和ETag                    |
| `trash restore TRASH_ID [--parent ID] [--name NAME] --if-match TAG` | 省略字段使用原值；额度仍计费              |
| `trash purge TRASH_ID --if-match TAG --confirm-permanent`           | 确认永久逻辑删除；物理GC可能尚未完成      |

`rm` 将整棵子树移入回收站，文件仍占用额度；`trash purge` 才永久删除，并且必须提供 `--confirm-permanent`
。内容文件的后台清理可能稍后才释放磁盘空间。

名称搜索按字面内容匹配，不把通配符当作模式。列表和搜索一次只取一页；继续查看时传入上一页返回的 cursor。写请求结果未知时，可以用
`stat`、`ls` 或回收站查询核对，但某次查询返回 404 本身不能证明之前的操作成功。

## 上传、下载与续传命令

上传源和下载目标使用本地绝对路径。服务端目标仍使用空间和节点 ID：

```sh
anof --profile family upload /home/me/photo.jpg --space SPACE_ID --parent PARENT_ID --name photo.jpg
anof --profile family upload resume TRANSFER_ID
anof --profile family upload list --limit 100
anof --profile family upload show UPLOAD_ID
anof --profile family upload complete UPLOAD_ID
anof --profile family upload cancel UPLOAD_ID
anof --profile family download NODE_ID --output /home/me/saved.jpg
anof --profile family download resume TRANSFER_ID
```

将大写占位符替换为实际 ID。`upload_id` 是服务器上的上传记录，`transfer_id` 是本地传输记录，resume 使用后者。本地记录保存在配置目录的
`transfers` 下。

上传结果中的 `published` 表示是否已经发布，202 只表示服务器接纳了后续处理。如果上传停在 READY 且目标冲突，可以用
`upload complete` 同时指定 `--parent` 和 `--name` 选择新目标。结果未知时，先查询或明确执行 resume，不要重新创建一次上传。

恢复需要继续读取源文件时，会重新检查文件身份、长度和摘要，源已改变就会拒绝。已经完成的记录仍会查询服务器；结果被永久删除时返回拒绝并保留历史记录，不重新上传。

下载目标必须不存在。客户端保留临时 part 文件和记录，以便中断后续传；完整长度和 SHA-256 检查通过后才发布目标文件。不要手工替换
part 或覆盖目标来绕过恢复检查。

管理员还可以查看和设置实例内容预算：

```sh
anof --profile family storage show
anof --profile family storage set --content-budget-bytes 1073741824 \
  --if-match '"storage-INSTANCE_ID-REVISION"'
```

使用 `storage show` 返回的实际标签。待清理、隔离和不可用内容统计是已有占用的子集，不要再次相加；待清理空间也还不能视为可用空间。

## 创建实例与完成中断的初始化

新实例目录须已存在、为空、归运行用户所有且权限为 0700，路径不能包含符号链接或 `..`。下面创建一个本机开发实例，配置 HTTPS
服务请按[部署说明](../../deploy/README.md)继续：

```sh
mkdir -m 700 /absolute/path/to/instance
anof init --root /absolute/path/to/instance \
  --admin-login admin --admin-display-name Administrator --development-http \
  --content-budget-bytes 10737418240 \
  --private-bytes 10737418240 --private-nodes 10000 \
  --public-bytes 10737418240 --public-nodes 10000 \
  --contribution-bytes 10737418240 --contribution-nodes 10000
```

内容预算与逻辑额度分别限制内容占用和账户/空间用量，不预分配磁盘。字节数允许 0～INT64_MAX，初始化节点额度为
1～INT64_MAX；参数使用规范十进制整数。`--max-users` 默认为 10，范围 1～10000，包含管理员。

磁盘安全余量默认至少 1 GiB、比例为 5%，分别由 `--disk-safety-min-bytes` 和 `--disk-safety-percent` 设置，比例范围为
0～100。管理员登录名和显示名、三组额度、内容预算均需提供。

密码从控制终端读取两次，要求 12～1024 个 UTF-8 字节、不含 NUL，不裁剪空白。初始化成功输出 `ANOF_INITIALIZED`
，不会自动登录或启动服务。参数或输入错误退出 2，实例冲突、锁竞争或初始化失败退出 3。

如果数据库引导已经提交，但配置/格式发布尚未完成，可以使用：

```sh
anof init --root /absolute/path/to/instance --finish
```

`--finish` 只能配合 `--root`，它核对原来的 pending 记录和已提交数据，完成发布后清除
pending，不重新设置密码。缺少记录、数据未提交或版本不符会拒绝；完成后不需要再次执行。

## 启动、停止及输出

```sh
anof serve --root /absolute/path/to/instance
```

准备实例和后台资源、完成必要恢复并成功监听后，服务日志包含：

```text
{"event":"ANOF_READY","port":8080,"code":"OK","outcome":"confirmed"}
```

这是一条省略了其他字段的示例；配置 port=0 时记录实际端口。其他诊断也写入日志，不能只把第一行当成就绪通知。持续检查可使用
`/health/ready`。

SIGINT 或 SIGTERM 请求正常停止。服务依次停止接纳、清理 HTTP 和后台工作，再释放实例锁，记录 `ANOF_STOPPING` 和 `ANOF_STOPPED`
。重复信号不会延长 HTTP 宽限，也不会强行中断运行中的事务。文件 I/O 尚未结束时，停止仍可能继续等待。

serve 的退出码为：0 正常停止，2 参数错误，3 实例准备失败，4 资源/服务启动失败，5 服务期间信号处理失败。端口冲突不会自动改端口重试，也不会输出
READY。`stage=dependencies` 表示运行库检查失败，可按[构建说明](../../BUILDING.md)核对依赖。

## 本地运维

本地维护与服务不能同时操作同一实例，先停止服务。路径使用绝对路径，工作目录在实例外；`--json` 放在子命令之前，不使用
`--profile` 或 `--config-dir`。

| 命令                              | 参数与结果                                                                                                                                        |
|-----------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------|
| `config show` / `config validate` | `--root DIR`；沿用安全配置读取器，show 显示全部生效配置和 `source=instance-file`，validate 不打开业务数据库恢复，也不证明 ready                   |
| `admin reset-password`            | `--root DIR`；仅终端两次输入，更新唯一管理员密码、epoch、撤销全部 Session 并同事务记录本地维护审计；无密码参数或环境变量入口                      |
| `fsck`                            | `--root DIR [--full] [--work-dir DIR] [--report FILE]`；默认 Quick，不写源数据库                                                                  |
| `fsck --repair`                   | 上述检查参数加 `--plan-out FILE`；只生成有界可审查计划，不执行修复                                                                                |
| `fsck --apply-plan FILE`          | `--root DIR --confirm-repair --backup-dir NEW_DIR [--work-dir DIR] [--report FILE]`；不能混用 `--full`/`--repair`，先保全原始元数据，再执行并复查 |
| `backup`                          | `--root DIR --output NEW_DIR [--exclude-quarantine] [--work-dir DIR]`；完整哈希检查，排除项明确列出；只在副本移除凭据、上传和不引用内容           |
| `restore`                         | `--backup DIR --root NEW_DIR --config FILE [--work-dir DIR]`；完整校验，新配置须完整显式给出，目标不得存在，不覆盖旧实例                          |

具体备份、恢复和修复示例见[部署说明](../../deploy/README.md)。报告、计划和备份目标使用新路径，避免覆盖已有证据。FSCK 的 JSON
报告按 header、finding、summary 输出，文本模式显示摘要与报告位置；未指定报告路径时，在外部工作目录生成随机名称。

修复计划最多 10000 项动作、16 MiB；详细 finding 最多 10000 条，每行最多 4096 字节，报告最多 48
MiB。报告不完整或空计划不能作为执行修复的依据。接续同一修复需提供原计划和匹配的完整原始副本，不能换一份副本继续。

备份保留业务 ID、文件和密码哈希，但恢复实例不含登录会话、邀请码和上传任务。备份过程清理的是副本，原实例的会话和有效上传不会因此消失。

本地维护退出码：0 成功/健康，2 参数错误，3 已知拒绝，4 检查发现问题或未完成，5 本地 I/O 或已确认操作后的输出失败，6
数据库/文件效果未知，130 未修改源数据的中断。发生未知结果时保留报告中的操作 ID、阶段和现场，先核对，不直接重试发布或清理临时文件。

## 远程命令的输出和错误

文本输出显示字段、精确数量、分页和条件标签。`--json` 成功时在 stdout 输出一行 `{"ok":true,"data":...,"meta":...}`，失败时在
stderr 输出 `{"ok":false,"error":...}`。这是结构示意，其中省略号不是有效 JSON。密码提示仍写到控制终端。

`meta` 包含 profile 及可用的 HTTP 状态、请求 ID 和 ETag。大整数与版本使用十进制字符串，程序化读取时不要先转换为浮点数。远程
`me` 还包含 `instance_id`。

| 退出码 | 含义                     |
|--------|--------------------------|
| 0      | 成功                     |
| 2      | 参数或秘密输入不合法     |
| 3      | 服务拒绝或缺少凭据       |
| 4      | 读取请求的网络/协议错误  |
| 5      | 本地状态、文件或输出失败 |
| 6      | 写请求结果未知           |
| 130    | 读取或输入被中断         |

错误中的远端和本地结果需要一起看。例如登录在服务器成功但凭据保存失败，会返回本地错误，仍保留远端成功信息；输出失败也可能发生在操作成功之后。登出即使远端失败也尝试清除本地凭据，其他
401 不会自动清除。客户端不自动重发写入或重新登录。
