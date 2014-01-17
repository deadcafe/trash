/*
 * test bed
 */

#include <sys/time.h>
#include <sys/types.h>
#include <sys/tree.h>

#include <assert.h>
#include <errno.h>
#include <event.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "libevent_wrapper.h"

#define USE_REDBLACK 1
#define ENABLE_CAHCE 1

#if 0
# include "test/stub.h"
#else
# define TRACE(x,...)
#endif

#define UNUSED __attribute__((unused))

#define BASE_STATE_INVALID      0
#define BASE_STATE_RUNNING      1
#define BASE_STATE_STOP         2

#define CTX_STATE_INVALID       1
#define CTX_STATE_ADDED_DB      (1 << 1)
#define CTX_STATE_ADDED_EV      (1 << 2)

#ifdef USE_REDBLACK
# define TREE_HEAD              RB_HEAD
# define TREE_ENTRY             RB_ENTRY
# define TREE_ROOT              RB_ROOT
# define TREE_GENERATE_STATIC   RB_GENERATE_STATIC
# define TREE_FIND              RB_FIND
# define TREE_INSERT            RB_INSERT
# define TREE_REMOVE            RB_REMOVE
# define TREE_FOREACH_SAFE      RB_FOREACH_SAFE
#else
# define TREE_HEAD              SPLAY_HEAD
# define TREE_ENTRY             SPLAY_ENTRY
# define TREE_ROOT              SPLAY_ROOT
# define TREE_GENERATE_STATIC   SPLAY_GENERATE_STATIC
# define TREE_FIND              SPLAY_FIND
# define TREE_INSERT            SPLAY_INSERT
# define TREE_REMOVE            SPLAY_REMOVE
# define TREE_FOREACH_SAFE      SPLAY_FOREACH_SAFE
#endif

#ifdef ENABLE_CAHCE
# define CACHE_CLEAR(b)         clear_cache_ctx((b))
# define CACHE_STORE(b,c)       store_cache_ctx((b),(c))
# define CACHE_FIND(b,k)        find_ctx_cache((b),(k))
#else
# define CACHE_CLEAR(b)
# define CACHE_STORE(b,c)
# define CACHE_FIND(b,k)        find_ctx_cache((b),(k))
#endif

typedef struct _ctx_t ctx_t;

/* from linux kernel */
#define container_of(ptr, type, member) ({                      \
      typeof( ((type *)0)->member ) *__mptr = (ptr);            \
      (type *)( (char *)__mptr - offsetof(type,member) );       \
    })

typedef struct {
  int state;
  int padding;
  ssize_t refcnt;
  pthread_key_t *key;

  ctx_t *cache_ctx;

  struct event_base *base;
  struct event *tick;
  pthread_t thread;

    TREE_HEAD( ctx_db, _ctx_t ) head;
  external_callback_t ext_cb;
} ev_base_t;

typedef struct {
  TREE_ENTRY( _ctx_t ) node;
  ssize_t refcnt;

  int state;
  short flags;
  short padding;

  struct event *ev;
  ev_base_t *base;
} ctx_hd_t;

struct _ctx_t {
  ctx_hd_t hd;
  event_handle_t handle;
};

static pthread_key_t Key;       /* for thread safe */
static ev_base_t *Base;         /*for none thread safe */

static void *( *_malloc_fn ) ( size_t ) = NULL;
static void *( *_realloc_fn ) ( void *, size_t ) = NULL;
static void ( *_free_fn ) ( void * ) = NULL;
static suseconds_t tick = 0;

#define MALLOC(s) _malloc_fn((s))
#define REALLOC(p,s) _realloc_fn((p),(s))
#define FREE(p)  _free_fn((p))

static void clear_cache_ctx( ev_base_t *ev_base );
static void clear_db_all_ctx( ev_base_t *ev_base );

/*
 * heartbeat timer
 */
static void
tick_handler( int fd UNUSED, short flags UNUSED, void *arg UNUSED )
{
  ;
}

/**************************************************************************
 * base functions
 **************************************************************************/
static ev_base_t *
create_ev_base( pthread_key_t *key, suseconds_t usec )
{
  ev_base_t *ev_base;

  if ( ( ev_base = MALLOC( sizeof( *ev_base ) ) ) ) {
    memset( ev_base, 0, sizeof( *ev_base ) );
    if ( !( ev_base->base = event_base_new(  ) ) ) {
      FREE( ev_base );
      return NULL;
    }

    if ( usec ) {
      if ( !( ev_base->tick = evtimer_new( ev_base->base, tick_handler, NULL ) ) ) {
        event_base_free( ev_base->base );
        FREE( ev_base );
        return NULL;
      }
    }

    if ( key ) {
      if ( pthread_setspecific( *key, ev_base ) ) {
        if ( ev_base->tick )
          event_free( ev_base->tick );
        event_base_free( ev_base->base );
        FREE( ev_base );
        return NULL;
      }
      ev_base->key = key;
    }
    ev_base->refcnt = 1;
    ev_base->thread = pthread_self();
  }
  TRACE( "base:%p", ev_base );
  return ev_base;
}

static void
free_ev_base( ev_base_t *ev_base )
{
  if ( ev_base->tick ) {
    event_free( ev_base->tick );
    ev_base->tick = NULL;
  }
  if ( ev_base->base ) {
    event_base_free( ev_base->base );
    ev_base->base = NULL;
  }
  TRACE( "base:%p", ev_base );
  FREE( ev_base );
}

