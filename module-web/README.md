# 模块内部网页项目

版本 **0.3.43**，运行目录 `/usrdata/qdc-web`，运行时状态目录 `/run/qdc-web`。本文件介绍当前实现，不包含旧版安装日志、个人设备账号或历史通信记录。

## 当前代码

| 文件 | 作用 |
| --- | --- |
| `api.c` 与 `*.inc` | 管理员认证、状态和设置 API |
| `comm.c`、`esim-worker.c` | 厂家 QMI 接口、短信、通话、SIM PIN、eSIM 操作 |
| `audio.c`、`audio-pcm.c` | 网页 WebSocket 音频与模块声卡桥接 |
| `entry.c`、`static-assets.inc` | 仅监听回环地址的公网转发入口和固定静态资源 |
| `traffic.c`、`call-history.c` | 流量和通话记录持久化 |
| `config-store.c`、`network-watch.c` | 配置恢复和本地网络变化监听 |
| `standalone-*.sh`、`module-web-remote.sh` | 模块自身的服务与开机入口 |
| `static/` | 网页源码 |
| `packaging/` | 首次安装脚本与发布打包 |
| `vendor/` | 固定版本的第三方源码与校验记录 |

## 构建环境

现有构建脚本主要在 Apple Silicon Mac 上验证，需要 Python 3.9+、Clang、Node.js 和固定版本 Zig 0.13.0。Docker 用于可选的 USB 内核兼容组件。其他开发平台需要使用适合本机的 Zig 和编译工具。

工具链和生成文件均可重新取得，不进入源码仓库：

```sh
./setup-toolchain.sh
./build.sh --native-only
```

`setup-toolchain.sh` 下载并核对已固定的官方工具，写入 `.tools/`；它不连接模块。`build.sh` 将编译缓存和本地模拟程序写入 `build/`。也可通过 `ZIG=/path/to/zig` 指定已安装的 Zig 0.13.0。

第三方源码保留其原许可证及 `provenance.json`。加密输入说明见 [CRYPTO-BUILD.md](CRYPTO-BUILD.md)。

### 编译网页后台

```sh
./build.sh --all
```

输出 `dist/www` 和 `dist/lib`，包括 ARM 模块程序、网页和校验清单。此操作不安装到模块。

### 编译独立入口

先准备与目标模块匹配的两个语音驱动：`qdc507_aprv3.ko`、`qdc507_voice.ko`。这些原厂预编译依赖不冒充本项目源码；可以从自己持有的、校验过的模块安装包中取得，也可指定现有组件目录。

```sh
# 从已验证安装包提取匹配驱动，不执行包内程序，不连接硬件
python3 scripts/import-release-deps.py --archive /path/to/module-install.zip

./build-standalone.sh
```

已有组件时可使用 `MODULE_VOICE_DIR=/path/to/components ./build-standalone.sh`。脚本核对固定驱动哈希；不会把不匹配的内核组件加入安装包。花生壳官方客户端及其独立运行库由 `setup-standalone-client.py` 准备，保留来源记录和已有适配校验。

USB 漫游兼容组件见 [usb-compat/README.md](usb-compat/README.md)。完整发布包需要先构建或导入该组件。

### 生成安装包

```sh
python3 packaging/build-release.py --output /path/to/release-output
```

脚本仅从明确清单中取文件，包含模块可执行程序、网页、运行组件、首次安装入口、第三方许可及必要的第三方源码。不会复制管理员凭据、花生壳设备身份、Bark 设置、短信、通话或流量记录。输出目录已存在同版本时拒绝覆盖。

首次安装说明见 [packaging/README-安装说明.md](packaging/README-安装说明.md)。首次安装入口拒绝覆盖已有项目；它不是通用升级工具。

## 本地测试

```sh
# 完成 --native-only 构建后
python3 test_communications.py
python3 test_call_history.py
python3 test_traffic.py
python3 test_traffic_recovery.py
node test_auth_ui.js
node test_sms_codec.js
node test_sms_outbox_ui.js
```

这些测试使用临时目录、模拟 QMI、虚构号码及测试凭据，不连接真实模块、不发送短信、不拨号。独立入口和配置恢复的本地测试由 `build-standalone.sh` 调用。

打包后可通过 `QDC_RELEASE_DIR=/path/to/release-output python3 packaging/test_release.py` 检查安装包。

## 本地凭据与数据

源码不包含已安装设备的密码、推送密钥或设备身份。首次安装时为新模块生成独立管理员密码；花生壳通过用户操作注册和绑定，Bark 地址在网页设置中填写。

实际短信位于卡片或模块存储中，后台持久化设置、发送记录、通话记录和流量位于模块的 `private` 目录。这些运行数据不能加入源码仓库。代码中出现相应文件名，只表示程序读写位置。

网页预览分支和模拟测试中的示例短信、号码和凭据不是真实用户记录。第三方版权署名应保留。
