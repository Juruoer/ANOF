# 部署与维护

本文以 Ubuntu 24.04 上的单机部署为例：nginx 提供 HTTPS 和 Web 页面，将 API 请求转发给本机的 ANOF；systemd 负责启动和停止服务。

开始前，请先完成源码根目录 `BUILDING.md` 中的编译和安装，包括 Web。下面使用这些路径，你可以按自己的机器调整：

| 内容     | 示例位置                       |
|----------|--------------------------------|
| 程序     | `/usr/local/bin/anof`          |
| Web 页面 | `/usr/local/share/anof/web`    |
| 部署示例 | `/usr/local/share/anof/deploy` |
| 实例数据 | `/var/lib/anof/instance`       |
| 网站地址 | `https://cloud.example.test`   |

还需要安装 nginx，并为自己的域名准备 HTTPS 证书和私钥。示例不会申请或续期证书，这部分请使用你熟悉的证书工具。

## 创建实例

用专门的系统账户运行 ANOF，并给它一个空的数据目录。如果已经创建过 `anof` 账户，请跳过创建账户的命令。

```sh
sudo useradd --system --home-dir /var/lib/anof --shell /usr/sbin/nologin anof
sudo install -d -o anof -g anof -m 0700 /var/lib/anof
sudo -u anof mkdir -m 700 /var/lib/anof/instance
sudo -u anof /usr/local/bin/anof init --root /var/lib/anof/instance \
  --admin-login admin --admin-display-name Administrator --development-http \
  --content-budget-bytes 10737418240 \
  --private-bytes 10737418240 --private-nodes 10000 \
  --public-bytes 10737418240 --public-nodes 10000 \
  --contribution-bytes 10737418240 --contribution-nodes 10000
```

程序会要求输入并确认管理员密码，输入时不显示字符。密码至少 12 字节，不要写进命令行或环境变量。

示例将内容预算和各项额度设为 10 GiB、节点数设为 10000，可以自行修改。这些数值是使用上限，不会立即分配对应空间。磁盘上还要给数据库、临时上传、日志和备份留出余量。

实例目录必须为空、属于运行账户且权限为 0700，路径不能包含符号链接或 `..`。它与 Web 页面目录是两回事：不要让 nginx
直接提供实例目录里的文件。

这里的 `--development-http` 只用于生成初始配置，初始化不会启动服务。下一步将它改为 HTTPS 代理配置。

## 配置 ANOF

复制 [config.toml](config.toml)，修改后保存为自己的配置文件。至少检查以下几项：

| 配置项                   | 示例值与含义                                        |
|--------------------------|-----------------------------------------------------|
| `listen_address`、`port` | `127.0.0.1`、`8080`，供本机 nginx 连接              |
| `development_http`       | `false`，使用 HTTPS 代理模式                        |
| `web_enabled`            | `true`，允许浏览器登录                              |
| `public_origin`          | 你的完整网站地址，例如 `https://cloud.example.test` |
| `trusted_proxies`        | `["127.0.0.1"]`，只接受本机代理转发的来源信息       |

如果使用非默认 HTTPS 端口，网站地址也要包含端口。ANOF 和 nginx 中填写的域名、端口需要一致，否则浏览器请求会被拒绝。

保留初始配置，再安装修改后的完整文件。将下面的 `/path/to/config.toml` 换成实际位置：

```sh
sudo -u anof cp /var/lib/anof/instance/.anof/config.toml /var/lib/anof/config.before-production.toml
sudo install -o anof -g anof -m 0600 /path/to/config.toml /var/lib/anof/instance/.anof/config.toml
sudo -u anof /usr/local/bin/anof --json config validate --root /var/lib/anof/instance
```

配置检查通过后再继续。也可以用 `config show --root /var/lib/anof/instance` 查看配置。ANOF 只需监听回环地址，不必向外开放
8080 端口。

## 配置 nginx

以 [nginx/anof.conf](nginx/anof.conf) 为模板，替换网站域名、证书和私钥路径。若安装位置或后端端口不同，还需要修改静态文件目录和
`proxy_pass`。

