/* SPDX-License-Identifier: BSD-2-Clause */
#define _POSIX_C_SOURCE 200809L
#include "mock_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ef_mock_driver_state {
  ef_driver_handle fd;
  unsigned children;
  struct ef_mock_driver_state *next;
};

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ef_mock_driver_state *drivers;
static struct ef_mock_memreg_state *registrations;
static ef_addr next_dma_addr = UINT64_C(0x100000000);
static unsigned next_pd_id = 1;

static struct ef_mock_driver_state *find_driver(int fd)
{
  for (struct ef_mock_driver_state *p = drivers; p; p = p->next)
    if (p->fd == fd)
      return p;
  return NULL;
}

int ef_driver_open(ef_driver_handle *out)
{
  if (!out) return -EINVAL;
  int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
  if (fd < 0) return -errno;
  struct ef_mock_driver_state *p = calloc(1, sizeof(*p));
  if (!p) { close(fd); return -ENOMEM; }
  p->fd = fd;
  pthread_mutex_lock(&registry_lock);
  p->next = drivers;
  drivers = p;
  pthread_mutex_unlock(&registry_lock);
  *out = fd;
  return 0;
}

int ef_driver_close(ef_driver_handle handle)
{
  pthread_mutex_lock(&registry_lock);
  struct ef_mock_driver_state **pp = &drivers;
  while (*pp && (*pp)->fd != handle) pp = &(*pp)->next;
  if (!*pp) { pthread_mutex_unlock(&registry_lock); return -EBADF; }
  if ((*pp)->children) { pthread_mutex_unlock(&registry_lock); return -EBUSY; }
  struct ef_mock_driver_state *p = *pp;
  *pp = p->next;
  pthread_mutex_unlock(&registry_lock);
  int rc = close(handle);
  free(p);
  return rc == 0 ? 0 : -errno;
}

int ef_pd_alloc(ef_pd *pd, ef_driver_handle dh, int ifindex,
                enum ef_pd_flags flags)
{
  if (!pd || pd->mock_private || ifindex < 0) return -EINVAL;
  if (flags & ~(EF_PD_MCAST_LOOP)) return -EOPNOTSUPP;
  char name[IF_NAMESIZE];
  if (ifindex && !if_indextoname((unsigned) ifindex, name)) return -ENODEV;
  if (!ifindex) strcpy(name, "any");
  struct ef_mock_pd_state *p = calloc(1, sizeof(*p));
  if (!p) return -ENOMEM;
  size_t n = strlen(name) + 1;
  char *name_copy = malloc(n);
  if (!name_copy) { free(p); return -ENOMEM; }
  memcpy(name_copy, name, n);
  pthread_mutex_lock(&registry_lock);
  struct ef_mock_driver_state *d = find_driver(dh);
  if (!d) { pthread_mutex_unlock(&registry_lock); free(name_copy); free(p); return -EBADF; }
  ++d->children;
  p->owner = dh;
  p->driver = d;
  p->ifindex = (unsigned) ifindex;
  pd->pd_resource_id = next_pd_id++;
  pthread_mutex_unlock(&registry_lock);
  pd->pd_flags = flags;
  pd->pd_intf_name = name_copy;
  pd->mock_private = p;
  return 0;
}

int ef_pd_free(ef_pd *pd, ef_driver_handle dh)
{
  if (!pd || !pd->mock_private) return -EINVAL;
  struct ef_mock_pd_state *p = pd->mock_private;
  pthread_mutex_lock(&registry_lock);
  if (p->owner != dh) { pthread_mutex_unlock(&registry_lock); return -EBADF; }
  if (p->children) { pthread_mutex_unlock(&registry_lock); return -EBUSY; }
  --p->driver->children;
  pthread_mutex_unlock(&registry_lock);
  free(pd->pd_intf_name);
  free(p);
  memset(pd, 0, sizeof(*pd));
  return 0;
}

