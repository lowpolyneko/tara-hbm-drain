#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include <numa.h>
#include <numaif.h>

#define PAGE_SHIFT 16                   /* getconf PAGESIZE = 65536      */
#define PAGE_ORDER 13                   /* hpage_pmd_size = 512 MiB      */
#define BLOCK_PAGES (1UL << PAGE_ORDER) /* 8192                          */
#define BLOCK_BYTES (BLOCK_PAGES << PAGE_SHIFT)

static const int gpu_node[] = {4, 12, 20, 28};
#define NGPU ((int)(sizeof gpu_node / sizeof gpu_node[0]))

static void pin(int cpu) {
  struct bitmask *m = numa_allocate_cpumask();

  numa_bitmask_setbit(m, cpu);
  if (numa_sched_setaffinity(0, m) != 0)
    fprintf(stderr, "cpu %d: sched_setaffinity: %s\n", cpu, strerror(errno));
  numa_bitmask_free(m);
}

/*
 * One node's parked pages and free pages, from /proc/zoneinfo.
 *
 * Each node and zone prints a "pages free" line (NR_FREE_PAGES) and then a
 * "pagesets" block of "cpu:" lines each followed by "count:", "high:" and
 * "batch:" (pcp->count, pcp->high, pcp->batch).
 *
 * "pages free" is summed over every zone of the node. "count" is taken only
 * from the node's Movable zone (the GPU's HBM).
 *
 * want >= 0 returns that one CPU's count, want < 0 the node's parked total.
 * *freep is the node's free pages either way.
 */
static unsigned long pcp_counts(int node, int want, unsigned long *freep) {
  char line[256], zone[32];
  FILE *fp = fopen("/proc/zoneinfo", "r");
  int cur = 0, cpu = -1, at_node = 0;
  unsigned long total = 0, got = 0, free_pages = 0;

  if (fp == NULL) {
    fprintf(stderr, "/proc/zoneinfo: %s\n", strerror(errno));
    return 0;
  }
  while (fgets(line, sizeof line, fp) != NULL) {
    int n;
    unsigned long v;

    if (sscanf(line, "Node %d, zone %31s", &n, zone) == 2) {
      at_node = (n == node);
      cur = (at_node && strcmp(zone, "Movable") == 0);
      cpu = -1;
      continue;
    }
    if (at_node && sscanf(line, " pages free %lu", &v) == 1) {
      free_pages += v;
      continue;
    }
    if (!cur)
      continue;
    if (sscanf(line, " cpu: %d", &n) == 1) {
      cpu = n;
    } else if (cpu >= 0 && sscanf(line, " count: %lu", &v) == 1) {
      if (cpu == want) {
        got = v;
        break;
      }
      total += v;
    }
  }
  fclose(fp);
  *freep = free_pages;
  return want >= 0 ? got : total;
}

/*
 * One order-13 page on node, or NULL. The mapping MUST be 512 MiB aligned.
 * mmap returns a 64 KiB aligned address, and hugepage_vma_check() requires the
 * VMA to be HPAGE_PMD_SIZE aligned, so an unaligned 512 MiB mapping will never
 * become an order-13 page.
 */
static void *map_block(int node) {
  unsigned long mask = BLOCK_BYTES - 1, head;
  struct bitmask *nm = numa_allocate_nodemask();
  char *raw, *p;

  numa_bitmask_setbit(nm, node);

  raw = mmap(NULL, 2 * BLOCK_BYTES, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (raw == MAP_FAILED) {
    fprintf(stderr, "node %d: mmap: %s\n", node, strerror(errno));
    numa_bitmask_free(nm);
    return NULL;
  }
  head = (BLOCK_BYTES - ((unsigned long)raw & mask)) & mask;
  p = raw + head;
  if (head != 0)
    munmap(raw, head);
  munmap(p + BLOCK_BYTES, BLOCK_BYTES - head);

  if (mbind(p, BLOCK_BYTES, MPOL_BIND, nm->maskp, nm->size, 0) != 0) {
    fprintf(stderr, "node %d: mbind: %s\n", node, strerror(errno));
    numa_bitmask_free(nm);
    munmap(p, BLOCK_BYTES);
    return NULL;
  }
  numa_bitmask_free(nm);
  madvise(p, BLOCK_BYTES, MADV_HUGEPAGE);
  memset(p, 0, BLOCK_BYTES); /* writing maps the zero page */

  return p;
}

/*
 * Release every parked pageblock on the node, one per CPU.
 */
static unsigned long drain_node(int node) {
  unsigned long drained = 0;
  int cpu, n = numa_num_configured_cpus();

  for (cpu = 0; cpu < n; cpu++) {
    unsigned long v, before, after, now;
    void *p;

    v = pcp_counts(node, cpu, &before);
    if (v < BLOCK_PAGES)
      continue; /* not a whole pageblock */
    pin(cpu);
    p = map_block(node);
    if (p != NULL) {
      munmap(p, BLOCK_BYTES);
      after = pcp_counts(node, cpu, &now);
      printf("  node %d cpu %3d  %lu -> %lu  node free %lu -> %lu\n", node, cpu,
             v, after, before, now);
      drained += v > after ? v - after : 0;
    }
  }
  return drained;
}

int main(void) {
  unsigned long total = 0, drained = 0;
  int i;

  for (i = 0; i < NGPU; i++) {
    unsigned long sum, free_pages;

    sum = pcp_counts(gpu_node[i], -1, &free_pages);
    printf("node %2d  pcp %8lu pages (%lu MiB)  free %lu\n", gpu_node[i], sum,
           sum << PAGE_SHIFT >> 20, free_pages);
    total += sum;
  }
  printf("parked total %lu pages (%lu MiB)\n\n", total,
         total << PAGE_SHIFT >> 20);

  for (i = 0; i < NGPU; i++)
    drained += drain_node(gpu_node[i]);

  printf("\ndrained %lu pages (%lu MiB)\n", drained,
         drained << PAGE_SHIFT >> 20);
  return 0;
}