这份文件放在 nginx 的 `http {}` 中加载一次。Ubuntu 通常可以使用 `/etc/nginx/conf.d/`，安装前请确认主配置包含该目录：

```sh
sudo install -d -o www-data -g adm -m 0750 /var/log/anof/nginx
sudo install -m 0644 /path/to/anof.conf /etc/nginx/conf.d/anof.conf
sudo nginx -t
```

模板中已经包含上传、下载、Cookie 和来源检查所需的代理设置，修改时保留这些配置。Web 页面和 API 必须使用同一个 HTTPS 地址；不要通过关闭
Origin 或 CSRF 检查来解决配置错误。

模板开启 HSTS，浏览器访问后会记住使用 HTTPS。启用前应确保域名和证书可持续使用。它只监听 443，没有配置 HTTP 跳转或证书验证站点。

## 启动并登录

安装 [systemd 服务文件](systemd/anof.service)：

```sh
sudo install -m 0644 /usr/local/share/anof/deploy/systemd/anof.service /etc/systemd/system/anof.service
sudo systemd-analyze verify /etc/systemd/system/anof.service
sudo systemctl daemon-reload
sudo systemctl start anof
sudo systemctl status anof --no-pager
sudo journalctl -u anof -n 50 --no-pager
sudo systemctl reload nginx
curl --fail https://cloud.example.test/health/ready
```

记得将最后一行的域名换成自己的地址。ANOF 日志中的 `ANOF_READY` 表示服务已启动，HTTPS 检查成功后，就可以在浏览器打开网站，用初始化时设置的管理员账户登录。

建议先创建一个目录，上传一个小文件，再下载核对内容。确认访问正常后，设置开机启动：

```sh
sudo systemctl enable anof
```

也可以通过命令行连接同一个网站：

```sh
anof --config-dir /absolute/path/to/client-config profile add home --server https://cloud.example.test
anof --config-dir /absolute/path/to/client-config --profile home login --login admin
anof --config-dir /absolute/path/to/client-config --profile home --json me
```

systemd 示例只允许服务写入 `/var/lib/anof`，并禁止访问用户家目录。如果更改数据路径，要同时调整服务文件中的工作目录、两条启动命令和
`ReadWritePaths`。程序、库和 Web 页面应由管理员维护，不授予服务账户写权限。

## 停止服务

```sh
sudo systemctl stop anof
sudo systemctl is-active anof
sudo journalctl -u anof -n 50 --no-pager
```

停止时 ANOF 会等待请求和后台工作结束，再释放实例锁。示例服务最多等待 120 秒，超时后 systemd
会强制结束进程。如果发生这种情况，保留日志并检查原因，不要删除锁文件或重新创建数据库。

本地检查、备份、恢复和密码重置都需要先停止服务。可以用下面的命令确认配置可读取且实例锁可取得：

```sh
sudo -u anof /usr/local/bin/anof --json config validate --root /var/lib/anof/instance
```

## 备份与恢复

备份前停止服务，在实例目录外准备一个工作目录。备份和恢复的目标路径必须尚不存在，每次操作请选用新名称。

```sh
sudo -u anof mkdir -m 700 /var/lib/anof/maintenance
sudo -u anof /usr/local/bin/anof --json backup \
  --root /var/lib/anof/instance \
  --output /var/lib/anof/backup-new \
  --work-dir /var/lib/anof/maintenance
```

备份保留文件、目录、回收站和用户密码哈希，不保留登录会话、邀请码和上传任务。它含有私人数据，请限制访问，并将副本放到其他存储设备；需要加密时请另外使用加密工具。

如果实例中有隔离文件，备份默认会拒绝。确认不需要将这些文件纳入备份时，可以使用 `--exclude-quarantine`，排除项会记录在备份中。

恢复到一个新目录，并提供完整配置：

```sh
sudo -u anof /usr/local/bin/anof --json restore \
  --backup /var/lib/anof/backup-new \
  --root /var/lib/anof/restored-new \
  --config /var/lib/anof/instance/.anof/config.toml \
  --work-dir /var/lib/anof/maintenance
sudo -u anof /usr/local/bin/anof --json fsck \
  --root /var/lib/anof/restored-new --full \
  --work-dir /var/lib/anof/maintenance
```