static void
attach_base( ev_base_t *ev_base )
{
  ev_base->refcnt++;
  TRACE( "base:%p cnt:%d", ev_base, ev_base->refcnt );
}

static void
detach_base( ev_base_t *ev_base )
{
  ev_base->refcnt--;
  TRACE( "base:%p cnt:%d", ev_base, ev_base->refcnt );
  if ( !ev_base->refcnt )
    free_ev_base( ev_base );
}

static void
destroy_ev_base( ev_base_t *ev_base )
{
  TRACE( "base:%p", ev_base );

  ev_base->state = BASE_STATE_INVALID;
  CACHE_CLEAR( ev_base );
  clear_db_all_ctx( ev_base );

  if ( ev_base->key ) {
    pthread_setspecific( *( ev_base->key ), NULL );
    ev_base->key = NULL;
  }
  detach_base( ev_base );
}

static void
base_destructor( void *arg )
{
  ev_base_t *ev_base = arg;

  if ( ev_base )
    destroy_ev_base( ev_base );
}

#define SAFE    true
#define UNSAFE  false

static inline ev_base_t *
get_ev_base( bool safe )
{
  ev_base_t *base = NULL;

  if ( safe )
    base = pthread_getspecific( Key );
  else
    base = Base;

  if (base->thread != pthread_self()) {
    assert(base->thread == pthread_self());
    return NULL;
  }

  assert( base );
  return base;
}

/**************************************************************************
 * ctx functions
 **************************************************************************/
static void handler_core( int fd, short flags, void *arg );

static inline int
cmp_handle( const event_handle_t *h0, const event_handle_t *h1 )
{
  int ret;

  ret = h0->type - h1->type;
  if ( ret )
    return ret;
  if ( h0->type == CTX_TYPE_FD )
    ret = h0->fd - h1->fd;
  else if ( h0->type == CTX_TYPE_TIMER ) {
    if ( h0->u.timer_val.timer_cb > h1->u.timer_val.timer_cb )
      ret = 1;
    else if ( h0->u.timer_val.timer_cb < h1->u.timer_val.timer_cb )
      ret = -1;
    else if ( h0->u.timer_val.timer_data > h1->u.timer_val.timer_data )
      ret = 1;
    else if ( h0->u.timer_val.timer_data < h1->u.timer_val.timer_data )
      ret = -1;
    else
      ret = 0;
  }
  else {                        /* signal */
    ret = h0->fd - h1->fd;
  }
  return ret;
}

static inline int
cmp_ctx( const ctx_t *c0, const ctx_t *c1 )
{
  return cmp_handle( &c0->handle, &c1->handle );
}

TREE_GENERATE_STATIC( ctx_db, _ctx_t, hd.node, cmp_ctx );

static inline ctx_t *
create_ctx( ev_base_t *ev_base, short type )
{
  ctx_t *ctx;

  if ( ( ctx = MALLOC( sizeof( *ctx ) ) ) ) {
    memset( ctx, 0, sizeof( *ctx ) );
    if ( ( ctx->hd.ev =
           event_new( ev_base->base, -1, 0, handler_core, ctx ) ) == NULL ) {
      FREE( ctx );
      return NULL;
    }
    ctx->hd.base = ev_base;
    attach_base( ev_base );

    ctx->hd.refcnt = 1;
    ctx->handle.type = type;
    ctx->handle.fd = -1;
    TRACE( "alloc ctx:%p", ctx );
  }
  return ctx;
}

static inline void
attach_ctx( ctx_t *ctx )
{
  ctx->hd.refcnt++;
}

static inline void
detach_ctx( ctx_t *ctx )
{
  ctx->hd.refcnt--;
  if ( !ctx->hd.refcnt ) {
    assert( ( ctx->hd.state & CTX_STATE_INVALID ) );

    TRACE( "free ctx:%p", ctx );
    if ( ctx->hd.ev ) {
      event_free( ctx->hd.ev );
      ctx->hd.ev = NULL;
    }

    if ( ctx->hd.base ) {
      detach_base( ctx->hd.base );
      ctx->hd.base = NULL;
    }
    ctx->handle.type = CTX_TYPE_INVALID;
    ctx->handle.fd = -1;
    FREE( ctx );
  }
}

static inline void
clear_cache_ctx( ev_base_t *ev_base )
{
  if ( ev_base->cache_ctx ) {
    detach_ctx( ev_base->cache_ctx );
    ev_base->cache_ctx = NULL;
  }
}

static inline void
store_cache_ctx( ev_base_t *ev_base, ctx_t *ctx )
{
  clear_cache_ctx( ev_base );
  ev_base->cache_ctx = ctx;
  attach_ctx( ctx );
}

static inline ctx_t *
find_ctx_cache( ev_base_t *ev_base, ctx_t *key )
{
#ifdef ENABLE_CACHE
  if ( ev_base->cache_ctx ) {
    if ( !cmp_ctx( key, ev_base->cache_ctx ) ) {
      TRACE( "cache hit ctx:%p type:%d fd:%d",
             ev_base->cache_ctx, ev_base->cache_ctx->handle.type,
             ev_base->cache_ctx->handle.fd );
      return ev_base->cache_ctx;
    }
  }
  TRACE( "cache mis" );
#endif /* ENABLE_CACHE */
  return TREE_FIND( ctx_db, &ev_base->head, key );
}

