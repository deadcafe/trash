/*
 * function queue
 */

#include <sys/types.h>
#include <sys/eventfd.h>

#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <trema.h>

#include "memory_barrier.h"
#include "func_queue.h"

//#define ENABLE_TRACE
#include "private_log.h"

#define	WMB()	write_memory_barrier()
#define	RMB()	read_memory_barrier()

#define	MALLOC(s)	xmalloc(s)
#define	FREE(p)		xfree(p)


enum {
  STATE_INVALID = 0,
  STATE_READY,
};

typedef struct {
  void *arg;
  void (*cb)(void *);
} func_q_msg_t;



static inline void
CLOSE(int fd)
{
  int err;

  do {
    err = 0;
    if (close(fd) < 0)
      err = errno;
  } while (err == EINTR);
}

/*
 * wake up
 */
static bool
wakeup(func_q_info_t *info)
{
  int err = 0;
  TRACE("knocking another thread. iam:%x", (unsigned)pthread_self());

  while (!err && info->val) {

    err = 0;
    if (eventfd_write(info->fd, info->val) < 0) {
      err = errno;
      if (err == EINTR) {
        err = 0;
      } else if (err == EAGAIN) {
        break;
      } else {
        char buf[128];
        strerror_r(err, buf, sizeof(buf));
        ERROR("failed eventfd_write() %s", buf);
        return false;
      }
    } else {
      info->val = 0;
    }
  }

  if (info->val) {
    if (info->another->queue) {
      if (info->eh_type == EH_TYPE_GENERIC) {
        set_writable(info->fd, true);
      } else {
        set_writable_safe(info->fd, true);
      }
    }
  }
  return true;
}


static void
read_cb(int fd,
        void *arg)
{
  func_q_info_t *info = arg;
  func_q_msg_t *msg;
  TRACE("fd:%d info:%p", fd, info);
  assert(fd == info->fd);
  assert(info->bind_th == pthread_self());

  while (1) {
    eventfd_t val;

    if (eventfd_read(info->fd, &val) < 0) {
      if (errno != EINTR)
        break;
    }
  }

  while (info->queue && (msg = dequeue(info->queue)) != NULL) {
    msg->cb(msg->arg);
    FREE(msg);
  }
}


static void
write_cb(int fd,
         void *arg)
{
  func_q_info_t *info = arg;
  assert(fd == info->fd);
  wakeup(info);
}


static void
null_cb(int fd,
        void *arg)
{
  ERROR("catch null event fd:%d arg:%p", fd, arg);
}


bool
func_q_request( func_q_t *func_q,
                func_q_dir_t dir,
                void (*cb)( void * ),
                void *arg )
{
  DEBUG("Q:%p dir:%d cb:%p arg:%p", func_q, dir, cb, arg);
  assert(cb);
  assert(dir == DIR_TO_UP || dir == DIR_TO_DOWN);
  func_q_msg_t *msg;
  func_q_info_t *info = &func_q->info[dir];

  if (info->queue && (msg = MALLOC(sizeof(*msg))) != NULL) {
    msg->cb = cb;
    msg->arg = arg;

    enqueue( info->queue, msg );
    info->val++;

    return wakeup(info);
  }
  return false;
}


bool
func_q_bind( func_q_t *func_q,
             func_q_dir_t dir,
             func_q_ehtype_t eh_type)
{
  assert(dir == DIR_TO_UP || dir == DIR_TO_DOWN);
  assert(eh_type == EH_TYPE_GENERIC || eh_type == EH_TYPE_SAFE);
  func_q_info_t *info = &func_q->info[dir];
  info->eh_type = eh_type;

  DEBUG("q:%p dir:%d type:%d", func_q, dir, eh_type);
  assert(info->queue == NULL);
  assert(info->fd >= 0);
  assert(info->another && info->another->fd >= 0);
  assert(info->bind_th == 0);

  queue *queue = create_queue();
  if (queue) {

    if (info->eh_type == EH_TYPE_GENERIC) {
      set_fd_handler(info->fd, read_cb, info, null_cb, NULL);
      set_readable(info->fd, true);

      set_fd_handler(info->another->fd, null_cb, NULL, write_cb, info->another);
    } else {
      set_fd_handler_safe(info->fd, read_cb, info, null_cb, NULL);
      set_readable_safe(info->fd, true);

      set_fd_handler_safe(info->another->fd, null_cb, NULL, write_cb, info->another);
    }
    info->bind_th = pthread_self();
    info->queue = queue;
    WMB();
    return true;
  }
  return false;
}


void
func_q_unbind(func_q_t *func_q,
              func_q_dir_t dir)
{
  assert(dir == DIR_TO_UP || dir == DIR_TO_DOWN);

  func_q_info_t *info = (&func_q->info[dir]);
  int fd = info->fd;
  int another_fd = info->another->fd;
  queue *queue = info->queue;

  RMB();
  info->queue = NULL;
  info->val = 0;
  WMB();

  if (queue) {
    func_q_msg_t *msg;

    while ((msg = dequeue(queue)) != NULL) {
      msg->cb(msg->arg);
      FREE(msg);
    }
    delete_queue(queue);
  }

  assert(info->bind_th == pthread_self());
  info->bind_th = 0;

  if (info->eh_type == EH_TYPE_GENERIC) {
    set_readable(fd, false);
    delete_fd_handler(fd);

    set_writable(another_fd, false);
    delete_fd_handler(another_fd);
  } else {
    set_readable_safe(fd, false);
    delete_fd_handler_safe(fd);

    set_writable_safe(another_fd, false);
    delete_fd_handler_safe(another_fd);
  }

  DEBUG("q:%p dir:%d", func_q, dir);
}


/*****************************************************************
 * main thread (client side) functions
 *****************************************************************/
void
func_q_destroy( func_q_t *func_q )
{
  int i;
  DEBUG("Q:%p", func_q);

  for (i = 0; i < DIR_NUM; i++) {
    assert(func_q->info[i].queue == NULL);
    if (func_q->info[i].fd >= 0) {
      CLOSE(func_q->info[i].fd);
      func_q->info[i].fd = -1;
    }
  }
  FREE(func_q);
}


func_q_t *
func_q_create( void )
{
  func_q_t *func_q = MALLOC(sizeof(*func_q));
  if (func_q) {
    int fd;

    memset(func_q, 0, sizeof(*func_q));
    func_q->info[DIR_TO_UP].fd = -1;
    func_q->info[DIR_TO_DOWN].fd = -1;
    func_q->info[DIR_TO_UP].another = &func_q->info[DIR_TO_DOWN];
    func_q->info[DIR_TO_DOWN].another = &func_q->info[DIR_TO_UP];

    if ((fd = eventfd(0, EFD_NONBLOCK)) < 0) {
      func_q_destroy(func_q);
      func_q = NULL;
      goto end;
    }
    func_q->info[DIR_TO_UP].fd = fd;

    if ((fd = eventfd(0, EFD_NONBLOCK)) < 0) {
      func_q_destroy(func_q);
      func_q = NULL;
      goto end;
    }
    func_q->info[DIR_TO_DOWN].fd = fd;
  }
 end:
  DEBUG("Q:%p", func_q);
  return func_q;
}


