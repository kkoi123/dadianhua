# 第三方组件

- **Mbed TLS**：认证及 WebSocket 握手使用的组件；许可见 `payload/licenses/MbedTLS-LICENSE.txt`。
- **libeuicc / lpac euicc**：eSIM 管理共享库，LGPL-2.1；cjson-ext 和 cJSON 使用 MIT 许可。对应源码、保留的本地修改、来源清单及构建说明位于 `third-party-source/libeuicc-source.tar.gz`。共享库独立存放在模块 `/usrdata/qdc-web/lib/libqdc-euicc.so`，可以用兼容 ABI 的重建版本替换。
- **花生壳 phtunnel**：官方 ARM 客户端及本项目既有适配；来源、原始校验和及两个适配位置见 `payload/standalone/provenance.json`。首次使用需要用户自己的账号及设备绑定。
- **musl / libgcc**：phtunnel 使用的独立运行库；来源及版本见同一 provenance 文件。许可见 `payload/licenses/musl-COPYRIGHT.txt`、`GCC-Runtime-Exception.txt` 和 `GPL-3.0.txt`。对应文本取自 musl 1.2.3 官方发布包及 GCC 11.2.0 官方镜像源码。musl 项目：https://musl.libc.org/；GCC 运行库许可：https://www.gnu.org/licenses/gcc-exception-3.1.html 。
- 原厂 QMI、音频库及原厂系统工具由目标模块提供，未随此安装包复制。

第三方组件继续适用各自原许可。自研部分采用 [QDC507 模块网页非商业源码许可 1.0](https://github.com/kkoi123/dadianhua/blob/main/LICENSE)，适用范围见 [LICENSE-SCOPE.md](https://github.com/kkoi123/dadianhua/blob/main/LICENSE-SCOPE.md)：允许非商业使用，商用须另行书面授权，向他人分发原版或修改版时须保留许可及署名并提供对应源码。自研部分的非商业限制不覆盖依原许可授权的第三方组件及 GPL-2.0-only USB 兼容源码。
