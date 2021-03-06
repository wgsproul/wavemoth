#undef NDEBUG

#define _GNU_SOURCE

/* libc */
#include <complex.h>
#include <assert.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* intrinsics */
#include <xmmintrin.h>
#include <emmintrin.h>

/* OS, Numa, pthreads, OpenMP */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <numa.h>
#include <numaif.h>

#include <pthread.h>
#include <sched.h>

#include <sys/times.h>
#include <time.h>

/* FFTW3 */
#include <fftw3.h>

/* Wavemoth */

#include "wavemoth_private.h"
#include "wavemoth_error.h"
#include "fmm1d.h"
#include "blas.h"
#include "butterfly_utils.h"
#include "legendre_transform.h"

typedef __m128d m128d;

#define CACHELINE 64
#define THREADS_PER_CPU 1
#define CONCURRENT_MEMORY_BUS_USE 1
/*
Every time resource format changes, we increase this, so that
we can keep multiple resource files around and jump in git
history.
*/
#define RESOURCE_FORMAT_VERSION 1
/*#define RESOURCE_HEADER "butterfly-compressed matrix data"*/

#define PI 3.14159265358979323846

#define PLANTYPE_HEALPIX 0x0

#define FFT_CHUNK_SIZE 4

static INLINE int imin(int a, int b) {
  return (a < b) ? a : b;
}

static INLINE size_t zmax(size_t a, size_t b) {
  return (a > b) ? a : b;
}

/** A more useful mod function; the result will have the same sign as
    the divisor rather than the dividend.
 */
static INLINE int imod_divisorsign(int a, int b) {
  int r;
  assert(-7 % 4 == -3); /* Behaviour of % is implementation-defined until C99 */
  r = a % b;
  r += ((r != 0) & ((r ^ b) < 0)) * b;
  return r;
}

/*
Global storage of precomputed data.

Precomputed data is stored as:
 - Array of precomputation_t indexed by resolution nside_level (Nside = 2**nside_level)
*/

#define MAX_NSIDE_LEVEL 15


/*
Precomputed
*/
static precomputation_t precomputed_data[MAX_NSIDE_LEVEL + 1];

#define MAX_RESOURCE_PATH 2048
static char global_resource_path[MAX_RESOURCE_PATH];

/*
Private
*/

static void print_array(char *msg, double* arr, bfm_index_t len) {
  bfm_index_t i;
  printf("%s ", msg);
  for (i = 0; i != len; ++i) {
    printf("%e ", arr[i]);
  }
  printf("\n");
}


static double walltime() {
  struct timespec tv;
  clock_gettime(CLOCK_REALTIME, &tv);
  return tv.tv_sec + 1e-9 * tv.tv_nsec;
}

/*
Public
*/

static int configured = 0;

void wavemoth_configure(char *resource_path) {
  /*check(!configured, "Already configured");*/
  configured = 1;
  strncpy(global_resource_path, resource_path, MAX_RESOURCE_PATH);
  global_resource_path[MAX_RESOURCE_PATH - 1] = '\0';
  memset(precomputed_data, 0, sizeof(precomputed_data));
}

static void wavemoth_get_resources_filename(char *filename, size_t buflen, int Nside) {
  snprintf(filename, buflen, "%s/rev%d/%d.dat",
           global_resource_path, RESOURCE_FORMAT_VERSION,
           Nside);
  filename[buflen - 1] = '\0';
}

int wavemoth_query_resourcefile(char *filename, int *out_Nside, int *out_lmax) {
  FILE *fd;
  int64_t fields[3];

  fd = fopen(filename, "r");
  if (fd == NULL) return -1;
  checkf(fread(fields, sizeof(int64_t[3]), 1, fd) == 1, "Could not read file header: %s", filename);
  fclose(fd);
  *out_lmax = fields[0];
  *out_Nside = fields[2];
  return 0;
}

/*
#define PAGESIZE 4096
#define WEAK __attribute__((weak))
#define __NR_move_pages 279
#define MPOL_MF_STRICT  (1<<0)
#define MPOL_MF_MOVE	(1<<1) 
#define MPOL_MF_MOVE_ALL (1<<2)
long WEAK move_pages(int pid, unsigned long count,
	void **pages, const int *nodes, int *status, int flags) {
  return syscall(__NR_move_pages, pid, count, pages, nodes, status, flags);
}


static void migrate_pages_numa(void *start, size_t len, int node) {
  start -= (size_t)start % PAGESIZE;
  len += (size_t)start % PAGESIZE;
  if (len % PAGESIZE > 0) {
    len += PAGESIZE - (len % PAGESIZE);
  }
  size_t pagecount = len / PAGESIZE;
  void *pages[1024];
  int nodes[1024];
  int status[1024];
  for (size_t i = 0; i < pagecount; i += 1024) {
    size_t stop = imin(1024, pagecount - i);
    for (size_t j = 0; j != stop; j++) {
      pages[j] = start + (i * 1024 + j) * PAGESIZE;
      nodes[j] = node;
      status[j] = 0;
    }
    int ret = move_pages(getpid(), stop, pages, nodes, status, MPOL_MF_MOVE);
    for (size_t j = 0; j != stop; j++) {
      if (status[j] < 0) {
	printf("status: %d of %d %d %d %d %d %d %d %d\n", status[j],
	EACCES, EINVAL, ENODEV, ENOENT, EPERM, ENOMEM, E2BIG, ESRCH 
	       );
	//-1= EPERM
      }
    }
    printf("%d\n", ret);
  }
  }*/

//int wavemoth_copy_resources(precomputation_t *data