int ef_memreg_alloc(ef_memreg *mr, ef_driver_handle mr_dh,
                    ef_pd *pd, ef_driver_handle pd_dh,
                    void *memory, size_t bytes)
{
  if (!mr || mr->mock_private || !pd || !pd->mock_private ||
      !memory || !bytes || ((uintptr_t)memory & EF_VI_NIC_PAGE_MASK))
    return -EINVAL;
  struct ef_mock_pd_state *p = pd->mock_private;
  if (mr_dh != pd_dh || p->owner != pd_dh) return -EBADF;
  if (bytes > SIZE_MAX - EF_VI_NIC_PAGE_MASK) return -EOVERFLOW;
  size_t pages = (bytes + EF_VI_NIC_PAGE_MASK) >> EF_VI_NIC_PAGE_SHIFT;
  if (pages > SIZE_MAX / sizeof(ef_addr) ||
      pages > UINT64_MAX / EF_VI_NIC_PAGE_SIZE) return -EOVERFLOW;
  ef_addr *dma = malloc(pages * sizeof(*dma));
  struct ef_mock_memreg_state *r = calloc(1, sizeof(*r));
  if (!dma || !r) { free(dma); free(r); return -ENOMEM; }
  pthread_mutex_lock(&registry_lock);
  if ((ef_addr)pages >
      (UINT64_MAX - next_dma_addr - EF_VI_NIC_PAGE_SIZE) / EF_VI_NIC_PAGE_SIZE) {
    pthread_mutex_unlock(&registry_lock); free(dma); free(r); return -EOVERFLOW;
  }
  r->owner = mr_dh;
  r->pd = pd;
  r->base = memory;
  r->bytes = bytes;
  r->address_base = next_dma_addr;
  r->driver = p->driver;
  for (size_t i = 0; i < pages; ++i)
    dma[i] = next_dma_addr + (ef_addr)i * EF_VI_NIC_PAGE_SIZE;
  next_dma_addr += (ef_addr)pages * EF_VI_NIC_PAGE_SIZE;
  /* Leave a guard page so DMA overruns never point into a later region. */
  next_dma_addr += EF_VI_NIC_PAGE_SIZE;
  ++p->children;
  ++r->driver->children;
  r->next = registrations;
  registrations = r;
  pthread_mutex_unlock(&registry_lock);
  mr->mr_dma_addrs = dma;
  mr->mr_dma_addrs_base = dma;
  mr->mock_private = r;
  return 0;
}

int ef_memreg_free(ef_memreg *mr, ef_driver_handle dh)
{
  if (!mr || !mr->mock_private) return -EINVAL;
  struct ef_mock_memreg_state *r = mr->mock_private;
  pthread_mutex_lock(&registry_lock);
  if (r->owner != dh) { pthread_mutex_unlock(&registry_lock); return -EBADF; }
  if (r->pending) { pthread_mutex_unlock(&registry_lock); return -EBUSY; }
  struct ef_mock_memreg_state **pp = &registrations;
  while (*pp && *pp != r) pp = &(*pp)->next;
  if (!*pp) { pthread_mutex_unlock(&registry_lock); return -EINVAL; }
  *pp = r->next;
  --((struct ef_mock_pd_state *)r->pd->mock_private)->children;
  --r->driver->children;
  pthread_mutex_unlock(&registry_lock);
  free(mr->mr_dma_addrs_base);
  free(r);
  memset(mr, 0, sizeof(*mr));
  return 0;
}

int ef_mock_pd_acquire(ef_pd *pd, ef_driver_handle dh)
{
  if (!pd || !pd->mock_private) return -EINVAL;
  pthread_mutex_lock(&registry_lock);
  struct ef_mock_pd_state *p = pd->mock_private;
  if (p->owner != dh) { pthread_mutex_unlock(&registry_lock); return -EBADF; }
  ++p->children;
  ++p->driver->children;
  pthread_mutex_unlock(&registry_lock);
  return 0;
}

void ef_mock_pd_release(ef_pd *pd)
{
  pthread_mutex_lock(&registry_lock);
  struct ef_mock_pd_state *p = pd->mock_private;
  --p->children;
  --p->driver->children;
  pthread_mutex_unlock(&registry_lock);
}

int ef_mock_lookup_and_hold(ef_pd *pd, ef_addr addr, size_t size,
                            struct ef_mock_memreg_state **reg, uint8_t **ptr)
{
  if (!pd || !reg || !ptr || !size) return -EINVAL;
  pthread_mutex_lock(&registry_lock);
  for (struct ef_mock_memreg_state *r = registrations; r; r = r->next) {
    if (r->pd == pd && addr >= r->address_base) {
      ef_addr off = addr - r->address_base;
      if (off <= r->bytes && size <= r->bytes - (size_t)off) {
        ++r->pending;
        *reg = r;
        *ptr = r->base + (size_t)off;
        pthread_mutex_unlock(&registry_lock);
        return 0;
      }
    }
  }
  pthread_mutex_unlock(&registry_lock);
  return -EFAULT;
}

void ef_mock_release_registration(struct ef_mock_memreg_state *r)
{
  pthread_mutex_lock(&registry_lock);
  --r->pending;
  pthread_mutex_unlock(&registry_lock);
}
