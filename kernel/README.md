# kernel — 目标内核镜像与符号表

这些文件来自 **PHY110 ColorOS 16 全量 OTA `PHY110_16.0.10.500(CN01)`**
（`PHY110_11.F.28_2280_202608111929`，内核 `6.1.141-android14-11-o-gb6893081c037`），
是本移植所有常量（符号地址、结构体偏移、页映射）的唯一来源。

| 文件 | 大小 | 说明 |
|---|---|---|
| `vmlinux.elf` | 39 MB | 由 `boot.img` 经 `vmlinux-to-elf 1.4.0` 重建；镜像基址 `0xffffffc008000000`，102,901 个符号 |
| `vmlinux.nm` | 4.3 MB | `nm -n vmlinux.elf` |
| `kernel.btf` | 5.6 MB | 从 `vmlinux.elf` 偏移 `0x1657A38` 抽出的 `.BTF`（5,788,319 B，magic `0xEB9F`） |

SHA-256：`ad2847b501d8cf450c64c186292bc327d7d219fdbed7ccf050daad2fd3859cee  vmlinux.elf`

## 可靠性校验（不是“看起来对”）

- **导出符号表交叉验证**：`__ksymtab` 中 `init_task = 0xffffffc00a02fc00`、
  `init_user_ns = 0xffffffc00a040ab0`，与 `nm` 完全一致。
- **符号大小 vs 真机**：设备 ftrace 打印 `do_timer_create+0xa8/0x5f8`、
  `copy_process+0x160/0x2298`，与本镜像中两符号到下一符号的距离逐字节相同。
- **结构体大小 vs 真机**：`task_struct = 4800`（BTF）等于设备 `kmem_cache_alloc`
  记录的 `bytes_req`；`k_itimer = 264` 等于 `/proc/slabinfo` 的 objsize。
- **线性映射**：`pfn = (va - 0xffffff7f80000000) >> 12` 与设备 `mm_page_alloc`
  记录的 pfn 命中 60/60（即 `memstart_addr = 0x80000000`）。

## 未入库的大文件

| 文件 | 大小 | 再生方式 |
|---|---|---|
| `PHY110_16.0.10.500.zip`（OTA 全量包） | 8.4 GB | 通过 OPPO `update/v5` 解析器现场换新 token 下载，见 `../tools/extract.sh` 顶部说明 |
| `payload.bin` | 8.4 GB | 从 OTA zip 解出 |
| `boot.img` / `vendor_boot.img` | 各 192 MB | `payload_dumper --partitions boot,vendor_boot` |
| `init_boot.img` | 8 MB | 同上 |

这些文件都可以由 `bash tools/extract.sh` 从 OTA 重新生成；因为超过 GitHub 单文件
100 MB 限制，未纳入仓库。