int wavemoth_mmap_resources(char *filename, precomputation_t *data, int *out_Nside) {
  int fd;
  struct stat fileinfo;
  int64_t mmax, lmax, m, n, odd, Nside;
  int64_t *offsets = NULL;
  m_resource_t *rec;
  int retcode;
  char *head;

  /*
    Memory map buffer
   */
  data->mmapped_buffer = MAP_FAILED;
  fd = open(filename, O_RDONLY);
  if (fd == -1) goto ERROR;
  if (fstat(fd, &fileinfo) == -1) goto ERROR;
  data->mmap_len = fileinfo.st_size;
  data->mmapped_buffer = mmap(NULL, data->mmap_len, PROT_READ, MAP_SHARED,
                              fd, 0);
  if (data->mmapped_buffer == MAP_FAILED) goto ERROR;
  close(fd);
  fd = -1;
  head = data->mmapped_buffer;
  /* Read mmax, allocate arrays, read offsets */
  lmax = read_int64(&head);
  mmax = read_int64(&head);
  *out_Nside = Nside = read_int64(&head);
  data->matrices = malloc(sizeof(m_resource_t[mmax + 1]));
  if (data->matrices == NULL) goto ERROR;
  data->lmax = lmax;
  data->mmax = mmax;
  offsets = (int64_t*)head;
  /* Assign pointers to compressed matrices. This doesn't actually load it
     into memory, just set out pointers into virtual memory presently on
     disk. */
  for (m = 0; m != mmax + 1; ++m) {
    n = (mmax - m) / 2;
    for (odd = 0; odd != 2; ++odd) {
      rec = &data->matrices[m];
      head = data->mmapped_buffer + offsets[4 * m + 2 * odd];
      if (head == data->mmapped_buffer) {
        /* For debugging/benchmarking, sometimes matrices are missing.
           In those cases the offset is registered as 0. */
        rec->data[odd] = NULL;
        continue;
      }
      rec->data[odd] = head;
      rec->len[odd] = offsets[4 * m + 2 * odd + 1];
    }
  }
  retcode = 0;
  goto FINALLY;
 ERROR:
  retcode = -1;
  if (fd != -1) close(fd);
  if (data->mmapped_buffer == MAP_FAILED) {
    munmap(data->mmapped_buffer, data->mmap_len);
    data->mmapped_buffer = NULL;
  }
 FINALLY:
  return retcode;
}

precomputation_t* wavemoth_fetch_resource(int Nside) {
  int Nside_level = 0, tmp, got_Nside;
  char filename[MAX_RESOURCE_PATH];

  if (!configured) {
    return NULL;
  }
  wavemoth_get_resources_filename(filename, MAX_RESOURCE_PATH, Nside);
  tmp = Nside;
  while (tmp /= 2) ++Nside_level;
  checkf(Nside_level < MAX_NSIDE_LEVEL + 1, "Nside=2**%d but maximum value is 2**%d",
         Nside_level, MAX_NSIDE_LEVEL);
  if (precomputed_data[Nside_level].refcount == 0) {
    check(wavemoth_mmap_resources(filename, precomputed_data + Nside_level, &got_Nside) == 0,
          "resource load failed");
    checkf(Nside == got_Nside, "Loading precomputation: Expected Nside=%d but got %d in %s",           Nside, got_Nside, filename);
  }
  ++precomputed_data[Nside_level].refcount;
  return &precomputed_data[Nside_level];
}

void wavemoth_release_resource(precomputation_t *data) {
  return;
  --data->refcount;
  if (data->refcount == 0) {
    munmap(data->mmapped_buffer, data->mmap_len);
    data->mmapped_buffer = NULL;
    free(data->matrices);
  }
}

static void bitmask_or(struct bitmask *a, struct bitmask *b, struct bitmask *out) {
  int n = numa_bitmask_nbytes(a) * 8;
  for (int i = 0; i != n; ++i) {
    if (numa_bitmask_isbitset(a, i) && numa_bitmask_isbitset(b, i)) {
      numa_bitmask_setbit(out, i);
    } else {
      numa_bitmask_clearbit(out, i);
    }
  }
}

typedef void (*thread_main_func_t)(wavemoth_plan, int, int, int, void*);

typedef struct {
  void *ctx;
  thread_main_func_t func;
  wavemoth_plan plan;
  int inode, icpu, ithread;
} thread_ctx_t;

static void *thread_main_adaptor(void *ctx_) {
  thread_ctx_t *ctx = ctx_;

  /* Ensure that the thread only allocates memory locally. */
  struct bitmask *mask = numa_allocate_nodemask();
  numa_bitmask_clearall(mask);
  numa_bitmask_setbit(mask, ctx->plan->node_plans[ctx->inode]->node_id);
  numa_set_membind(mask);
  numa_free_nodemask(mask);

  ctx->func(ctx->plan, ctx->inode, ctx->icpu, ctx->ithread, ctx->ctx);
  return NULL;
}

static int wavemoth_run_in_threads(wavemoth_plan plan, thread_main_func_t func, int threads_per_cpu,
                                  void *ctx, pthread_t *threads,
                                  thread_ctx_t *adaptor_ctx) {
  /* Spawn pthreads on the CPUs designated in the plan, and wait for them
     to finish. Each thread will only allocate memory locally, because
     of numa_set_membind in thread_main_adaptor. */
  int n = plan->ncpus_total * threads_per_cpu;
  thread_ctx_t adaptor_ctx_on_stack[n];
  pthread_t threads_on_stack[n];
  if (threads == NULL) {
    threads = threads_on_stack;
    adaptor_ctx = adaptor_ctx_on_stack;
  }
  int idx = 0;
  for (int inode = 0; inode != plan->nnodes; ++inode) {
    for (int icpu = 0; icpu != plan->node_plans[inode]->ncpus; ++icpu) {
      cpu_set_t cpu_set;
      pthread_attr_t attr;
      CPU_ZERO(&cpu_set);
      CPU_SET(plan->node_plans[inode]->cpu_plans[icpu].cpu_id, &cpu_set);
      pthread_attr_init(&attr);
      pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpu_set);
      for (int ithread = 0; ithread != threads_per_cpu; ++ithread) {
        adaptor_ctx[idx] = (thread_ctx_t){ ctx, func, plan, inode, icpu, ithread };
        pthread_create(&threads[idx], &attr, thread_main_adaptor, &adaptor_ctx[idx]);
        idx++;
      }
      pthread_attr_destroy(&attr);
    }
  }
  assert(idx == n);
  if (threads == threads_on_stack) {
    for (idx = 0; idx != n; ++idx) {
      pthread_join(threads[idx], NULL);
    }
  }
  return n;
}

static void wavemoth_create_plan_thread(wavemoth_plan plan, int inode, int icpu,
                                       int ithread, void *ctx); /* forward decl */

static void wait_for_execute_thread(wavemoth_plan plan, int inode, int icpu,
                                    int ithread, void *ctx);

static int next_numa_node(struct bitmask *nodemask, int node_id, int nnodes) {
  do {
    node_id = (node_id + 1) % nnodes;
  } while (!numa_bitmask_isbitset(nodemask, node_id));
  return node_id;
}

