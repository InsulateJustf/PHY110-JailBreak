# prebuilt — 二进制产物

本目录只放**跑通当前链路所需的最小二进制集合**，源码都在 `../exploit` 与 `../tools`。
重新构建见根 README。

| 文件 | 大小 | SHA-256 | 来源 |
|---|---|---|---|
| `t98260` | 429 KB | `b2aaf61a…a346` | 本仓库 `exploit/` + `PYB110_TARGET=phy110-6.1` 构建 |
| `rootdclient` | 8.7 MB | `f1cf3d2f…8573` | 本仓库 `tools/rootdclient.c` 静态编译 |
| `kernelsu/ksud` | 6.4 MB | `049491e2…fb1b` | 上游 KernelSU `v3.3.1` release，未修改 |
| `kernelsu/android14-6.1_kernelsu.ko` | 413 KB | `27d45be8…6f0e` | 上游 KernelSU `v3.3.1` release（KMI `android14-6.1`） |
| `kernelsu/KernelSU_v3.3.1_32680-release.apk` | 13.6 MB | `b4a059ac…e450` | 上游 KernelSU `v3.3.1` release，未修改 |

完整哈希：

```
b2aaf61af5343760e9d5bd8bd18bc52054995bbd43799589b4a803153e39a346  t98260
f1cf3d2f9bea6be0c1b5c29087c7fa3d7a435a1f1748e5ca2c3f85ef687b8573  rootdclient
049491e2b4cc0a6c913d3920cb90b8c94c9e8ca96c0ee92818293304973bfb1b  kernelsu/ksud
27d45be86a68e03aa9d8e103fe229c66245f5c3ddccd236e52272131ad156f0e  kernelsu/android14-6.1_kernelsu.ko
b4a059ace71de2fa65cc7f12b1c58d55fc23ab25e057ee4230ce779742abe450  kernelsu/KernelSU_v3.3.1_32680-release.apk
```

## 许可

- 本仓库自研代码（`t98260`、`rootdclient`）沿用 MIT，见根目录 `LICENSE`。
- `ksud`、`kernelsu.ko`、管理器 APK 来自 [tiann/KernelSU](https://github.com/tiann/KernelSU)，
  **GPL-2.0**；此处按上游 release 原样分发，未做任何修改。

## 安装提示

ColorOS 会拒绝流式安装（`Failure [-99]`），管理器 APK 必须用 push 安装：

```bash
adb install --no-streaming -r prebuilt/kernelsu/KernelSU_v3.3.1_32680-release.apk
```