static inline ctx_t *
find_ctx_fd( ev_base_t *ev_base, int fd )
{
  ctx_t key;

  TRACE( "base:%p fd:%d", ev_base, fd );
  key.handle.type = CTX_TYPE_FD;
  key.handle.fd = fd;
  return CACHE_FIND( ev_base, &key );
}

static inline ctx_t *
find_ctx_tm( ev_base_t *ev_base, void *cb, void *arg )
{
  ctx_t key;

  TRACE( "base:%p cb:%p arg:%p", ev_base, cb, arg );
  key.handle.type = CTX_TYPE_TIMER;
  key.handle.fd = -1;
  key.handle.u.timer_val.timer_cb = cb;
  key.handle.u.timer_val.timer_data = arg;
  return CACHE_FIND( ev_base, &key );
}

static inline void
add_db_ctx( ctx_t *ctx )
{
  ctx_t *old;
  ev_base_t *ev_base = ctx->hd.base;

  assert( !( ctx->hd.state & CTX_STATE_ADDED_DB ) );
  ctx->hd.state |= CTX_STATE_ADDED_DB;
  old = TREE_INSERT( ctx_db, &ev_base->head, ctx );
  assert( !old );
  attach_ctx( ctx );
  TRACE( "fd:%d cnt:%d state:%x", ctx->handle.fd, ctx->hd.refcnt,
         ctx->hd.state );
}

static inline void
del_db_ctx( ctx_t *ctx )
{
  ev_base_t *ev_base = ctx->hd.base;

  if ( ctx->hd.state & CTX_STATE_ADDED_DB ) {
    ctx->hd.state &= ~CTX_STATE_ADDED_DB;
    TREE_REMOVE( ctx_db, &ev_base->head, ctx );
    TRACE( "fd:%d cnt:%d state:%x", ctx->handle.fd, ctx->hd.refcnt,
           ctx->hd.state );
    detach_ctx( ctx );
  }
}

static inline void
clear_db_timer_ctx( ev_base_t *ev_base )
{
  ctx_t *ctx, *tmp;

  TREE_FOREACH_SAFE( ctx, ctx_db, &ev_base->head, tmp ) {
    if ( ctx->handle.type != CTX_TYPE_TIMER )
      break;
    destroy_event_handle( &ctx->handle );
  }
}

static inline void
clear_db_all_ctx( ev_base_t *ev_base )
{
  ctx_t *ctx;

  TRACE( "base:%p", ev_base );
  while ( ( ctx = TREE_ROOT( &ev_base->head ) ) != NULL ) {
    destroy_event_handle( &ctx->handle );
  }
}

static inline void
del_ev_ctx( ctx_t *ctx )
{
  if ( ctx->hd.state & CTX_STATE_ADDED_EV ) {
    ctx->hd.state &= ~CTX_STATE_ADDED_EV;
    event_del( ctx->hd.ev );
    ctx->hd.flags = 0;
    TRACE( "fd:%d cnt:%d state:%x", ctx->handle.fd, ctx->hd.refcnt,
           ctx->hd.state );
    detach_ctx( ctx );
  }
}

static inline bool
add_ev_ctx( ctx_t *ctx, const struct timeval *tm )
{
  assert( !( ctx->hd.state & CTX_STATE_ADDED_EV ) );

  if ( event_add( ctx->hd.ev, tm ) )
    return false;
  ctx->hd.state |= CTX_STATE_ADDED_EV;
  attach_ctx( ctx );
  TRACE( "fd:%d cnt:%d state:%x flags:%x",
         ctx->handle.fd, ctx->hd.refcnt, ctx->hd.state, ctx->hd.flags );
  return true;
}

#define VALID_TV(_a) (((_a)->tv_sec > 0 || (_a)->tv_usec > 0) ? 1 : 0)
#define VALID_CTX(_a) (!((_a)->hd.flags & CTX_STATE_INVALID))