wavemoth_plan wavemoth_plan_to_healpix(int Nside, int lmax, int mmax, int nmaps,
                                     int nthreads, double *input, double *output,
                                     int ordering, unsigned flags,
                                     char *resource_filename) {
  wavemoth_plan plan = malloc(sizeof(struct _wavemoth_plan));
  size_t nrings;
  int out_Nside;
  wavemoth_grid_info *grid;
  bfm_index_t start, stop;

  /* Simple attribute assignment */
  nrings = 4 * Nside - 1;
  plan->type = PLANTYPE_HEALPIX;
  plan->input = input;
  plan->output = output;
  plan->grid = grid = wavemoth_create_healpix_grid_info(Nside);
  plan->nmaps = nmaps;
  plan->Nside = Nside;
  plan->lmax = lmax;
  plan->mmax = mmax;
  plan->flags = flags;
  plan->nthreads = nthreads;

  /* Figure out how threads should be distributed. We query NUMA for
     the nodes we can run on (intersection of cpubind and membind),
     and then fill up node-by-node until we
     hit nthreads. */

  if (nthreads <= 0) {
    fprintf(stderr, "Require nthreads > 0\n");
    return NULL; /* TODO */
  }

  /* Find intersection of cpubind and membind */
  struct bitmask *nodemask = numa_allocate_nodemask(), *a, *b;
  a = numa_get_run_node_mask();
  b = numa_get_membind();
  bitmask_or(a, b, nodemask);
  numa_free_nodemask(a);
  numa_free_nodemask(b);

  plan->ncpus_total = 0;

  struct bitmask *cpumask = numa_allocate_cpumask();
  int icpu = 0;
  int max_node_id = numa_max_node();
  size_t nm_bound = (mmax + 1);
  size_t inode = 0;

  /* Set up node-specific structures for all available nodes */
  inode = 0;
  for (int node_id = 0; node_id != max_node_id + 1; ++node_id) {
    if (numa_bitmask_isbitset(nodemask, node_id)) {
      size_t bufsize = sizeof(wavemoth_node_plan_t) + 
        sizeof(m_resource_t[nm_bound]) + 
        sizeof(double*[mmax + 1]);
      char *buf = numa_alloc_onnode(bufsize, node_id);
      wavemoth_node_plan_t *node_plan = (void*)buf;
      buf += sizeof(wavemoth_node_plan_t);
      if ((size_t)buf % sizeof(void*) != 0) {
        buf += sizeof(void*) - (size_t)buf % sizeof(void*);
      }
      //node_plan->m_to_phase_ring = (void*)buf;
      buf += sizeof(double*[mmax + 1]);
      node_plan->m_resources = (void*)buf;
      buf += sizeof(m_resource_t[nm_bound]);

      node_plan->node_id = node_id;
      node_plan->ncpus = 0;      
      node_plan->cpu_plans = malloc(sizeof(wavemoth_cpu_plan_t[16])); // TODO
      sem_init(&node_plan->memory_bus_semaphore, 0, CONCURRENT_MEMORY_BUS_USE);
      pthread_mutex_init(&node_plan->queue_lock, NULL);
      plan->node_plans[inode] = node_plan;
      inode++;
    }
  }
  int nnodes = plan->nnodes = inode;

  /* Distribute CPUs in round-robin fashion on available nodes. The
     assumption is that the nodemask really does specify what nodes we
     can use, and this equal distribution is mainly to enable/disable
     use of Intel hyperthreading. I.e., for determining program
     scalability one *must* use numactl! */
  inode = 0;
  int cpus_assigned;
  for (cpus_assigned = 0; cpus_assigned != nthreads; ++cpus_assigned) {
    wavemoth_node_plan_t *node_plan = plan->node_plans[inode];
    inode = (inode + 1) % nnodes;

    /* Walk through cpuid's and check which is next on this node */
    int cpu_id = (node_plan->ncpus == 0) ? 0 : 
      node_plan->cpu_plans[node_plan->ncpus - 1].cpu_id + 1;
    if (numa_node_to_cpus(node_plan->node_id, cpumask) < 0) {
      check(0, "numa_node_to_cpus failed");
    }
    for (; cpu_id < numa_bitmask_nbytes(cpumask) * 8; ++cpu_id) {
      if (numa_bitmask_isbitset(cpumask, cpu_id)) {
        wavemoth_cpu_plan_t *cpu_plan = &node_plan->cpu_plans[node_plan->ncpus];
        cpu_plan->cpu_id = cpu_id;
        node_plan->ncpus++;
        break;
      }
    }
    if (cpu_id == numa_bitmask_nbytes(cpumask) * 8) {
      check(0, "Requested number of CPUs not available\n");
    }
  }
  plan->ncpus_total = cpus_assigned;

  numa_free_nodemask(nodemask);
  numa_free_cpumask(cpumask);


  /* Distribute Legendre transform tasks to nodes */
  size_t nms[nnodes];
  memset(nms, 0, sizeof(nms));
  inode = 0;
  for (int m = 0; m != mmax + 1; ++m) {
    int im = nms[inode];
    plan->node_plans[inode]->m_resources[im].m = m;
    nms[inode]++;
    inode = (inode + 1) % nnodes;
  }
  for (int inode = 0; inode != nnodes; ++inode) {
    plan->node_plans[inode]->nm = nms[inode];
  }
  /* Figure out how work should be distributed among nodes. */
  /* First allocate information buffers */
  int ring_block_size = FFT_CHUNK_SIZE;
  size_t nring_bound = nrings;
  for (int inode = 0; inode != nnodes; ++inode) {
    for (int icpu = 0; icpu != plan->node_plans[inode]->ncpus; ++icpu) {
      wavemoth_cpu_plan_t *td = &plan->node_plans[inode]->cpu_plans[icpu];
      td->buf_size = sizeof(ring_pair_info_t[nring_bound]);
      td->ring_pairs = numa_alloc_onnode(td->buf_size, inode);
      check(td->ring_pairs != NULL, "Could not allocate");
    }
  }
  /* Distribute rings */
  size_t mid_ring = plan->grid->mid_ring;
  size_t nrings_half = mid_ring + 1;
  size_t iring = 0;
  for (int inode = 0; inode != nnodes; ++inode) {
    for (int icpu = 0; icpu != plan->node_plans[inode]->ncpus; ++icpu) {
      wavemoth_cpu_plan_t *cpu_plan = &plan->node_plans[inode]->cpu_plans[icpu];
      cpu_plan->nrings = 0;
    }
  }
  while (iring < nrings_half) {
    for (int inode = 0; inode != nnodes; ++inode) {
      for (int icpu = 0; icpu != plan->node_plans[inode]->ncpus; ++icpu) {
        size_t stop = imin(nrings_half, iring + ring_block_size);
        size_t rings_in_block = stop - iring;

        wavemoth_cpu_plan_t *cpu_plan = &plan->node_plans[inode]->cpu_plans[icpu];
        ring_pair_info_t *ring_pairs = cpu_plan->ring_pairs;
        for (size_t j = 0; j != rings_in_block; ++j) {
          ring_pair_info_t *ri = &ring_pairs[cpu_plan->nrings + j];
          ri->ring_number = iring + j;
          ri->phi0 = grid->phi0s[mid_ring + iring + j];
          ri->offset_top = grid->ring_offsets[mid_ring - ri->ring_number];
          ri->offset_bottom = grid->ring_offsets[mid_ring + ri->ring_number];
          ri->length = (grid->ring_offsets[mid_ring + ri->ring_number + 1] - 
                        grid->ring_offsets[mid_ring + ri->ring_number]);
        }
        cpu_plan->nrings += rings_in_block;
        iring += rings_in_block;
      }
    }
  }

  /* Load map of resources globally... */
  if (resource_filename != NULL) {
    /* Used in debugging/benchmarking */
    plan->resources = malloc(sizeof(precomputation_t));
    plan->did_allocate_resources = 1;
    checkf(wavemoth_mmap_resources(resource_filename, plan->resources, &out_Nside) == 0,
           "Error in loading resource %s", resource_filename);
    check(Nside < 0 || out_Nside == Nside, "Incompatible Nside");
    Nside = out_Nside;
  } else {
    check(Nside >= 0, "Invalid Nside");
    plan->did_allocate_resources = 0;
    plan->resources = wavemoth_fetch_resource(Nside);
  }
  check(mmax == plan->resources->mmax, "Incompatible mmax");
  check(lmax == plan->resources->lmax, "Incompatible lmax");


  /* Compute stride for work_q */
  size_t nvecs = 2 * plan->nmaps;
  size_t s = nvecs * nrings_half;
  assert(CACHELINE % sizeof(double) == 0);
  s *= sizeof(double);
  if (s % CACHELINE != 0) s += CACHELINE - s % CACHELINE;
  s /= sizeof(double);
  plan->work_q_stride = s;

  /* Spawn threads to do thread-local intialization:
     Copy over precomputed data, initialize butterfly & FFT plans */
  
  struct {
    pthread_mutex_t mutex;
    pthread_barrier_t barrier, node_barrier;
  } sync;
  pthread_mutex_init(&sync.mutex, NULL);
  //  pthread_barrier_init(&sync.barrier, NULL, nthreads);
  //pthread_barrier_init(&sync.node_barrier, NULL, nnodes);
  wavemoth_run_in_threads(plan, &wavemoth_create_plan_thread, 1, &sync, NULL, NULL);
  //pthread_barrier_destroy(&sync.barrier);
  //pthread_barrier_destroy(&sync.node_barrier);
  pthread_mutex_destroy(&sync.mutex);

  /* Now that work_q has been allocated, set up m_to_phase_ring */
  double **m_to_phase_ring = plan->m_to_phase_ring = malloc(sizeof(double[mmax + 1]));
  size_t work_stride = plan->work_q_stride;
  for (inode = 0; inode != nnodes; ++inode) {
    wavemoth_node_plan_t *np = plan->node_plans[inode];
    for (size_t im = 0; im != np->nm; ++im) {
      m_to_phase_ring[np->m_resources[im].m] = np->work_q + (2 * im) * work_stride;
    }
  }

  thread_ctx_t adaptor_ctx[nthreads];
  plan->destructing = 0;
  plan->execute_threads = malloc(sizeof(pthread_t[nthreads]));
  pthread_barrier_init(&plan->execute_barrier, NULL, nthreads + 1);
  wavemoth_run_in_threads(plan, &wait_for_execute_thread, 1, NULL,
                         plan->execute_threads, adaptor_ctx);
  /* Wait until adaptor_ctx is no longer needed */
  pthread_barrier_wait(&plan->execute_barrier);
  return plan;
}

