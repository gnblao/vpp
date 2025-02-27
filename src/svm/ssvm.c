/*
 * Copyright (c) 2015-2019 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <svm/ssvm.h>
#include <svm/svm_common.h>

typedef int (*init_fn) (ssvm_private_t *);
typedef void (*delete_fn) (ssvm_private_t *);

static init_fn server_init_fns[SSVM_N_SEGMENT_TYPES] =
  { ssvm_server_init_shm, ssvm_server_init_memfd, ssvm_server_init_private };
static init_fn client_init_fns[SSVM_N_SEGMENT_TYPES] =
  { ssvm_client_init_shm, ssvm_client_init_memfd, ssvm_client_init_private };
static delete_fn delete_fns[SSVM_N_SEGMENT_TYPES] =
  { ssvm_delete_shm, ssvm_delete_memfd, ssvm_delete_private };

int
ssvm_server_init_shm (ssvm_private_t * ssvm)
{
  int ssvm_fd;
  u8 junk = 0, *ssvm_filename;
  ssvm_shared_header_t *sh;
  uword page_size, requested_va = 0;
  void *oldheap;

  if (ssvm->ssvm_size == 0)
    return SSVM_API_ERROR_NO_SIZE;

  if (CLIB_DEBUG > 1)
    clib_warning ("[%d] creating segment '%s'", getpid (), ssvm->name);

  ASSERT (vec_c_string_is_terminated (ssvm->name));
  ssvm_filename = format (0, "/dev/shm/%s%c", ssvm->name, 0);
  unlink ((char *) ssvm_filename);
  vec_free (ssvm_filename);

  ssvm_fd = shm_open ((char *) ssvm->name, O_RDWR | O_CREAT | O_EXCL, 0777);
  if (ssvm_fd < 0)
    {
      clib_unix_warning ("create segment '%s'", ssvm->name);
      return SSVM_API_ERROR_CREATE_FAILURE;
    }

  if (fchmod (ssvm_fd, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) < 0)
    clib_unix_warning ("ssvm segment chmod");
  if (svm_get_root_rp ())
    {
      /* TODO: is this really needed? */
      svm_main_region_t *smr = svm_get_root_rp ()->data_base;
      if (fchown (ssvm_fd, smr->uid, smr->gid) < 0)
	clib_unix_warning ("ssvm segment chown");
    }

  if (lseek (ssvm_fd, ssvm->ssvm_size, SEEK_SET) < 0)
    {
      clib_unix_warning ("lseek");
      close (ssvm_fd);
      return SSVM_API_ERROR_SET_SIZE;
    }

  if (write (ssvm_fd, &junk, 1) != 1)
    {
      clib_unix_warning ("set ssvm size");
      close (ssvm_fd);
      return SSVM_API_ERROR_SET_SIZE;
    }

  page_size = clib_mem_get_fd_page_size (ssvm_fd);
  if (ssvm->requested_va)
    {
      requested_va = ssvm->requested_va;
      clib_mem_vm_randomize_va (&requested_va, min_log2 (page_size));
    }

  sh = clib_mem_vm_map_shared (uword_to_pointer (requested_va, void *),
			       ssvm->ssvm_size, ssvm_fd, 0,
			       (char *) ssvm->name);
  if (sh == CLIB_MEM_VM_MAP_FAILED)
    {
      clib_unix_warning ("mmap");
      close (ssvm_fd);
      return SSVM_API_ERROR_MMAP;
    }

  close (ssvm_fd);

  clib_mem_unpoison (sh, sizeof (*sh));
  sh->server_pid = ssvm->my_pid;
  sh->ssvm_size = ssvm->ssvm_size;
  sh->ssvm_va = pointer_to_uword (sh);
  sh->type = SSVM_SEGMENT_SHM;
  sh->heap = clib_mem_create_heap (((u8 *) sh) + page_size,
				   ssvm->ssvm_size - page_size,
				   1 /* locked */ , "ssvm server shm");

  oldheap = ssvm_push_heap (sh);
  sh->name = format (0, "%s", ssvm->name, 0);
  ssvm_pop_heap (oldheap);

  ssvm->sh = sh;
  ssvm->my_pid = getpid ();
  ssvm->is_server = 1;

  /* The application has to set set sh->ready... */
  return 0;
}