static void
handler_core( int fd, short flags, void *arg )
{
  ctx_t *ctx = arg;

  TRACE( "start fd:%d flags:%x state:%x --->", fd, flags, ctx->hd.state );
  assert( ctx && ctx->handle.fd == fd );

  attach_ctx( ctx );

  ctx->handle.flags |= flags;
  if ( !( event_get_events( ctx->hd.ev ) & EV_PERSIST ) )
    del_ev_ctx( ctx );

  if ( flags & EV_TIMEOUT && VALID_CTX( ctx ) ) {
    if ( ctx->handle.old_style ) {
      timer_callback cb = ctx->handle.u.timer_val.timer_cb;
      cb( ctx->handle.u.timer_val.timer_data );
    }
    else {
      new_event_cb_t cb = ctx->handle.u.timer_val.timer_cb;
      cb( -1, &ctx->handle, ctx->handle.u.timer_val.timer_data );
    }
    if ( VALID_TV( &ctx->handle.u.timer_val.interval ) ) {
      event_assign( ctx->hd.ev, ctx->hd.base->base, -1, EV_PERSIST,
                    handler_core, ctx );
      add_ev_ctx( ctx, &ctx->handle.u.timer_val.interval );
      memset( &ctx->handle.u.timer_val.interval, 0,
              sizeof( ctx->handle.u.timer_val.interval ) );
    }

  }
  else if ( flags & EV_SIGNAL ) {
    if ( ctx->handle.old_style ) {
      void (*cb)( int ) = ctx->handle.u.signal_val.signal_cb;
      cb( fd );
    }
    else {
      new_event_cb_t cb = ctx->handle.u.signal_val.signal_cb;
      cb( fd, &ctx->handle, ctx->handle.u.signal_val.signal_data );
    }
  }
  else {
    CACHE_STORE( ctx->hd.base, ctx );

    if ( flags & EV_READ && VALID_CTX( ctx ) && ctx->handle.u.fd_val.read_cb ) {
      if ( ctx->handle.old_style ) {
        event_fd_callback cb = ctx->handle.u.fd_val.read_cb;
        cb( fd, ctx->handle.u.fd_val.read_data );
      }
      else {
        new_event_cb_t cb = ctx->handle.u.fd_val.read_cb;
        cb( fd, &ctx->handle, ctx->handle.u.fd_val.read_data );
      }
    }

    if ( flags & EV_WRITE && VALID_CTX( ctx )
         && ctx->handle.u.fd_val.write_cb ) {
      if ( ctx->handle.old_style ) {
        event_fd_callback cb = ctx->handle.u.fd_val.write_cb;
        cb( fd, ctx->handle.u.fd_val.write_data );
      }
      else {
        new_event_cb_t cb = ctx->handle.u.fd_val.write_cb;
        cb( fd, &ctx->handle, ctx->handle.u.fd_val.write_data );
      }
    }

    CACHE_CLEAR( ctx->hd.base );
  }
  detach_ctx( ctx );
  TRACE( "<--- end fd:%d", fd );
}

/***************************************************************************
 * wrapping functions
 ***************************************************************************/
static void
init_event_handler_r( void )
{
  ev_base_t *ev_base;

  TRACE( "" );

  ev_base = create_ev_base( &Key, tick );
  assert( ev_base );
}

static void
init_event_handler_x( void )
{
  TRACE( "" );

  init_event_handler_r();
  Base = get_ev_base( SAFE );
}

/***************************************************************************
 *
 ***************************************************************************/
static void
finalize_event_handler_r( void )
{
  TRACE( "" );
  ev_base_t *ev_base = get_ev_base( SAFE );
  destroy_ev_base( ev_base );
}

static void
finalize_event_handler_x( void )
{
  TRACE( "" );
  if ( Base ) {
    finalize_event_handler_r();
    Base = NULL;
  }
}

/***************************************************************************
 *
 ***************************************************************************/
static bool
run_event_handler_once_raw( ev_base_t *ev_base, suseconds_t usec )
{
  TRACE( "start --->" );
  bool ret = true;

  if ( ev_base->ext_cb ) {
    external_callback_t cb = ev_base->ext_cb;
    ev_base->ext_cb = NULL;
    cb(  );
    TRACE( "done extern callback:%p", cb );
  }

  if ( ev_base->tick ) {
    struct timeval tm;
    tm.tv_sec = usec / 1000000;
    tm.tv_usec = usec % 1000000;
    evtimer_add( ev_base->tick, &tm );
  }

  /* block */
  if ( event_base_loop( ev_base->base, EVLOOP_ONCE ) < 0 )
    ret = false;
  TRACE( "<--- end %d", ret );
  return ret;
}

#if 0                           /* not implemented */
static bool
run_event_handler_once_r( int timeout_usec )
{
  return run_event_handler_once_raw( get_ev_base( SAFE ), timeout_usec );
}
#endif

static bool
run_event_handler_once_x( int timeout_usec )
{
  return run_event_handler_once_raw( get_ev_base( UNSAFE ), timeout_usec );
}

/***************************************************************************
 *
 ***************************************************************************/
static bool
start_event_handler_raw( ev_base_t *ev_base )
{
  TRACE( "" );
  ev_base->state = BASE_STATE_RUNNING;

  while ( ev_base->state == BASE_STATE_RUNNING ) {
    if ( !run_event_handler_once_raw( ev_base, tick ) )
      break;
  }
  return true;
}

static bool
start_event_handler_r( void )
{
  return start_event_handler_raw( get_ev_base( SAFE ) );
}

static bool
start_event_handler_x( void )
{
  return start_event_handler_raw( get_ev_base( UNSAFE ) );
}

/***************************************************************************
 *
 ***************************************************************************/
static void
stop_event_handler_raw( ev_base_t *ev_base )
{
  TRACE( "" );
  ev_base->state = BASE_STATE_STOP;
}

static void
stop_event_handler_r( void )
{
  stop_event_handler_raw( get_ev_base( SAFE ) );
}

static void
stop_event_handler_x( void )
{
  stop_event_handler_raw( get_ev_base( UNSAFE ) );
}

/***************************************************************************
 *
 ***************************************************************************/
static event_handle_t *
create_event_handle_raw( ev_base_t *ev_base,
                         int fd,
                         void *r_cb, void *r_arg, void *w_cb, void *w_arg )
{
  TRACE( "base:%p fd:%d", ev_base, fd );
  ctx_t *ctx;
  assert( ev_base && fd >= 0 );

  if ( ( ctx = create_ctx( ev_base, CTX_TYPE_FD ) ) ) {
    ctx->handle.fd = fd;
    ctx->handle.u.fd_val.read_cb = r_cb;
    ctx->handle.u.fd_val.read_data = r_arg;
    ctx->handle.u.fd_val.write_cb = w_cb;
    ctx->handle.u.fd_val.write_data = w_arg;
    add_db_ctx( ctx );
    return &ctx->handle;
  }
  assert( ctx );
  return NULL;
}