示例沿用原配置，也可以提供为新服务器准备的配置。检查通过后，将 systemd 的 `ExecStartPre` 和 `ExecStart` 中的实例路径改为恢复目录，再执行
`daemon-reload` 和 `start`。保留原实例，等重新登录、核对目录和下载文件都正常后再决定如何处理它。

恢复后需要重新登录，原来的浏览器上传记录也不能继续对应已被清除的服务器任务。不要直接把备份文件覆盖到正在使用的实例上。

## 检查和修复

`fsck` 用来检查目录、引用、用量和内容文件。加上 `--full` 会读取文件并核对 SHA-256，文件多时需要更长时间：

```sh
sudo -u anof /usr/local/bin/anof --json fsck \
  --root /var/lib/anof/instance --full \
  --work-dir /var/lib/anof/maintenance
```

需要修复时，先生成计划，查看它准备修改什么，再执行：

```sh
sudo -u anof /usr/local/bin/anof --json fsck --repair \
  --root /var/lib/anof/instance \
  --work-dir /var/lib/anof/maintenance \
  --plan-out /var/lib/anof/repair-new.json
sudo -u anof /usr/local/bin/anof --json fsck \
  --apply-plan /var/lib/anof/repair-new.json --confirm-repair \
  --root /var/lib/anof/instance \
  --backup-dir /var/lib/anof/repair-original-new \
  --work-dir /var/lib/anof/maintenance
```

计划文件和原始副本目录都使用新路径。修复保存的原始副本用于保留修改前的现场，与 `backup` 生成的备份不同，不能交给 `restore`
使用。修复也不能重新生成已经丢失的文件内容。

检查发现问题时可能返回退出码 4，同时输出报告。退出码 5 表示 I/O 或结果输出失败，6
表示无法确定修改是否完成；遇到这些情况应先查看报告，保留临时目录和副本，不要因命令失败就假定没有修改、直接重试。参数错误为
2，已知拒绝为 3，未发生修改的取消为 130。

忘记管理员密码时，可在停机后执行：

```sh
sudo -u anof /usr/local/bin/anof admin reset-password --root /var/lib/anof/instance
```

输入新密码后，原有登录会话会被撤销，文件内容不变。

## 日志和常见问题

ANOF 的日志由 journald 收集，用 `journalctl -u anof` 查看。日志包含事件和请求 ID，不记录密码、令牌或请求正文。

nginx 模板的访问日志位于 `/var/log/anof/nginx/access.log`，记录方法、状态、字节数和耗时。由于 nginx
原生错误日志可能包含请求中的敏感内容，模板关闭了该站点的错误日志。排障时先查看访问日志、ANOF 日志和 `nginx -t` 的结果。

[logrotate 示例](logrotate/anof-nginx)可安装到 `/etc/logrotate.d/`，使用前核对 nginx 用户、组和 PID 路径。它每日轮转、保留 7
份，并在轮转任务执行时检查 32 MiB 大小。journald 的空间和保留时间由主机配置管理。

| 问题                        | 可以先检查什么                                             |
|-----------------------------|------------------------------------------------------------|
| 端口已占用                  | 查看哪个进程使用了监听端口，调整 ANOF 与 nginx 的对应配置  |
| 实例锁冲突                  | 是否还有 ANOF 或维护命令运行；等待它退出，不删除锁文件     |
| 浏览器返回 Origin/CSRF 错误 | 网站地址、Host、可信代理设置是否一致                       |
| nginx 返回 502/503          | ANOF 是否启动成功、后端地址是否正确、是否正在过载          |
| 空间不足或返回 507          | 内容预算、磁盘余量，以及日志、临时上传和备份的占用         |
| 检查或恢复失败              | 保留报告和原目录，按具体错误处理，不手工删除内容文件来修账 |

更换程序或迁移服务器时，另请阅读[更新程序与数据格式](RELEASE.md)。