int
ssvm_client_init_shm (ssvm_private_t * ssvm)
{
  struct stat stat;
  int ssvm_fd = -1;
  ssvm_shared_header_t *sh;

  ASSERT (vec_c_string_is_terminated (ssvm->name));
  ssvm->is_server = 0;

  while (ssvm->attach_timeout-- > 0)
    {
      if (ssvm_fd < 0)
	ssvm_fd = shm_open ((char *) ssvm->name, O_RDWR, 0777);
      if (ssvm_fd < 0)
	{
	  sleep (1);
	  continue;
	}
      if (fstat (ssvm_fd, &stat) < 0)
	{
	  sleep (1);
	  continue;
	}

      if (stat.st_size > 0)
	goto map_it;
    }
  clib_warning ("client timeout");
  return SSVM_API_ERROR_CLIENT_TIMEOUT;

map_it:
  sh = (void *) mmap (0, MMAP_PAGESIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		      ssvm_fd, 0);
  if (sh == MAP_FAILED)
    {
      clib_unix_warning ("client research mmap");
      close (ssvm_fd);
      return SSVM_API_ERROR_MMAP;
    }

  while (ssvm->attach_timeout-- > 0)
    {
      if (sh->ready)
	goto re_map_it;
    }
  close (ssvm_fd);
  munmap (sh, MMAP_PAGESIZE);
  clib_warning ("client timeout 2");
  return SSVM_API_ERROR_CLIENT_TIMEOUT;

re_map_it:
  ssvm->requested_va = sh->ssvm_va;
  ssvm->ssvm_size = sh->ssvm_size;
  munmap (sh, MMAP_PAGESIZE);

  sh = ssvm->sh = (void *) mmap ((void *) ssvm->requested_va, ssvm->ssvm_size,
				 PROT_READ | PROT_WRITE,
				 MAP_SHARED | MAP_FIXED, ssvm_fd, 0);

  if (sh == MAP_FAILED)
    {
      clib_unix_warning ("client final mmap");
      close (ssvm_fd);
      return SSVM_API_ERROR_MMAP;
    }
  sh->client_pid = getpid ();
  close (ssvm_fd);
  return 0;
}

void
ssvm_delete_shm (ssvm_private_t * ssvm)
{
  u8 *fn;

  fn = format (0, "/dev/shm/%s%c", ssvm->name, 0);

  if (CLIB_DEBUG > 1)
    clib_warning ("[%d] unlinking ssvm (%s) backing file '%s'", getpid (),
		  ssvm->name, fn);

  /* Throw away the backing file */
  if (unlink ((char *) fn) < 0)
    clib_unix_warning ("unlink segment '%s'", ssvm->name);

  vec_free (fn);
  vec_free (ssvm->name);

  if (ssvm->is_server)
    clib_mem_vm_unmap (ssvm->sh);
  else
    munmap ((void *) ssvm->sh, ssvm->ssvm_size);
}

/**
 * Initialize memfd segment server
 */