int _dummy = 0;

static void migrate_data(void *startptr, size_t len, int node) {
  const int CHUNK = 512;
  const size_t PAGESIZE = getpagesize();
  void *pages[CHUNK];
  int nodes[CHUNK];
  int status[CHUNK];

  char *start = startptr, *end = (char*)startptr + len;
  start -= ((size_t)start & (PAGESIZE - 1));
  end -= ((size_t)end & (PAGESIZE - 1));

  /* Apparently the return values and status codes are fairly
     useless; if the page is already on the node, errors will be given,
     and the function call itself fails if no pages were moved (for that
     reason). But things seem to work and we just ignore errors. */
  int idx = 0;
  for (char *ptr = start; ptr != end; ptr += PAGESIZE) {
    /* touch page */
    _dummy += *ptr;
    /* migrate page */
    pages[idx] = ptr;
    nodes[idx] = node;
    status[idx] = 0;
    idx++;
    if (idx == CHUNK) {
      numa_move_pages(0, idx, pages, nodes, status, MPOL_MF_MOVE_ALL);
      idx = 0;
    }
  }
  if (idx > 0) {
    //    numa_move_pages(0, idx, pages, nodes, status, MPOL_MF_MOVE);
  }
}

static void* round_down_to(void *addr, size_t size) {
  char *p = addr;
  if ((size_t)p % size > 0) p -= ((size_t)p % size);
  return p;
}

static void* round_up_to(void *addr, size_t size) {
  char *p = addr;
  if ((size_t)p % size > 0) p += size - ((size_t)p % size);
  return p;
}

