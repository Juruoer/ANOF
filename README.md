# ANOF

ANOF（ANOF is Not Only a Folder）是一个 Linux 单机个人／家庭云盘：用 SQLite 管理账户、空间和逻辑目录树，用本地文件系统保存不可变文件内容，通过统一
HTTP API 服务 CLI 和 Web。

HTTP 服务由 [muduox](third_party/muduox/README.md) 提供。

项目仍是内部开发版本，v0.1 将在规划功能与验收完成后发布。当前已提供：

- 实例初始化、本机前台服务、账户／邀请／空间管理和远程 CLI。
- 目录查询与创建、逻辑额度及实例内容预算管理。
- 有界上传、SHA-256 校验、不可变内容发布、取消／过期清理与监听前恢复。
- GET／HEAD 内容下载、单范围及条件请求、上传／下载 CLI 和持久化记录续传。

尚未提供目录／文件移动、复制、删除和回收站，也未提供 Web 客户端或生产部署方案。
当前验证以 Linux／WSL2 小文件为主，不代表大容量性能或物理机掉电耐久性已验证。

服务入口及运行要求见 [apps/anof/README.md](apps/anof/README.md)
，运行时结构见 [anof/runtime/README.md](anof/runtime/README.md)，客户端库见 [anof/client/README.md](anof/client/README.md)。

## 获取源码与 muduox

`third_party/muduox` 是 Git 子模块，仓库地址由 [.gitmodules](.gitmodules) 指定。首次克隆时，将下方 `<ANOF仓库地址>`
替换为实际地址，一并获取子模块：

```sh
git clone --recurse-submodules <ANOF仓库地址> ANOF
cd ANOF
```

如果已经克隆 ANOF，但尚未下载 muduox，在 ANOF 根目录执行：

```sh
git submodule update --init --recursive
```

该命令将 muduox 检出到 ANOF 所记录的提交，不会自动跟随 muduox 的最新版本。更新或切换 ANOF 版本后，也应执行此命令，使子模块与当前
ANOF 版本一致；如有子模块本地修改，先妥善保存。

构建前必须完成子模块初始化。普通 Git 克隆不会自动下载子模块源码，GitHub 的普通源码 ZIP 也不包含完整子模块内容，建议使用上述
Git 命令获取可构建源码。

## 许可证

ANOF 自有代码及文档采用 **BSD-3-Clause** 许可证，完整条款见 [LICENSE](LICENSE)。

Copyright (c) 2026, Juruoer and ANOF contributors.

该许可证允许使用、修改及以源码或二进制形式再分发，包括商业使用和闭源衍生版本。再分发须按条款保留版权、许可条件与免责声明；未经许可，不得使用版权持有人或贡献者的名称为衍生产品背书。软件按原样提供，不附带许可证所排除的担保。

第三方代码和依赖保留各自的版权与许可证，不由 ANOF 的根许可证替代。尤其是 `third_party/muduox`，其原始 Muduo 代码以及 muduox
新增／修改部分的声明见 [muduox License](third_party/muduox/License)。分发包含第三方组件的源码或二进制包时，须同时遵守相应条款并随包保留所需许可材料。
