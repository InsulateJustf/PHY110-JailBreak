# PHY110 / 6.1.141 port notes

Target: OPPO Find X7 Ultra (PHY110), ColorOS 16.1, `PHY110_16.0.10.500(CN01)`,
`6.1.141-android14-11-o-gb6893081c037`, patch 2026-08-01, SELinux Enforcing,
BL locked, fastboot unavailable, no 9008 (ARB fused).

All values below were measured on the device, not copied from other builds.

## Runtime-measured constants

| Quantity | Value | How |
|---|---|---|
| `k_itimer` size / `it.cpu.node` | 264 (0x108) / +0x78 | `/proc/slabinfo` + text trace `do_timer_create+0xa8`, bytes_req=264 |
| timer alloc call_site | `do_timer_create+0xa8` | text trace, pinned CPU0 |
| `task_struct` alloc | `copy_process+0x160`, `bytes_req=4800` | text trace during fork |
| VA→PA linear map | `pfn = (va - 0xffffff7f80000000) >> 12` | `INTERSECT=60/60` vs `mm_page_alloc order=1` |
| → `memstart_addr` | `0x80000000` | implied by the constant above |

## tracefs reality on this build

- `events/*/enable`, `events/*/filter`, `events/enable`, `set_event` : **denied** to shell.
- `events/kmem/rss_stat/enable` : **writable**.
- `events/kmem/rss_stat/trigger` : **writable**; `enable_event:<ev>` works and
  stacks with `:unlimited`; `!enable_event:<ev>` removes it.
- `disable_event:` : rejected on this kernel — do not rely on it.
- `tracing_on`, `buffer_size_kb`, `trace` (snapshot, non-consuming),
  `trace_pipe`, `per_cpu/*/trace_pipe_raw` : usable.
- Reading `trace_pipe_raw` **consumes** the ring buffer, so take the `trace`
  snapshot first when both views are needed.

## Reclaim measurements

- Freeing 2048 timers: `posix_timers_cache` 79 → 20 slabs; pages really return.
- Fake A/B test (`spraytest`): create 2048 timers → delete → 4096 × 8 KiB AF_UNIX
  sends → create 2048 timers again ⇒ `PAGE_REUSE=3/67`, i.e. the spray captured
  ~64 of the 67 freed 8 KiB slab pages.
- **order-1 frees go through the PCP list** (`pcp_allowed_order(1)=true`) and do
  **not** emit `mm_page_free`. The original plant's `pipe_scan("mm_page_free", …)`
  gating can never fire on this kernel; replace it with a delay + spray and keep
  the `mm_page_alloc` hit check.
- `mm_page_alloc` *is* emitted for order-1 allocations (spray produced ~4.2k).

## Open items

- image_off for `selinux_state`, `do_timer_create`, `init_user_ns`, `root_user`,
  `init_groups`, `init_ucounts`, `init_cred` (from `work/out/vmlinux.elf`).
- struct offsets (`task_struct.cred/real_cred`, `cred` layout) from `.BTF`.
- 6.1 has `real_cred`/`cred` pair: the pointer write must cover both.
- `cred.usage` is `atomic_t` on 6.1 → fake-cred field offsets differ from 6.18.

## Achieved on device (2026-11-01)

Full chain up to kernel root on PHY110 6.1.141:

```
PROBE_OK                 6.1.141 gate + trace channel
SLIDE_DERIVED            0x....  (match with the OTA image's do_timer_create+0xa8)
SELINUX=permissive       zero write at selinux_state - 7
TASK=0x...               copy_process+0x160 / 4800 B leak
CRED_INSTALLED           task->cred -> fake cred at the stale node
ROOT uid=0 caps=0x1ffffffffff test=ok
PLANT_CRED_OK            rootd listening on /data/local/tmp/t98260-root.sock
```

`rootdclient -c "id; getenforce"` returns
`uid=0(root) ... context=u:object_r:unlabeled:s0`, `Permissive`,
`Uid: 0 0 0 0`, `CapEff: 000001ffffffffff`.

Key mechanics learned on this kernel:
- `timerqueue_del` calls `rb_next()` to update `rb_leftmost`; the fake node's
  `__rb_parent_color` must therefore equal the node address (RB_EMPTY_NODE),
  otherwise `rb_next()` returns the parent and cleanup iterates into a bogus
  node and panics.
- With that geometry the erase leaves uid/gid (+4), suid/sgid (+8) and
  sgid/euid (+0x10) clobbered; three tile zero writes repair them.
- A reclaim miss must not reap the victim: the zombie is left pinned and the
  stage simply retries with a fresh victim.

Still missing for a persistent, manager-backed root: KernelSU LKM for KMI
`android14-11` + `ksud late-load` (the plant already accepts `PYB110_KSUD`).