static void wavemoth_create_plan_thread(wavemoth_plan plan, int inode, int icpu,
                                       int ithread, void *ctx) {
  struct {
    pthread_mutex_t mutex;
      //    pthread_barrier_t barrier, node_barrier;
  } *sync = ctx;

  const int PAGESIZE = getpagesize();

  wavemoth_node_plan_t *node_plan = plan->node_plans[inode];
  wavemoth_cpu_plan_t *cpu_plan = &node_plan->cpu_plans[icpu];
  size_t nm = node_plan->nm;
  int nmaps = plan->nmaps;
  size_t nrings_half = plan->grid->mid_ring + 1;

  sem_init(&cpu_plan->cpu_lock, 0, 1);

  /* First, ensure that data is all faulted into memory in a nice,
     serial order so that we don't have to wait forever for harddisk
     seeks. However, each page should be faulted by the node that
     wants the page in its local memory. We assume that im refers to
     tasks in order of increasing m. */
  size_t im = 0;
  double t0 = walltime();
  size_t faulted_bytes = 0;
  if (icpu == 0) {
    for (int m = 0; m != plan->mmax + 1; ++m) {
      m_resource_t *m_resources_node = node_plan->m_resources;
      while (im < nm && m_resources_node[im].m < m) ++im;
      if (m_resources_node[im].m == m) {
        if (m % 97 == 0) {
          double t = walltime();
          printf("Loading m=%d on node %d (%.1f MB/s)\n", m, node_plan->node_id,
                 faulted_bytes / 1024.0 / 1024.0 / (t - t0));
          faulted_bytes = 0;
          t0 = t;
        }
        m_resource_t *m_resource_mmap = &plan->resources->matrices[m];
        for (int odd = 0; odd != 2; ++odd) {
          char *addr = m_resource_mmap->data[odd];
          size_t size = m_resource_mmap->len[odd];
          size_t npages = (size + PAGESIZE - 1) / PAGESIZE;
          char vec[npages];

          addr = round_down_to(addr, PAGESIZE);
          size = (char*)round_up_to(addr + size, PAGESIZE) - addr;
          int r = mincore(addr, size, vec);
          checkf(r == 0, "mincore failed: %x", r);
          for (size_t ipage = 0; ipage != npages; ++ipage) {
            if ((vec[ipage] & 0x1) == 0) {
              /* fault page */
              faulted_bytes += PAGESIZE;
              _dummy += *(addr + ipage * PAGESIZE);
            } else {
              /* TODO migrate page */
            }
          }
        }
      }
      /* All nodes wait in line for harddrive access */
      //      if (plan->nthreads > 1) pthread_barrier_wait(&sync->node_barrier);
    }
  }
  //  if (plan->nthreads > 1) pthread_barrier_wait(&sync->barrier);

  /* Copy matrix data into cpu_plans buffers. Stride by
     our thread-on-node number.

     While we're at it, inspect precomputed data to figure out buffer
     sizes (common for all, so synchronize/reduce-max at the end). */
  size_t k_max = 0, nblocks_max = 0;
  int do_copy = !((plan->flags & WAVEMOTH_NO_RESOURCE_COPY) == WAVEMOTH_NO_RESOURCE_COPY);
  for (im = icpu; im < nm; im += node_plan->ncpus) {
    m_resource_t *localres = &node_plan->m_resources[im];
    int m = localres->m;
    m_resource_t *fileres = &plan->resources->matrices[m];
    for (int odd = 0; odd != 2; ++odd) {
#if 0
      migrate_data(fileres->data[odd], fileres->len[odd], node_plan->node_id);
      localres->data[odd] = fileres->data[odd];
      localres->len[odd] = fileres->len[odd];
#else
      if (do_copy) {
        localres->data[odd] = memalign(4096, fileres->len[odd]);
        localres->len[odd] = fileres->len[odd];
        checkf(localres->data[odd] != NULL, "No memory allocated of size %ld on node %d",
               fileres->len[odd], node_plan->node_id);
        memcpy(localres->data[odd], fileres->data[odd], fileres->len[odd]);
      } else {
        localres->data[odd] = fileres->data[odd];
        localres->len[odd] = fileres->len[odd];
      }
#endif

      bfm_matrix_data_info info;
      bfm_query_matrix_data(localres->data[odd], &info);
      k_max = zmax(k_max, info.k_max);
      nblocks_max = zmax(nblocks_max, info.nblocks_max);
    }
  }

  /* reduce-max */
  if (icpu == 0) {
    /* root-cpu initializes to 0 */
    node_plan->k_max = node_plan->nblocks_max = 0;
  }

  //  if (plan->nthreads > 1) pthread_barrier_wait(&sync->barrier);

  /* all threads do max*/
  pthread_mutex_lock(&sync->mutex);
  node_plan->k_max = zmax(node_plan->k_max, k_max);
  node_plan->nblocks_max = zmax(node_plan->nblocks_max,
                                nblocks_max);
  pthread_mutex_unlock(&sync->mutex);
  //  if (plan->nthreads > 1) pthread_barrier_wait(&sync->barrier);
  /* all threads read back values */
  k_max = node_plan->k_max;
  nblocks_max = node_plan->nblocks_max;

  /* Allocate legendre-worker plans (>1 per cpu) */
  size_t legendre_work_size = wavemoth_legendre_transform_sse_query_work(2 * nmaps);
  size_t nvecs = 2 * plan->nmaps;
  size_t nmats = 2 * nm;

  cpu_plan->legendre_workers = malloc(sizeof(wavemoth_legendre_worker_t[THREADS_PER_CPU]));
  for (int w = 0; w != THREADS_PER_CPU; ++w) {
    wavemoth_legendre_worker_t *worker_plan = &cpu_plan->legendre_workers[w];
    worker_plan->bfm = bfm_create_plan(k_max, nblocks_max, 2 * nmaps,
                                       &node_plan->memory_bus_semaphore,
                                       &cpu_plan->cpu_lock);
    worker_plan->legendre_transform_work = 
      (legendre_work_size == 0) ? NULL : memalign(4096, legendre_work_size);
    worker_plan->work_a_l = memalign(4096, sizeof(double[(nvecs * (plan->lmax + 1))]));
  }

  /* Target q_m buffer (per node) */
  if (icpu == 0) {
    node_plan->work_q = memalign(4096, sizeof(double[nmats * plan->work_q_stride]));
  }

  /* For FFTs, we use inplace c2r/r2c. This means that the buffer per
     map must be as long as the longest ring + one complex
     coefficient: len(complex) = len(real) // 2 + 1. We allocate
     work space for FFT_CHUNK_SIZE rings in both the northern and southern
     hemisphere. */
  cpu_plan->work_fft = memalign(4096, sizeof(double[2 * FFT_CHUNK_SIZE * nmaps * 
                                                     (4 * plan->Nside + 2)]));

  /* Make FFT plans. FFTW is *not* thread-safe in the fftw_plan_X functions,
     but we *do* want to run it in each local thread, to properly benchmark
     using local memory. So, we serialize access to FFTW. Note that the
     fftw_execute_... functions *are* thread-safe (as long as used with
     different plans).
  */

  unsigned fftw_flags = FFTW_DESTROY_INPUT;
  fftw_flags |= (plan->flags & WAVEMOTH_MEASURE) ? FFTW_MEASURE : FFTW_ESTIMATE;

  pthread_mutex_lock(&sync->mutex);
  for (int i = 0; i != cpu_plan->nrings; ++i) {
    ring_pair_info_t *ri = &cpu_plan->ring_pairs[i];
    int ringlen = ri->length;
    ri->fft_plan = fftw_plan_many_dft_c2r(1, &ringlen, nmaps,
                                          (fftw_complex*)cpu_plan->work_fft, NULL, nmaps, 1,
                                          cpu_plan->work_fft, NULL, nmaps, 1,
                                          fftw_flags);
  }
  pthread_mutex_unlock(&sync->mutex);
}