/* New API */
event_handle_t *
create_event_handle_r( int fd,
                       new_event_cb_t r_cb, void *r_arg,
                       new_event_cb_t w_cb, void *w_arg )
{
  return create_event_handle_raw( get_ev_base( SAFE ), fd, r_cb, r_arg, w_cb,
                                  w_arg );
}

event_handle_t *
create_event_handle( int fd,
                     new_event_cb_t r_cb, void *r_arg,
                     new_event_cb_t w_cb, void *w_arg )
{
  return create_event_handle_raw( Base, fd, r_cb, r_arg, w_cb, w_arg );
}

static bool
set_fd_handler_raw( ev_base_t *ev_base,
                    int fd,
                    event_fd_callback read_cb, void *read_d,
                    event_fd_callback write_cb, void *write_d )
{
  event_handle_t *handle = create_event_handle_raw( ev_base, fd,
                                                    read_cb, read_d,
                                                    write_cb, write_d );
  if ( handle ) {
    ctx_t *ctx = container_of( handle, ctx_t, handle );
    handle->old_style = true;
    detach_ctx( ctx );
    return true;
  }
  return false;
}

static void
set_fd_handler_r( int fd,
                  event_fd_callback read_cb, void *read_d,
                  event_fd_callback write_cb, void *write_d )
{
  set_fd_handler_raw( get_ev_base( SAFE ), fd, read_cb, read_d, write_cb,
                      write_d );
}

static void
set_fd_handler_x( int fd,
                  event_fd_callback read_cb, void *read_d,
                  event_fd_callback write_cb, void *write_d )
{
  set_fd_handler_raw( get_ev_base( UNSAFE ), fd, read_cb, read_d, write_cb,
                      write_d );
}

/***************************************************************************
 *
 ***************************************************************************/
void
destroy_event_handle( event_handle_t *handle )
{
  assert( handle );
  TRACE( "fd:%d", handle->fd);
  ctx_t *ctx = container_of( handle, ctx_t, handle );

  ctx->hd.state |= CTX_STATE_INVALID;
  ctx->handle.u.fd_val.read_cb = NULL;
  ctx->handle.u.fd_val.write_cb = NULL;
  ctx->handle.u.timer_val.timer_cb = NULL;
  del_ev_ctx( ctx );
  del_db_ctx( ctx );
  detach_ctx( ctx );
}

static bool
delete_fd_handler_raw( ev_base_t *ev_base, int fd )
{
  TRACE( "base:%p fd:%d", ev_base, fd );

  if ( ev_base ) {
    ctx_t *ctx;

    if ( ( ctx = find_ctx_fd( ev_base, fd ) ) ) {
      attach_ctx( ctx );
      destroy_event_handle( &ctx->handle );
      detach_ctx( ctx );
      return true;
    }
  }
  return false;
}

static void
delete_fd_handler_r( int fd )
{
  delete_fd_handler_raw( get_ev_base( SAFE ), fd );
}

static void
delete_fd_handler_x( int fd )
{
  delete_fd_handler_raw( Base, fd );
}

/***************************************************************************
 *
 ***************************************************************************/
static void
set_flags_ctx( ctx_t *ctx, short flags )
{
  short next = ctx->hd.flags | flags;

  TRACE( "fd:%d set flags:%x->%x state:%x",
         ctx->handle.fd, ctx->hd.flags, next, ctx->hd.state );

  ctx->handle.flags &= ( ( short ) ~flags );

  if ( ctx->hd.state & CTX_STATE_ADDED_EV ) {
    if ( event_get_events( ctx->hd.ev ) & flags ) {
      /* already set ... skip */
      TRACE( "already set flags" );
    }
    else {
      TRACE( "re-set flags:%x->%x", ctx->hd.flags, next );
      del_ev_ctx( ctx );
      ctx->hd.flags = next;
      event_assign( ctx->hd.ev, ctx->hd.base->base,
                    ctx->handle.fd, ctx->hd.flags, handler_core, ctx );
      add_ev_ctx( ctx, NULL );
    }
  }
  else {
    ctx->hd.flags = next;
    TRACE( "set flags:%x", ctx->hd.flags );
    event_assign( ctx->hd.ev, ctx->hd.base->base,
                  ctx->handle.fd, ctx->hd.flags, handler_core, ctx );
    add_ev_ctx( ctx, NULL );
  }
}

static void
clear_flags_ctx( ctx_t *ctx, short flags )
{
  short next = ctx->hd.flags & ( ( short ) ~flags );
  TRACE( "fd:%d cleared flags:%x->%x state:%x",
         ctx->handle.fd, ctx->hd.flags, next, ctx->hd.state );

  ctx->handle.flags |= flags;

  if ( ctx->hd.state & CTX_STATE_ADDED_EV ) {
    if ( event_get_events( ctx->hd.ev ) & flags ) {
      if ( ctx->hd.flags ) {
        TRACE( "re-set flags:%x->%x", ctx->hd.flags, next );
        del_ev_ctx( ctx );
        ctx->hd.flags = next;
        event_assign( ctx->hd.ev, ctx->hd.base->base,
                      ctx->handle.fd, ctx->hd.flags, handler_core, ctx );
        add_ev_ctx( ctx, NULL );
      }
      else {
        TRACE( "clear flags" );
        del_ev_ctx( ctx );
      }
    }
  }
}

