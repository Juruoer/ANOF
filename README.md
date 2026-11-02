# ANOF

ANOF（ANOF is Not Only a Folder）是一个运行在 Linux 上的个人和家庭云盘。你可以把它部署在自己的服务器上，通过浏览器或命令行管理文件。

当前版本： **0.1.0**。各版本变化见 [更新记录](CHANGELOG.md)。

## 功能

- 为家庭成员创建账户，分别管理私人空间和公共空间，并设置存储额度。
- 上传、下载文件，在传输中断后继续上传；命令行也支持下载续传。
- 创建目录、移动和重命名文件、复制单个文件，以及按名称搜索。
- 将文件移入回收站，在保留期内恢复，或永久删除。
- 由管理员管理邀请、账户和额度；在服务器上检查数据、备份和恢复实例。

文件内容保存在服务器的本地文件系统中，账户和目录信息由 SQLite 管理。HTTP 服务使用 [muduox](third_party/muduox/README.md)。

## 获取源码

ANOF 通过源码安装。克隆时请同时获取 muduox 子模块：

```sh
git clone --recurse-submodules https://github.com/Juruoer/ANOF.git
cd ANOF
```

如果已经克隆过仓库，或刚切换了 ANOF 的版本，在项目根目录执行：

```sh
git submodule update --init --recursive
```

这样会取得 ANOF 对应的 muduox 版本。GitHub 自动生成的源码 ZIP 不包含子模块，建议使用 Git 克隆。

## 编译与使用

准备好 [编译依赖](BUILDING.md#准备依赖) 后，在项目根目录运行：

```sh
./scripts/build.sh --preset release
```

生成的 `cmake-build-release/apps/anof/anof` 同时提供服务、命令行客户端和维护命令。Web
界面单独构建，具体步骤见 [编译和安装](BUILDING.md)。

接下来按 [部署说明](deploy/README.md) 创建实例，配置 HTTPS 并启动服务。日常使用可查阅：

- [Web 界面](web/README.md)
- [命令行用法](apps/anof/README.md)
- [备份与恢复](deploy/README.md#备份与恢复)

如果要了解或修改代码，可以从 [领域模型](anof/domain/README.md)、[应用用例](anof/application/README.md)、[运行时](anof/runtime/README.md)
和 [客户端库](anof/client/README.md) 开始。测试方法见 [测试说明](tests/README.md)。

## 查看版本

服务端和命令行客户端共用 `anof` 程序。在相应机器上执行：

```sh
anof --version
anof serve --version
```

两条命令都显示本机程序版本，不需要实例目录，也不会启动服务。查看远程服务器的版本时，请在服务器上运行其实际部署的程序；客户端不会用本机版本代替远程版本。

## 参与项目

问题反馈、文档修改和代码贡献见 [贡献说明](CONTRIBUTING.md)。安全问题同样通过 Issues 提交，报告内容请参考 [安全报告说明](SECURITY.md)。

## 许可证

ANOF 使用 [BSD-3-Clause](LICENSE) 许可证。

Copyright (c) 2026, Juruoer and ANOF contributors.

第三方组件使用各自的许可证，见 [muduox License](third_party/muduox/License) 和 [依赖与许可证](deploy/DEPENDENCIES.md)。