void wavemoth_destroy_plan(wavemoth_plan plan) {
  int iring;

  plan->destructing = 1;
  pthread_barrier_wait(&plan->execute_barrier);
  for (int idx = 0; idx != plan->nthreads; ++idx) {
    pthread_join(plan->execute_threads[idx], NULL);
  }
  pthread_barrier_destroy(&plan->execute_barrier);


  /* Cleanup for all threads. Do not move to per-thread without a mutex,
     FFTW3 destructor access must be serialized!
   */
  /*  for (int ithread = 0; ithread != plan->nthreads; ++ithread) {
    wavemoth_cpu_plan_t *lp = &plan->cpu_plans[ithread];
    for (size_t iring = 0; iring != lp->nrings; ++iring) {
      fftw_destroy_plan(lp->ring_pairs[iring].fft_plan);
    }
    for (size_t im = 0; im != lp->nm; ++im) {
      for (int odd = 0; odd != 2; ++odd) {
        free(lp->m_resources[im].data[odd]);
      }
    }
    bfm_destroy_plan(lp->bfm);
    free(lp->work);
    free(lp->work_a_l);
    free(lp->work_fft);*/
    /* Free buffer for ring_pairs, m_resources, and m_to_phase_ring,
       allocated by numa_alloc. */
  //    numa_free(lp->buf, lp->buf_size);
  //}

  wavemoth_free_grid_info(plan->grid);
  wavemoth_release_resource(plan->resources);
  if (plan->did_allocate_resources) free(plan->resources);
  free(plan);
}

int64_t wavemoth_get_legendre_flops(wavemoth_plan plan, int m, int odd) {
  int64_t N, nvecs;
  bfm_matrix_data_info info;
  bfm_query_matrix_data(plan->resources->matrices[m].data[odd],
                        &info);
  N = info.element_count;
  nvecs = 2;
  N *= nvecs;
  return N * 2; /* count mul and add seperately */
}


static void legendre_transforms_thread(wavemoth_plan plan, int inode, int icpu,
                                       int ithread, void *ctx) {
  wavemoth_node_plan_t *node_plan = plan->node_plans[inode];
  wavemoth_cpu_plan_t *cpu_plan = &node_plan->cpu_plans[icpu];
  size_t nrings_half = plan->grid->mid_ring + 1;
  double *work_q = node_plan->work_q;
  int nvecs = 2 * plan->nmaps;
  size_t lmax = plan->lmax;
  size_t nm = node_plan->nm;
  size_t work_q_stride = plan->work_q_stride;

  wavemoth_legendre_worker_t *thread_plan = &cpu_plan->legendre_workers[ithread];
  assert(ithread == 0);

  size_t im;
  do {
    /* First grab the CPU lock */
    int val;
    /*sem_wait(&cpu_plan->cpu_lock);*/
    /* Fetch next work item from queue */
    pthread_mutex_lock(&node_plan->queue_lock);
    im = node_plan->im;
    if (im < nm) {
      ++node_plan->im;
    }
    pthread_mutex_unlock(&node_plan->queue_lock);

    if (im < nm) {
      /* Not done */
      m_resource_t *m_resource = &node_plan->m_resources[im];
      size_t m = m_resource->m;

      
      for (int odd = 0; odd < 2; ++odd) {
        double *target = work_q + (2 * im + odd) * plan->work_q_stride;
        wavemoth_perform_matmul(plan, thread_plan->bfm, m_resource->data[odd],
                               m, odd, nrings_half, target,
                               thread_plan->legendre_transform_work,
                               thread_plan->work_a_l);
      }
    }
    /*sem_post(&cpu_plan->cpu_lock);*/
  } while (im != nm);
}

void wavemoth_perform_legendre_transforms(wavemoth_plan plan) {
  /* Reset queue head for all nodes */
  for (int inode = 0; inode != plan->nnodes; ++inode) {
    plan->node_plans[inode]->im = 0;
  }
  /* Run threads */
  wavemoth_run_in_threads(plan, &legendre_transforms_thread, THREADS_PER_CPU, NULL,
                         NULL, NULL);
}


typedef struct {
  double *input, *input_pack_buf;
  char *work;
} transpose_apply_ctx_t;

static void pack_every_other(size_t nk, size_t nvecs, double *input, double *packed) {
  for (size_t k = 0; k != nk; ++k) {
    for (size_t j = 0; j != nvecs; j += 2) {
      m128d x = _mm_load_pd(input + 2 * k * nvecs + j);
      _mm_store_pd(packed + k * nvecs + j, x);
    }
  }
}