int
ssvm_server_init_memfd (ssvm_private_t * memfd)
{
  uword page_size, n_pages;
  ssvm_shared_header_t *sh;
  int log2_page_size;
  void *oldheap;

  if (memfd->ssvm_size == 0)
    return SSVM_API_ERROR_NO_SIZE;

  ASSERT (vec_c_string_is_terminated (memfd->name));

  if (memfd->huge_page)
    memfd->fd = clib_mem_vm_create_fd (CLIB_MEM_PAGE_SZ_DEFAULT_HUGE,
				       (char *) memfd->name);
  else
    memfd->fd =
      clib_mem_vm_create_fd (CLIB_MEM_PAGE_SZ_DEFAULT, (char *) memfd->name);

  if (memfd->fd == CLIB_MEM_ERROR)
    {
      clib_unix_warning ("failed to create memfd");
      return SSVM_API_ERROR_CREATE_FAILURE;
    }

  log2_page_size = clib_mem_get_fd_log2_page_size (memfd->fd);
  if (log2_page_size == 0)
    {
      clib_unix_warning ("cannot determine page size");
      return SSVM_API_ERROR_CREATE_FAILURE;
    }

  n_pages = ((memfd->ssvm_size - 1) >> log2_page_size) + 1;

  if ((ftruncate (memfd->fd, n_pages << log2_page_size)) == -1)
    {
      clib_unix_warning ("memfd ftruncate failure");
      return SSVM_API_ERROR_CREATE_FAILURE;
    }

  sh = clib_mem_vm_map_shared (uword_to_pointer (memfd->requested_va, void *),
			       memfd->ssvm_size, memfd->fd, 0,
			       (char *) memfd->name);
  if (sh == CLIB_MEM_VM_MAP_FAILED)
    {
      clib_unix_warning ("memfd map (fd %d)", memfd->fd);
      close (memfd->fd);
      return SSVM_API_ERROR_CREATE_FAILURE;
    }

  memfd->sh = sh;
  memfd->my_pid = getpid ();
  memfd->is_server = 1;

  sh->server_pid = memfd->my_pid;
  sh->ssvm_size = memfd->ssvm_size;
  sh->ssvm_va = pointer_to_uword (sh);
  sh->type = SSVM_SEGMENT_MEMFD;

  page_size = clib_mem_get_page_size ();
  sh->heap = clib_mem_create_heap (((u8 *) sh) + page_size,
				   memfd->ssvm_size - page_size,
				   1 /* locked */ , "ssvm server memfd");
  oldheap = ssvm_push_heap (sh);
  sh->name = format (0, "%s", memfd->name, 0);
  ssvm_pop_heap (oldheap);

  /* The application has to set set sh->ready... */
  return 0;
}

/**
 * Initialize memfd segment client
 *
 * Subtly different than svm_client_init. The caller needs to acquire
 * a usable file descriptor for the memfd segment e.g. via
 * vppinfra/socket.c:default_socket_recvmsg
 */
int ssvm_client_init_buffers_memfd (ssvm_private_t * memfd)
{
  int mmap_flags = MAP_SHARED;

  memfd->ssvm_size = clib_mem_get_fd_size (memfd->fd);
  if (!memfd->ssvm_size)
    {
      clib_unix_warning ("page size unknown");
      return SSVM_API_ERROR_MMAP;
    }

  memfd->log2_page_size = clib_mem_get_fd_log2_page_size (memfd->fd);
  if (!memfd->log2_page_size)
    {
      clib_unix_warning ("page size unknown");
      return SSVM_API_ERROR_MMAP;
    }

  if (memfd->requested_va)
    mmap_flags |= MAP_FIXED;

  /*
   * Remap the segment at the 'right' address
   */
  memfd->sh = (void *) mmap (uword_to_pointer (memfd->requested_va, void *),
		      memfd->ssvm_size,
		      PROT_READ | PROT_WRITE, mmap_flags, memfd->fd, 0);

  if (memfd->sh == MAP_FAILED)
    {
      clib_unix_warning ("client final mmap");
      close (memfd->fd);
      return SSVM_API_ERROR_MMAP;
    }

  return 0;
}


/**
 * Initialize memfd segment client
 *
 * Subtly different than svm_client_init. The caller needs to acquire
 * a usable file descriptor for the memfd segment e.g. via
 * vppinfra/socket.c:default_socket_recvmsg
 */
int
ssvm_client_init_memfd (ssvm_private_t * memfd)
{
  int mmap_flags = MAP_SHARED;
  ssvm_shared_header_t *sh;
  uword page_size;

  memfd->is_server = 0;

  page_size = clib_mem_get_fd_page_size (memfd->fd);
  if (!page_size)
    {
      clib_unix_warning ("page size unknown");
      return SSVM_API_ERROR_MMAP;
    }

  /*
   * Map the segment once, to look at the shared header
   */
  sh = (void *) mmap (0, page_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      memfd->fd, 0);

  if (sh == MAP_FAILED)
    {
      clib_unix_warning ("client research mmap (fd %d)", memfd->fd);
      close (memfd->fd);
      return SSVM_API_ERROR_MMAP;
    }

  memfd->requested_va = sh->ssvm_va;
  memfd->ssvm_size = sh->ssvm_size;
  munmap (sh, page_size);

  if (memfd->requested_va)
    mmap_flags |= MAP_FIXED;

  /*
   * Remap the segment at the 'right' address
   */
  sh = (void *) mmap (uword_to_pointer (memfd->requested_va, void *),
		      memfd->ssvm_size,
		      PROT_READ | PROT_WRITE, mmap_flags, memfd->fd, 0);

  if (sh == MAP_FAILED)
    {
      clib_unix_warning ("client final mmap");
      close (memfd->fd);
      return SSVM_API_ERROR_MMAP;
    }

  sh->client_pid = getpid ();
  memfd->sh = sh;
  return 0;
}

