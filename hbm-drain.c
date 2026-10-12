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

#define NGPU ((int)(sizeof gpu_node / sizeof gpu_node[0]))
static const int gpu_node[] = {4, 12, 20, 28};

struct pcp {
  unsigned long cpu, total, free;
};

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
 * "free" is summed over every zone of the node, "total" over every CPU of the
 * node's Movable zone (the GPU's HBM), and "cpu" is that CPU's own count (cpu
 * < 0 leaves it 0).
 */
static struct pcp pcp_counts(int node, int cpu) {
  struct pcp r = {0, 0, 0};
  char line[256], zone[32];
  FILE *fp = fopen("/proc/zoneinfo", "r");
  int cur = 0, line_cpu = -1, at_node = 0;

  if (fp == NULL) {
    fprintf(stderr, "/proc/zoneinfo: %s\n", strerror(errno));
    return r;
  }
  while (fgets(line, sizeof line, fp) != NULL) {
    int n;
    unsigned long v;

    if (sscanf(line, "Node %d, zone %31s", &n, zone) == 2) {
      at_node = (n == node);
      cur = (at_node && strcmp(zone, "Movable") == 0);
      continue;
    }
    if (at_node && sscanf(line, " pages free %lu", &v) == 1) {
      r.free += v;
      continue;
    }
    if (!cur)
      continue;
    if (sscanf(line, " cpu: %d", &n) == 1) {
      line_cpu = n;
    } else if (line_cpu >= 0 && sscanf(line, " count: %lu", &v) == 1) {
      if (line_cpu == cpu)
        r.cpu = v;
      r.total += v;
    }
  }
  fclose(fp);
  return r;
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
    struct pcp before, after;
    void *p;

    before = pcp_counts(node, cpu);
    if (before.cpu < BLOCK_PAGES)
      continue; /* not a whole pageblock */
    pin(cpu);
    p = map_block(node);
    if (p != NULL) {
      munmap(p, BLOCK_BYTES);
      after = pcp_counts(node, cpu);
      printf("  node %d cpu %3d  %lu -> %lu  node free %lu -> %lu\n", node, cpu,
             before.cpu, after.cpu, before.free, after.free);
      drained += before.cpu > after.cpu ? before.cpu - after.cpu : 0;
    }
  }
  return drained;
}

int main(void) {
  unsigned long total = 0, drained = 0;
  int i;

  for (i = 0; i < NGPU; i++) {
    struct pcp s = pcp_counts(gpu_node[i], -1);

    printf("node %2d  pcp %8lu pages (%lu MiB)  free %lu\n", gpu_node[i],
           s.total, s.total << PAGE_SHIFT >> 20, s.free);
    total += s.total;
  }
  printf("parked total %lu pages (%lu MiB)\n\n", total,
         total << PAGE_SHIFT >> 20);

  for (i = 0; i < NGPU; i++)
    drained += drain_node(gpu_node[i]);

  printf("\ndrained %lu pages (%lu MiB)\n", drained,
         drained << PAGE_SHIFT >> 20);
  return 0;
}
