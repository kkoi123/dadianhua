# USB 转发兼容组件

该组件处理 `bridge0` / `ecm0` 到 `rmnet_data数字` 的 IPv4 转发数据，将发出的 IPv4 TTL 设为 64 并修正头校验和。模块自身 LOCAL_OUT 流量不经过此钩子。它不修改 APN、SIM 或无线模式，IPv6 不在当前适配范围内。

网页中的「USB 漫游兼容」控制本组件；它与运营商数据漫游权限是两个不同功能。只应在确认目标固件兼容且确有需要时开启。

## 源码

- `qdc_usb_compat.c`：内核转发钩子，默认关闭。
- `ttl.h`、`test_ttl.c`：校验和逻辑及独立测试。
- `module.config`：构建所需的内核配置，不含设备账号或用户运行数据。
- `Makefile`、`build.sh`：组件构建入口。

## 构建依赖

- Linux 3.18.44 内核源码：`the-modem-distro/quectel_eg25_kernel`，提交 `82ed00908b3e8efc3ff0de27d2b5a7c0524ecd7f`。
- AOSP `android-8.1.0_r1` 的 ARM GCC 4.9。
- 可运行 Linux amd64 工具链的 Docker 环境。
- 本目录的 `module.config`。

构建脚本使用准备好的外部依赖目录，不把数百 MB 的内核和工具链缓存纳入源码。依赖目录包含 `kernel.tar.gz`、`gcc.tar.gz`、已展开的内核目录和 `gcc/`。归档和配置的固定校验值由 `build.sh` 核对。

```sh
QDC_USB_TOOLS=/path/to/prepared-kernel-and-gcc ./build.sh
```

默认容器镜像名为 `qdc-usb-compat-builder:20261010`，可以用 `QDC_USB_BUILD_IMAGE` 指定自己的构建镜像。容器需有 GNU make、主机 C 编译器、BC 及相应构建依赖；交叉编译器由已准备的 AOSP 工具链提供。此源码整理不包含该本地 Docker 镜像。

仅有相同内核版本和 vermagic 不能保证不同厂商固件 ABI 兼容。此组件没有完整原厂 `Module.symvers`，不能以版本号替代实际兼容验证。

## 只在电脑运行的逻辑测试

```sh
clang -std=c11 -O2 -Wall -Wextra -Werror test_ttl.c -o /tmp/qdc-test-ttl
/tmp/qdc-test-ttl
```

`trial-remote.sh` 是按需调用的临时试验脚本，不参与构建，也不会被构建脚本自动执行。加载内核组件须另行确认目标硬件与固件；源码检查和构建不需要连接模块。