void
ssvm_delete_memfd (ssvm_private_t * memfd)
{
  vec_free (memfd->name);
  if (memfd->is_server)
    clib_mem_vm_unmap (memfd->sh);
  else
    munmap (memfd->sh, memfd->ssvm_size);
  close (memfd->fd);
}

/**
 * Initialize segment in a private heap
 */
int
ssvm_server_init_private (ssvm_private_t * ssvm)
{
  uword page_size, log2_page_size, rnd_size = 0;
  ssvm_shared_header_t *sh;
  clib_mem_heap_t *heap, *oldheap;

  log2_page_size = clib_mem_get_log2_page_size ();
  if (log2_page_size == 0)
    {
      clib_unix_warning ("cannot determine page size");
      return SSVM_API_ERROR_CREATE_FAILURE;
    }

  page_size = 1ULL << log2_page_size;
  rnd_size = clib_max (ssvm->ssvm_size + (page_size - 1), ssvm->ssvm_size);
  rnd_size &= ~(page_size - 1);

  sh = clib_mem_vm_map (0, rnd_size + page_size, log2_page_size,
			(char *) ssvm->name);
  if (sh == CLIB_MEM_VM_MAP_FAILED)
    {
      clib_unix_warning ("private map failed");
      return SSVM_API_ERROR_CREATE_FAILURE;
    }

  heap = clib_mem_create_heap ((u8 *) sh + page_size, rnd_size,
			       1 /* locked */ , "ssvm server private");
  if (heap == 0)
    {
      clib_unix_warning ("heap alloc");
      return -1;
    }

  rnd_size = clib_mem_get_heap_free_space (heap);

  ssvm->ssvm_size = rnd_size;
  ssvm->is_server = 1;
  ssvm->my_pid = getpid ();
  ssvm->requested_va = ~0;

  /* First page in allocated memory is set aside for the shared header */
  ssvm->sh = sh;

  clib_memset (sh, 0, sizeof (*sh));
  sh->heap = heap;
  sh->ssvm_size = rnd_size;
  sh->ssvm_va = pointer_to_uword (sh);
  sh->type = SSVM_SEGMENT_PRIVATE;

  oldheap = ssvm_push_heap (sh);
  sh->name = format (0, "%s", ssvm->name, 0);
  ssvm_pop_heap (oldheap);

  return 0;
}

int
ssvm_client_init_private (ssvm_private_t * ssvm)
{
  clib_warning ("BUG: this should not be called!");
  return -1;
}

void
ssvm_delete_private (ssvm_private_t * ssvm)
{
  vec_free (ssvm->name);
  clib_mem_destroy_heap (ssvm->sh->heap);
  clib_mem_vm_unmap (ssvm->sh);
}

int
ssvm_server_init (ssvm_private_t * ssvm, ssvm_segment_type_t type)
{
  return (server_init_fns[type]) (ssvm);
}

int
ssvm_client_init (ssvm_private_t * ssvm, ssvm_segment_type_t type)
{
  return (client_init_fns[type]) (ssvm);
}

void
ssvm_delete (ssvm_private_t * ssvm)
{
  delete_fns[ssvm->sh->type] (ssvm);
}

ssvm_segment_type_t
ssvm_type (const ssvm_private_t * ssvm)
{
  return ssvm->sh->type;
}

u8 *
ssvm_name (const ssvm_private_t * ssvm)
{
  return ssvm->sh->name;
}

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
