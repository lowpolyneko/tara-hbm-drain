# HBM Residual on the Tara GH200 Nodes

```sh
$ make
$ ./hbm-drain
node  4  pcp   311296 pages (19456 MiB)  free 1243452
node 12  pcp        4 pages (0 MiB)      free 1556330
node 20  pcp   417803 pages (26112 MiB)  free 1138657
node 28  pcp   548864 pages (34304 MiB)  free 1007387
parked total 1277967 pages (79872 MiB)

  node 4 cpu  74  8192 -> 0  node free 1243452 -> 1251644
  node 4 cpu  75  8192 -> 0  node free 1251644 -> 1259836
  ...
  node 28 cpu 215  8192 -> 0  node free 1548059 -> 1556251

drained 1277952 pages (79872 MiB)
```

On a node with residue it releases one parked pageblock per CPU. On a node that
already reads ~0 it does nothing.

## What's the Residual?

The residue is **free pages parked on a zone's per-CPU page lists**.

Each GPU's HBM is onlined as a CPU-less NUMA node whose only populated zone is
`ZONE_MOVABLE` (GPU0 -> node 4, GPU1 -> 12, GPU2 -> 20, GPU3 -> 28; each
1556480 pages of 64 KiB). **`nvidia-smi memory.used` on these nodes is not a
measurement of GPU memory.** It is the node's

```
MemUsed = MemTotal - MemFree = present_pages - NR_FREE_PAGES
```

`NR_FREE_PAGES` excludes pages parked on a zone's per-CPU page lists
(`pcp->count`). Example residual measured on a real Tara node:

| | pages |
|---|---|
| `present` | 1,556,480 |
| `NR_FREE_PAGES` | 1,507,194 |
| buddy free (`/proc/buddyinfo`) | 1,507,194 |
| **pagesets (`pcp->count`)** | **49,286** |
| every page class | 0 |
| `MemUsed` | **49,286** |

`NR_FREE_PAGES` counts the kernel page allocator's own free lists only. The
allocator keeps free pages in power-of-two blocks, splitting them for small
requests and merging adjacent "buddies" for large ones. A zone's per-CPU page
lists sit in front of it, holding pages just freed or about to be handed out,
meaning a parked page is both free and not in any buddy free list.

The whole usage is parked pages. They are technically free, and the next
allocation on the node will take them.

## How the Drain Works

It reads each GPU node's per-CPU parked counts from `/proc/zoneinfo`. For every
CPU whose `count` is at least one pageblock (8192 pages, 512 MiB) it:

1. pins to that CPU
2. maps an aligned 512 MiB block on the GPU, bound with `mbind(MPOL_BIND)` and
   `MADV_HUGEPAGE`
3. writes to it, so it faults in as one order-13 page
4. `munmap`s it
5. re-reads the node's counts

The allocation in step 2 takes the parked entry off the pcp list, and the
`munmap` in step 4 returns it to the buddy allocator.

Linux allocates memory in power-of-two blocks called *orders*: order N is 2^N
contiguous pages. With 64 KiB pages, order 13 is 8192 pages = 512 MiB. This is
the `pageblock_order`: the unit the buddy allocator hands out and takes back.
It's also the size of one transparent huge page.

The parked pageblock sits on the per-CPU list at order 13. An allocation takes
from the per-CPU list for its *own* order, so only an order-13 allocation looks
at the free list the parked entry is on.

CPUs below one pageblock are skipped. On a CPU with no parked pages, the
allocation refills two pageblocks from the buddy allocator and hands out one,
meaning it would *park* a block rather than release.

## Alternative Fixes

`NVreg_CoherentGPUMemoryMode=driver` (CDMM) makes the driver manage GPU memory
and stops it being exposed as a NUMA node at all. There is then nothing to park
on.

```
echo 'options nvidia NVreg_CoherentGPUMemoryMode=driver' | \
  sudo tee /etc/modprobe.d/nvidia-openrm.conf
# then reload the driver or reboot
```

`CoherentGPUMemoryMode: ""` is what's set, which defaults to NUMA mode.