void pull_a_through_legendre_block(double *buf, size_t start, size_t stop,
                                   size_t nvecs, char *payload, size_t payload_len,
                                   void *ctx_) {
  transpose_apply_ctx_t *ctx = ctx_;
  double *input = ctx->input, *input_pack_buf = ctx->input_pack_buf;
  skip128(&payload);
  size_t row_start = read_int64(&payload);
  size_t row_stop = read_int64(&payload);
  size_t nk = row_stop - row_start;
  input += 2 * row_start * nvecs;
  if (nk <= 4 || start == stop) {
    double *A = read_aligned_array_d(&payload, (stop - start) * nk);
    pack_every_other(nk, nvecs, input, input_pack_buf);
    dgemm_ccc(input_pack_buf, A, buf,
              nvecs, stop - start, nk, 0.0);
  } else {
    size_t nstrips = read_int64(&payload);
    double *auxdata = read_aligned_array_d(&payload, 3 * (nk - 2));
    size_t rstart, cstart, cstop;
    cstart = 0;
    for (size_t i = 0; i != nstrips; ++i) {
      rstart = read_int64(&payload);
      cstop = read_int64(&payload);
      size_t nx_strip = cstop - cstart;
      size_t nk_strip = nk - rstart;
      if (nk - rstart <= 4) {
        double *A = read_aligned_array_d(&payload, nk_strip * nx_strip);
        pack_every_other(nk_strip, nvecs, input + 2 * rstart * nvecs, input_pack_buf);
        dgemm_ccc(input_pack_buf,
                  A,
                  buf + cstart * nvecs,
                  nvecs,
                  nx_strip,
                  nk_strip, 0.0);
      } else {
        double *x_squared = read_aligned_array_d(&payload, nx_strip);
        double *P0 = read_aligned_array_d(&payload, nx_strip);
        double *P1 = read_aligned_array_d(&payload, nx_strip);
        wavemoth_legendre_transform_pack(nk_strip, nvecs, input + 2 * rstart * nvecs,
                                        input_pack_buf);
        wavemoth_legendre_transform_sse(nx_strip, nk_strip, nvecs,
                                       input_pack_buf,
                                       buf + cstart * nvecs,
                                       x_squared,
                                       auxdata + 3 * rstart,
                                       P0, P1,
                                       ctx->work);
      }
      cstart = cstop;
    }
  }
}

void wavemoth_perform_matmul(wavemoth_plan plan, bfm_plan *bfm, char *matrix_data,
                            bfm_index_t m, int odd, size_t ncols,
                            double *output, char *legendre_transform_work,
                            double *work_a_l) {
  bfm_index_t l, lmax = plan->lmax, j;
  size_t nvecs = 2 * plan->nmaps;
  double *input_m = plan->input + nvecs * m * (2 * lmax - m + 3) / 2;
  input_m += odd * nvecs;

  transpose_apply_ctx_t ctx = { input_m, work_a_l, legendre_transform_work };
  int ret = bfm_transpose_apply_d(bfm,
                                  matrix_data,
                                  pull_a_through_legendre_block,
                                  output,
                                  ncols * nvecs,
                                  &ctx);
  checkf(ret == 0, "bfm_transpose_apply_d retcode %d", ret);
}

void wavemoth_cossin(double *out, size_t n, double x0, double delta) {
  /* Computes cos(x0 + i * delta) and sin(x0 + i * delta) */
  double a = sin(.5 * delta);
  a = 2.0 * a * a;
  m128d alpha = (m128d){ -a, -a };
  double b = sin(delta);
  m128d beta = (m128d){ b, -b };
  m128d y = (m128d){ cos(x0), sin(x0) };
  _mm_store_pd(out, y);
  out += 2;
  n--;
  while (n--) {
    m128d t = _mm_mul_pd(alpha, y);
    m128d u = _mm_mul_pd(beta, y);
    u = _mm_shuffle_pd(u, u, _MM_SHUFFLE2(0, 1)); /* flip elements of u*/
    u = _mm_add_pd(t, u);
    y = _mm_add_pd(y, u);
    _mm_store_pd(out, y);
    out += 2;
  }
}

/*static void fetch_and_wrap_ring(wavemoth_plan plan, ring_pair_info_t info) {
  size_t mmax = plan->mmax;
  int *m_to_phase_ring = plan->m_to_phase_ring;

  for (size_t m = 0; m != mmax + 1; ++m) {
    m_to_thread[m]
  }
} 
*/

static INLINE m128d complex_mul_pd(m128d a_b, m128d c_d) {
  /* Multiply (a + I*b) and (c + I*d) */
  m128d a_a = _mm_unpacklo_pd(a_b, a_b);
  m128d b_b = _mm_unpackhi_pd(a_b, a_b);
  m128d minusb_b = _mm_mul_pd(b_b, (m128d){-1.0, 1.0});
  m128d d_c = _mm_shuffle_pd(c_d, c_d, _MM_SHUFFLE2(0, 1));

  return _mm_add_pd(_mm_mul_pd(a_a, c_d), _mm_mul_pd(minusb_b, d_c));
}

static INLINE void inplace_add_pd(double *px, m128d r) {
  m128d x = _mm_load_pd(px);
  _mm_store_pd(px, _mm_add_pd(x, r));
}

static void _printreg(char *msg, m128d r) {
  double *pd = (double*)&r;
  printf("%s = [%.2f %.2f]\n", msg, pd[0], pd[1]);
}
#define printreg(x) _printreg(#x, x)

