#include <sys/time.h>
#include <sys/stat.h>
#include <sys/poll.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <trema.h>
#include "libevent_wrapper.h"

#include "http_client.h"


# if 1
#  include <stdlib.h>
#  include <syslog.h>
#  define _log(pri_,fmt_,...)     fprintf(stdout,fmt_,##__VA_ARGS__)
#  define LOG(pri_,fmt_,...)      _log((pri_),"%s:%d:%s() " fmt_ "\n", __FILE__,__LINE__,__func__, ##__VA_ARGS__)
#  define TRACE(fmt_,...)         LOG(LOG_DEBUG,fmt_,##__VA_ARGS__)
# endif


#define	FREE(_p)	xfree((_p))
#define	MALLOC(_s)	xmalloc((_s))


/*****************************************************************************
 * FIFO
 ****************************************************************************/
typedef struct {
  FILE* input;
  int sock;
} fifo_info_t;


static void
dump(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  //  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

static void
resp(int status,
     int code,
     const http_content_t *content,
     void *arg __attribute__((unused)))
{
  TRACE("status:%d code:%d", status, code);

  if (content) {
    if (content->type)
    TRACE("type: %s", content->type);

    if (content->body) {
      dump_buffer(content->body, dump);
      //      fputs(content->body->data, stderr);
    }
  }
}

/* Create a named pipe and tell libevent to monitor it */
static const char *FIFO = ".hiper.fifo";

static void
destroy_fifo_info(fifo_info_t *fifo)
{
  if (fifo) {
    TRACE("");
    if (fifo->input) {
      delete_fd_handler_safe(fifo->sock);
      fclose(fifo->input);
    }
    FREE(fifo);
  }
  unlink(FIFO);
}


/* This gets called whenever data is received from the fifo */
static void
fifo_cb(int sock __attribute__((unused)),
        void *arg)
{
  char s[1024];
  long int rv = 0;
  int n = 0;
  fifo_info_t *fifo = arg;

  do {
    s[0] = '\0';
    rv = fscanf(fifo->input, "%1023s%n", s, &n);
    s[n] = '\0';
    if (n && s[0]) {

      if (strcmp(s, "exit")) {
        do_http_request( HTTP_METHOD_GET, s, NULL, resp, NULL);
      } else {
        stop_http_client();
        stop_event_handler_safe();
        delete_fd_handler_safe(sock);
        return;
      }
    } else
      break;
    TRACE("=======================================================");
  } while (rv != EOF);
}


static fifo_info_t *
create_fifo_info(void)
{
  fifo_info_t *fifo;

  if ((fifo = MALLOC(sizeof(*fifo))) != NULL) {
    memset(fifo, 0, sizeof(*fifo));

    struct stat st;
    int sock;

    TRACE("Creating named pipe \"%s\"", FIFO);
    if (lstat(FIFO, &st) == 0) {
      if ((st.st_mode & S_IFMT) == S_IFREG) {
        errno = EEXIST;
        perror("lstat");
        goto error;
      }
    }

    unlink(FIFO);
    if (mkfifo(FIFO, 0600) == -1) {
      perror("mkfifo");
      goto error;
    }

    sock = open(FIFO, O_RDWR | O_NONBLOCK, 0);
    if (sock == -1) {
      perror("open");
      goto error;
    }
    fifo->input = fdopen(sock, "r");
    fifo->sock = sock;

    set_fd_handler_safe(sock, fifo_cb, fifo, NULL, NULL);
    set_readable_safe(sock, true);
    set_writable_safe(sock, false);

    TRACE("Now, pipe some URL's into > %s", FIFO);

    if (0) {
    error:
      destroy_fifo_info(fifo);
      fifo = NULL;
    }
  }
  return fifo;
}

/*****************************************************************************
 * main
 *****************************************************************************/
int
main(int ac, char **av)
{
  int opt;
  fifo_info_t *fifo;

  while ((opt = getopt(ac, av, "p")) != -1) {
    switch (opt) {
    case 'p':	/* NO Proxy */
      unsetenv("http_proxy");
      break;
    default:
      fprintf(stderr, "%s [-p]\n", av[0]);
      exit(0);
    }
  }

  init_libevent_wrapper( xmalloc, NULL, xfree, 0 );

  init_event_handler_safe();
  init_timer_safe();
  init_signal_handler();

  init_http_client(10L, 5L);

  fifo = create_fifo_info();

  start_event_handler_safe();
  TRACE("stopping main thread");
  destroy_fifo_info(fifo);

  finalize_http_client();

  finalize_signal_handler();
  finalize_timer_safe();
  finalize_event_handler_safe();

  finalize_libevent_wrapper();

  fprintf(stderr, "END\n");
  return 0;
}