void
add_read_event( event_handle_t *handle )
{
  ctx_t *ctx = container_of( handle, ctx_t, handle );
  set_flags_ctx( ctx, EV_READ );
}

void
pending_read_event( event_handle_t *handle )
{
  ctx_t *ctx = container_of( handle, ctx_t, handle );
  clear_flags_ctx( ctx, EV_READ );
}

void
add_write_event( event_handle_t *handle )
{
  ctx_t *ctx = container_of( handle, ctx_t, handle );
  set_flags_ctx( ctx, EV_WRITE );
}

void
pending_write_event( event_handle_t *handle )
{
  ctx_t *ctx = container_of( handle, ctx_t, handle );
  clear_flags_ctx( ctx, EV_WRITE );
}

static void
set_flags_raw( ev_base_t *ev_base, int fd, short flags, bool set )
{
  TRACE( "base:%p fd:%d flags:%x set:%d", ev_base, fd, flags, set );
  if ( !ev_base )
    return;
  assert( flags != 0 );

  ctx_t *ctx = find_ctx_fd( ev_base, fd );
  assert( ctx );

  if ( set )
    set_flags_ctx( ctx, flags );
  else
    clear_flags_ctx( ctx, flags );
}

static void
set_readable_r( int fd, bool state )
{
  TRACE( "fd:%d state:%d", fd, state );
  set_flags_raw( get_ev_base( SAFE ), fd, EV_READ | EV_PERSIST, state );
}

static void
set_readable_x( int fd, bool state )
{
  TRACE( "fd:%d set:%d", fd, state );
  set_flags_raw( Base, fd, EV_READ | EV_PERSIST, state );
}

/***************************************************************************
 *
 ***************************************************************************/
static void
set_writable_r( int fd, bool state )
{
  TRACE( "fd:%d state:%d", fd, state );
  set_flags_raw( get_ev_base( SAFE ), fd, EV_WRITE | EV_PERSIST, state );
}

static void
set_writable_x( int fd, bool state )
{
  TRACE( "fd:%d state:%d", fd, state );
  set_flags_raw( Base, fd, EV_WRITE | EV_PERSIST, state );
}

/***************************************************************************
 *
 ***************************************************************************/
bool
is_readable( const event_handle_t *handle )
{
  TRACE( "fd:%d flags:%x", handle->fd, handle->flags );
  return ( handle->flags & EV_READ );
}

static bool
readable_raw( ev_base_t *ev_base, int fd )
{
  TRACE( "base:%p fd:%d", ev_base, fd );
  ctx_t *ctx = find_ctx_fd( ev_base, fd );
  if ( ctx )
    return is_readable( &ctx->handle );
  return false;
}

static bool
readable_r( int fd )
{
  return readable_raw( get_ev_base( SAFE ), fd );
}

static bool
readable_x( int fd )
{
  return readable_raw( Base, fd );
}

/***************************************************************************
 *
 ***************************************************************************/
bool
is_writable( const event_handle_t *handle )
{
  TRACE( "fd:%d flags:%x", handle->fd, handle->flags );
  return ( handle->flags & EV_WRITE );
}

static bool
writable_raw( ev_base_t *ev_base, int fd )
{
  TRACE( "" );
  ctx_t *ctx = find_ctx_fd( ev_base, fd );
  if ( ctx )
    return is_writable( &ctx->handle );
  return false;
}

static bool
writable_r( int fd )
{
  return writable_raw( get_ev_base( SAFE ), fd );
}

static bool
writable_x( int fd )
{
  return writable_raw( Base, fd );
}

/***************************************************************************
 *
 ***************************************************************************/
static bool
set_external_callback_raw( ev_base_t *ev_base, external_callback_t cb )
{
  TRACE( "base:%p cb:%p", ev_base, cb );

  if ( ev_base->ext_cb )
    return false;
  ev_base->ext_cb = cb;
  return true;
}

static bool
set_external_callback_r( external_callback_t cb )
{
  return set_external_callback_raw( get_ev_base( SAFE ), cb );
}

static bool
set_external_callback_x( external_callback_t cb )
{
  return set_external_callback_raw( Base, cb );
}

/***************************************************************************
 *
 ***************************************************************************/
static void
reg_event_wrapper( void )
{
  TRACE( "" );

  init_event_handler = init_event_handler_x;
  finalize_event_handler = finalize_event_handler_x;
  start_event_handler = start_event_handler_x;
  stop_event_handler = stop_event_handler_x;
  run_event_handler_once = run_event_handler_once_x;
  set_fd_handler = set_fd_handler_x;
  delete_fd_handler = delete_fd_handler_x;
  set_readable = set_readable_x;
  set_writable = set_writable_x;
  readable = readable_x;
  writable = writable_x;
  set_external_callback = set_external_callback_x;

  init_event_handler_safe = init_event_handler_r;
  finalize_event_handler_safe = finalize_event_handler_r;
  start_event_handler_safe = start_event_handler_r;
  stop_event_handler_safe = stop_event_handler_r;
//run_event_handler_once_safe = run_event_handler_once_r;
  set_fd_handler_safe = set_fd_handler_r;
  delete_fd_handler_safe = delete_fd_handler_r;
  set_readable_safe = set_readable_r;
  set_writable_safe = set_writable_r;
  readable_safe = readable_r;
  writable_safe = writable_r;
  set_external_callback_safe = set_external_callback_r;
}

