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

#define ENABLE_TRACE
#include "private_log.h"

#define	FREE(_p)	xfree((_p))
#define	MALLOC(_s)	xmalloc((_s))


/*****************************************************************************
 * FIFO
 ****************************************************************************/
typedef struct {
  FILE* input;
  int sock;
} fifo_info_t;


int counter;

static inline void
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
     const http_content *content,
     void *arg)
{
  intptr_t p = (intptr_t) arg;
  INFO("status:%d code:%d counter:%d p:%d", status, code, --counter, p);

  if (content) {
    INFO("type: %s", content->content_type);

    if (content->body->data) {
      //      dump_buffer(content->body, dump);
      INFO("resp content length:%zu", content->body->length);
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
      if (fifo->sock >= 0)
        delete_fd_handler(fifo->sock);
      fclose(fifo->input);
    }
    FREE(fifo);
  }
  unlink(FIFO);
}


/* maybe unused */
static inline void
http_client_end(void *arg)
{
  TRACE("arg:%p", arg);

  finalize_http_client();
  stop_event_handler();
}

/* This gets called whenever data is received from the fifo */
static void
fifo_cb(int sock,
        void *arg)
{
  char s[1024];
  long int rv = 0;
  int n = 0;
  fifo_info_t *fifo = arg;

  TRACE("sock:%d", sock);

  do {
    s[0] = '\0';
    rv = fscanf(fifo->input, "%1023s%n", s, &n);
    s[n] = '\0';
    if (n && s[0]) {

      if (strcmp(s, "exit")) {
        intptr_t p = counter;

        do_http_request( HTTP_METHOD_GET, s, NULL, resp, (void *)p);
        counter++;
      } else {
        set_readable(sock, false);
        delete_fd_handler(sock);
        fifo->sock = -1;

        finalize_http_client();

        stop_event_handler();
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

    set_fd_handler(sock, fifo_cb, fifo, NULL, NULL);
    set_readable(sock, true);

    INFO("Now, pipe some URL's into > %s", FIFO);

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

#if 0
  init_libevent_wrapper( xmalloc, NULL, xfree, 0 );
#endif

  init_trema(&ac, &av);
  TRACE("Done trema init");

  set_logging_level("info");

  init_http_client(NULL);

  fifo = create_fifo_info();

  TRACE("starting trema");
  start_trema();
  TRACE("stoping trema");


  TRACE("stopping main thread");
  destroy_fifo_info(fifo);


#if 0
  finalize_libevent_wrapper();
#endif

  fprintf(stderr, "END\n");
  return 0;
}