static void perform_backward_ffts_thread(wavemoth_plan plan, int inode, int icpu,
                                         int ithread, void *ctx) {
  /*
    a) Phase shift all coefficients according to their phi0
    b) Zero padding and wrap-around coefficients (contribution from +/- m)
    c) Fourier transforms
  */
  assert(ithread == 0);
  int nmaps = plan->nmaps;
  size_t mid_ring = plan->grid->mid_ring;
  size_t nrings_half = mid_ring + 1;
  int mmax = plan->mmax;
  size_t npix = plan->grid->npix;
  size_t n;
  int m;
  int imap, iring, ipix;
  double *output = plan->output;
  bfm_index_t *ring_offsets = plan->grid->ring_offsets;
  double *cos_and_sin = memalign(16, sizeof(double[2 * plan->mmax]));
  double phi0;
  double *map, *ring;

  wavemoth_cpu_plan_t *cpu_plan = &plan->node_plans[inode]->cpu_plans[icpu];
  ring_pair_info_t *ring_pairs = cpu_plan->ring_pairs;

  double **m_to_phase_ring = plan->m_to_phase_ring;

  size_t idx, offset, length;

  double *work = cpu_plan->work_fft;
  size_t work_stride = nmaps * (4 * plan->Nside + 2);
  
  m128d conjugating_const = (m128d){ 1.0, -1.0 };

  /* This assertion holds true currently simply because of how
     work is distributed during plan initialization. */
  assert((cpu_plan->nrings) % FFT_CHUNK_SIZE == 0);

  int s;
  for (size_t chunk_start = 0;
       chunk_start < cpu_plan->nrings;
       chunk_start += FFT_CHUNK_SIZE) {

    memset(work, 0, sizeof(double[2 * FFT_CHUNK_SIZE * work_stride]));
    for (size_t m = 0; m != mmax + 1; ++m) {
      double *q_m_even_array = m_to_phase_ring[m];
      double *q_m_odd_array = q_m_even_array + 2 * nmaps * nrings_half;

      for (size_t j = 0; j != FFT_CHUNK_SIZE; ++j) {
        double *work_top = work + 2 * j * work_stride;
        double *work_bottom = work + (2 * j + 1) * work_stride;
        /* Note: iring is index into task list, NOT the physical ring
           number (which is ri->ring_number) */
        size_t iring = chunk_start + j;
        ring_pair_info_t *ri = &ring_pairs[iring];

        double cos_phi = cos(m * ri->phi0);
        double sin_phi = sin(m * ri->phi0);
        m128d phase_shift = (m128d){cos_phi, sin_phi};

        size_t ring_number = ri->ring_number;
        int ringlen = ri->length;
        int j1, j2;
        j1 = m % ringlen;
        j2 = imod_divisorsign(ringlen - m, ringlen);

        for (size_t k = 0; k != nmaps; ++k) {
          m128d q_even, q_odd, q_top_1, q_bottom_1, q_top_2, q_bottom_2;
          q_even = _mm_load_pd(q_m_even_array + 2 * (ring_number * nmaps + k));
          q_odd = _mm_load_pd(q_m_odd_array + 2 * (ring_number * nmaps + k));
          q_top_1 = _mm_add_pd(q_even, q_odd);
          q_bottom_1 = _mm_sub_pd(q_even, q_odd);

          q_top_1 = complex_mul_pd(q_top_1, phase_shift);
          q_bottom_1 = complex_mul_pd(q_bottom_1, phase_shift);

          q_top_2 = _mm_mul_pd(q_top_1, conjugating_const);
          q_bottom_2 = _mm_mul_pd(q_bottom_1, conjugating_const);

          if (j1 <= ringlen / 2) {
            inplace_add_pd(work_top + 2 * (j1 * nmaps + k), q_top_1);
            inplace_add_pd(work_bottom + 2 * (j1 * nmaps + k), q_bottom_1);
          }
          if (m != 0 && j2 <= ringlen / 2) {
            inplace_add_pd(work_top + 2 * (j2 * nmaps + k), q_top_2);
            inplace_add_pd(work_bottom + 2 * (j2 * nmaps + k), q_bottom_2);
          }
        }
      }
    }
    
    for (size_t j = 0; j != FFT_CHUNK_SIZE; ++j) {
      size_t iring = chunk_start + j;

      ring_pair_info_t *ri = &ring_pairs[chunk_start + j];
      double *work_top = work + 2 * j * work_stride;
      double *work_bottom = work + (2 * j + 1) * work_stride;
      fftw_plan fft_plan = ring_pairs[iring].fft_plan;
      fftw_execute_dft_c2r(fft_plan, (fftw_complex*)work_top, work_top);
      memcpy(output + nmaps * ri->offset_top, work_top,
             sizeof(double[ri->length * nmaps]));
      if (ri->offset_bottom != ri->offset_top) {
        fftw_execute_dft_c2r(fft_plan, (fftw_complex*)work_bottom, work_bottom);
        memcpy(output + nmaps * ri->offset_bottom, work_bottom,
               sizeof(double[ri->length * nmaps]));
      }
    }
  }
}

void wavemoth_perform_backward_ffts(wavemoth_plan plan) {
  wavemoth_run_in_threads(plan, &perform_backward_ffts_thread, 1, NULL, NULL, NULL);
}



wavemoth_grid_info* wavemoth_create_healpix_grid_info(int Nside) {
  int iring, ring_npix, ipix;
  int nrings = 4 * Nside - 1;
  /* Allocate all memory for the ring info in a single blob and just
     set up internal pointers. */
  char *buf = (char*)malloc(sizeof(wavemoth_grid_info) + sizeof(double[nrings]) +
                            sizeof(bfm_index_t[nrings + 1]));
  wavemoth_grid_info *result = (wavemoth_grid_info*)buf;
  buf += sizeof(wavemoth_grid_info);
  result->phi0s = (double*)buf;
  buf += sizeof(double[nrings]);
  result->ring_offsets = (bfm_index_t*)buf;
  result->has_equator = 1;
  result->nrings = nrings;
  result->mid_ring = 2 * Nside - 1;
  ring_npix = 0;
  ipix = 0;
  for (iring = 0; iring != nrings; ++iring) {
    if (iring <= Nside - 1) {
      ring_npix += 4;
      result->phi0s[iring] = PI / (4.0 * (iring + 1));
    } else if (iring > 3 * Nside - 1) {
      ring_npix -= 4;
      result->phi0s[iring] = PI / (4.0 * (nrings - iring));
    } else {
      result->phi0s[iring] = (PI / (4.0 * Nside)) * (iring  % 2);
    }
    result->ring_offsets[iring] = ipix;
    ipix += ring_npix;
  }
  result->ring_offsets[nrings] = ipix;
  result->npix = ipix;
  return result;
}

void wavemoth_free_grid_info(wavemoth_grid_info *info) {
  /* In the constructor we allocate the internal arrays as part of the
     same blob. */
  free((char*)info);
}

void wavemoth_disable_phase_shifting(wavemoth_plan plan) {
  int iring;
  /* Used for debug purposes.
     TODO: If the grid starts to get shared across plans we need to think
     of something else here. */
  for (iring = 0; iring != plan->grid->nrings; ++iring) {
    plan->grid->phi0s[iring] = 0;
  }
}

static void wait_for_execute_thread(wavemoth_plan plan, int inode, int icpu,
                                    int ithread, void *ctx) {
  /* First wait for the barrier during plan creation, so that the
     thread is created before planning returns. */
  pthread_barrier_wait(&plan->execute_barrier);
  /* Then, enter loop waiting for executions */
  while (1) {
    pthread_barrier_wait(&plan->execute_barrier);
    if (plan->destructing) return;

    legendre_transforms_thread(plan, inode, icpu, ithread, ctx);

    pthread_barrier_wait(&plan->execute_barrier);

    perform_backward_ffts_thread(plan, inode, icpu, ithread, ctx);

    pthread_barrier_wait(&plan->execute_barrier);
  }
  
}

void wavemoth_execute(wavemoth_plan plan) {
  /* Reset queue head for all nodes */
  for (int inode = 0; inode != plan->nnodes; ++inode) {
    plan->node_plans[inode]->im = 0;
  }

  plan->times.legendre_transform_start = walltime();
  pthread_barrier_wait(&plan->execute_barrier);
  pthread_barrier_wait(&plan->execute_barrier);
  plan->times.legendre_transform_done = walltime();
  pthread_barrier_wait(&plan->execute_barrier);
  plan->times.fft_done = walltime();
}