/**************************************************************************
 * timer
 **************************************************************************/

static bool
init_timer_raw( ev_base_t *ev_base )
{
  TRACE( "" );
  if ( !ev_base )
    return false;
  return true;
}

static bool
init_timer_r( void )
{
  return init_timer_raw( get_ev_base( SAFE ) );
}

static bool
init_timer_x( void )
{
  return init_timer_raw( Base );
}

/*
 *
 */
static bool
finalize_timer_raw( ev_base_t *ev_base )
{
  TRACE( "" );
  if ( !ev_base )
    return false;

  clear_db_timer_ctx( ev_base );
  return true;
}

static bool
finalize_timer_r( void )
{
  return finalize_timer_raw( get_ev_base( SAFE ) );
}

static bool
finalize_timer_x( void )
{
  return finalize_timer_raw( Base );
}

/*
 *
 */
static bool
execute_timer_events_raw( ev_base_t *ev_base, int *next_timeout_usec UNUSED )
{
  if ( !ev_base )
    return false;
  return true;
}

static void
execute_timer_events_r( int *next_timeout_usec )
{
  execute_timer_events_raw( get_ev_base( SAFE ), next_timeout_usec );
}

static void
execute_timer_events_x( int *next_timeout_usec )
{
  execute_timer_events_raw( Base, next_timeout_usec );
}

static inline void
timespec2timeval( struct timeval *tv, const struct timespec *ts )
{
  tv->tv_sec = ts->tv_sec;
  tv->tv_usec = ts->tv_nsec * 1000;
}

#define VALID_TS(_a) (((_a)->tv_sec > 0 || (_a)->tv_nsec > 0) ? 1 : 0)

/*
 *
 */
static event_handle_t *
create_timer_handle_raw( ev_base_t *ev_base,
                         const struct timeval *time,
                         const struct timeval *interval, void *cb, void *arg )
{
  ctx_t *ctx = create_ctx( ev_base, CTX_TYPE_TIMER );
  short flags = 0;
  if ( !ctx )
    return NULL;

  ctx->handle.u.timer_val.timer_cb = cb;
  ctx->handle.u.timer_val.timer_data = arg;

  add_db_ctx( ctx );

  if ( !time ) {
    time = interval;
    flags |= EV_PERSIST;
    TRACE( "add timer:0.0 interval:%u.%06u",
           ( unsigned int ) interval->tv_sec,
           ( unsigned int ) interval->tv_usec );
  }
  else if ( interval ) {
    TRACE( "add timer:%u.%06u interval:%u.%06u",
           ( unsigned int ) time->tv_sec, ( unsigned int ) time->tv_usec,
           ( unsigned int ) interval->tv_sec,
           ( unsigned int ) interval->tv_usec );
    memcpy( &ctx->handle.u.timer_val.interval, interval,
            sizeof( *interval ) );
  }
  else {
    TRACE( "add timer:%u.%06u interval:0.0",
           ( unsigned int ) time->tv_sec, ( unsigned int ) time->tv_usec );
  }

  event_assign( ctx->hd.ev, ctx->hd.base->base,
                ctx->handle.fd, flags, handler_core, ctx );
  add_ev_ctx( ctx, time );

  return &ctx->handle;
}

event_handle_t *
create_timer_handle_r( const struct timeval *time,
                       const struct timeval *interval,
                       new_event_cb_t cb, void *arg )
{
  if ( !time && !interval )
    return NULL;
  return create_timer_handle_raw( get_ev_base( SAFE ), time, interval, cb,
                                  arg );
}

event_handle_t *
create_timer_handle( const struct timeval *time,
                     const struct timeval *interval,
                     new_event_cb_t cb, void *arg )
{
  if ( !time && !interval )
    return NULL;
  return create_timer_handle_raw( Base, time, interval, cb, arg );
}

static bool
add_timer_event_callback_raw( ev_base_t *ev_base,
                              struct itimerspec *interval,
                              timer_callback callback, void *user_data )
{
  struct timeval tm, it_tm;
  struct timeval *tm_p = NULL, *it_tm_p = NULL;
  event_handle_t *handle;

  if ( VALID_TS( &interval->it_value ) ) {
    timespec2timeval( &tm, &interval->it_value );
    tm_p = &tm;
  }
  if ( VALID_TS( &interval->it_interval ) ) {
    timespec2timeval( &it_tm, &interval->it_interval );
    it_tm_p = &it_tm;
  }
  if ( !it_tm_p && !tm_p )
    return false;

  handle =
    create_timer_handle_raw( ev_base, tm_p, it_tm_p, callback, user_data );
  if ( handle ) {
    handle->old_style = true;
    return true;
  }
  return false;
}

static bool
add_timer_event_callback_r( struct itimerspec *interval,
                            timer_callback callback, void *user_data )
{
  return add_timer_event_callback_raw( get_ev_base( SAFE ), interval,
                                       callback, user_data );
}

