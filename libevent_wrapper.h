/*
 * test bed
 */

#ifndef _LIBEVENT_WRAPPER_H_
#define _LIBEVENT_WRAPPER_H_

#include <sys/types.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdbool.h>

#include <event_handler.h>
#include <timer.h>
#include <safe_event_handler.h>
#include <safe_timer.h>
#include <signal_handler.h>

#define CTX_TYPE_INVALID        0
#define CTX_TYPE_TIMER          1
#define CTX_TYPE_FD             2
#define CTX_TYPE_SIGNAL         3

typedef struct event_handle event_handle_t;
typedef void ( *new_event_cb_t ) ( int fd, event_handle_t *handle,
                                   void *arg );

struct event_handle {
  short type;
  short flags;
  int old_style;
  int fd;                       /* or signal */

  union {
    struct {
      void *read_cb;
      void *read_data;

      void *write_cb;
      void *write_data;
    } fd_val;

    struct {
      void *timer_cb;
      void *timer_data;
      struct timeval interval;
    } timer_val;

    struct {
      void *signal_cb;
      void *signal_data;
    } signal_val;
  } u;
};

/*
 *
 */
extern event_handle_t *create_event_handle_r( int fd,
                                              new_event_cb_t r_cb,
                                              void *r_arg,
                                              new_event_cb_t w_cb,
                                              void *w_arg );
extern event_handle_t *create_event_handle( int fd, new_event_cb_t r_cb,
                                            void *r_arg, new_event_cb_t w_cb,
                                            void *w_arg );
extern void destroy_event_handle( event_handle_t *eh );
extern void add_read_event( event_handle_t *eh );
extern void pending_read_event( event_handle_t *eh );
extern void add_write_event( event_handle_t *eh );
extern void pending_write_event( event_handle_t *eh );
extern bool is_readable( const event_handle_t *eh );
extern bool is_writable( const event_handle_t *eh );

extern event_handle_t *create_timer_handle( const struct timeval *time,
                                            const struct timeval *interval,
                                            new_event_cb_t cb, void *arg );
extern event_handle_t *create_timer_handle_r( const struct timeval *time,
                                              const struct timeval *interval,
                                              new_event_cb_t cb, void *arg );
#define destroy_timer_handle(h_)        destroy_event_handle(h_)

extern event_handle_t *create_signal_handle( int signal,
                                             new_event_cb_t cb, void *arg );
extern event_handle_t *create_signal_handle_r( int signal,
                                               new_event_cb_t cb, void *arg );
#define destroy_signal_handle(h_)       destroy_event_handle(h_)

extern bool init_libevent_wrapper( void *( *malloc_fn ) ( size_t sz ),
                                   void *( *realloc_fn ) ( void *ptr, size_t sz ),
                                   void ( *free_fn ) ( void *ptr ),
                                   suseconds_t tick );
extern bool finalize_libevent_wrapper( void );

#endif /* !_LIBEVENT_WRAPPER_H_ */