static bool
add_timer_event_callback_x( struct itimerspec *interval,
                            timer_callback callback, void *user_data )
{
  return add_timer_event_callback_raw( Base, interval, callback, user_data );
}

/*
 *
 */
static bool
add_periodic_event_callback_raw( ev_base_t *ev_base,
                                 const time_t seconds,
                                 timer_callback callback, void *user_data )
{
  struct itimerspec interval;

  memset( &interval, 0, sizeof( interval ) );
  interval.it_interval.tv_sec = seconds;
  return add_timer_event_callback_raw( ev_base, &interval, callback,
                                       user_data );
}

static inline bool
add_periodic_event_callback_r( const time_t seconds,
                               timer_callback callback, void *user_data )
{
  return add_periodic_event_callback_raw( get_ev_base( SAFE ), seconds,
                                          callback, user_data );
}

static bool
add_periodic_event_callback_x( const time_t seconds,
                               timer_callback callback, void *user_data )
{
  return add_periodic_event_callback_raw( Base, seconds, callback,
                                          user_data );
}

/*
 *
 */
static bool
delete_timer_event_raw( ev_base_t *ev_base,
                        timer_callback callback, void *user_data )
{
  ctx_t *ctx = find_ctx_tm( ev_base, callback, user_data );

  if ( ctx ) {
    destroy_timer_handle( &ctx->handle );
    return true;
  }
  return false;
}

static bool
delete_timer_event_r( timer_callback callback, void *user_data )
{
  return delete_timer_event_raw( get_ev_base( SAFE ), callback, user_data );
}

static bool
delete_timer_event_x( timer_callback callback, void *user_data )
{
  return delete_timer_event_raw( Base, callback, user_data );
}

/*
 *
 */
static void
reg_timer_wrapper( void )
{
  TRACE( "" );

  init_timer = init_timer_x;
  finalize_timer = finalize_timer_x;
  add_timer_event_callback = add_timer_event_callback_x;
  add_periodic_event_callback = add_periodic_event_callback_x;
  delete_timer_event = delete_timer_event_x;
  execute_timer_events = execute_timer_events_x;

  init_timer_safe = init_timer_r;
  finalize_timer_safe = finalize_timer_r;
  execute_timer_events_safe = execute_timer_events_r;
  add_timer_event_callback_safe = add_timer_event_callback_r;
//add_periodic_event_callback_safe = add_periodic_event_callback_r;
  delete_timer_event_safe = delete_timer_event_r;
}


/***************************************************************************
 *
 ***************************************************************************/
static event_handle_t *
create_signal_handle_raw( ev_base_t *ev_base,
                          int sig_no, new_event_cb_t cb, void *arg )
{
  ctx_t *ctx;

  if ( ( ctx = create_ctx( ev_base, CTX_TYPE_SIGNAL ) ) == NULL )
    return NULL;

  ctx->handle.fd = sig_no;
  ctx->handle.u.signal_val.signal_cb = cb;
  ctx->handle.u.signal_val.signal_data = arg;

  add_db_ctx( ctx );

  evsignal_assign( ctx->hd.ev, ctx->hd.base->base, sig_no, handler_core, ctx );

  add_ev_ctx( ctx, NULL );
  return &ctx->handle;
}

event_handle_t *
create_signal_handle_r( int sig_no, new_event_cb_t cb, void *arg )
{
  return create_signal_handle_raw( get_ev_base( SAFE ), sig_no, cb, arg );
}

event_handle_t *
create_signal_handle( int sig_no, new_event_cb_t cb, void *arg )
{
  return create_signal_handle_raw( Base, sig_no, cb, arg );
}

static bool
reg_signal_handler_raw( int signum, void (*cb)( int ) )
{
  event_handle_t *handle;

  handle = create_signal_handle_raw( Base, signum, (new_event_cb_t) cb, NULL );
  if ( handle ) {
    ctx_t *ctx = container_of( handle, ctx_t, handle );
    handle->old_style = true;
    detach_ctx( ctx );
    return true;
  }
  return false;
}

static bool
init_signal_handler_raw( void )
{
  return true;
}

static bool
finalize_signal_handler_raw( void )
{
  return true;
}

static void
reg_signal_wrapper( void )
{
  TRACE( "" );

  reg_signal_handler = reg_signal_handler_raw;
  //  ignore_signal: not wrapping
  init_signal_handler = init_signal_handler_raw;
  finalize_signal_handler = finalize_signal_handler_raw;
}

/**************************************************************************
 * wrapper initializer
 **************************************************************************/
bool
init_libevent_wrapper( void *( *malloc_fn ) ( size_t ),
                       void *( *realloc_fn ) ( void *, size_t ),
                       void ( *free_fn ) ( void * ), suseconds_t usec )
{
  TRACE( "" );

  if ( pthread_key_create( &Key, base_destructor ) )
    return false;

  if ( malloc_fn )
    _malloc_fn = malloc_fn;
  if ( realloc_fn )
    _realloc_fn = realloc_fn;
  if ( free_fn )
    _free_fn = free_fn;

  tick = usec;

  event_set_mem_functions( _malloc_fn, NULL, _free_fn );

  reg_event_wrapper(  );
  reg_timer_wrapper(  );
  reg_signal_wrapper(  );
  return true;
}

bool
finalize_libevent_wrapper( void )
{
  TRACE( "" );
  if ( pthread_key_delete( Key ) )
    return false;
  return true;
}
