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

#include "hiper.h"


/*****************************************************************************
 * FIFO
 ****************************************************************************/
typedef struct {
  http_th_info_t *th_info;
  struct event *ev;
  FILE* input;
} fifo_info_t;


/* This gets called whenever data is received from the fifo */
static void
fifo_cb(int fd __attribute__((unused)),
        short events __attribute__((unused)),
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
      create_http_transaction(fifo->th_info, s);
    } else
      break;
  } while (rv != EOF);
}


/* Create a named pipe and tell libevent to monitor it */
static const char *FIFO = ".hiper.fifo";

static void
destroy_fifo_info(fifo_info_t *fifo)
{
  if (fifo) {
    if (fifo->input)
      fclose(fifo->input);
    if (fifo->ev) {
      if (event_pending(fifo->ev, (EV_READ | EV_PERSIST), NULL))
        event_del(fifo->ev);
      event_free(fifo->ev);
    }
    FREE(fifo);
  }
  unlink(FIFO);
}

static fifo_info_t *
create_fifo_info(http_th_info_t *th_info,
                 struct event_base *base)
{
  fifo_info_t *fifo;

  if ((fifo = MALLOC(sizeof(*fifo))) != NULL) {
    memset(fifo, 0, sizeof(*fifo));

    struct stat st;
    int sockfd;

    fifo->th_info = th_info;

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

    sockfd = open(FIFO, O_RDWR | O_NONBLOCK, 0);
    if (sockfd == -1) {
      perror("open");
      goto error;
    }
    fifo->input = fdopen(sockfd, "r");

    TRACE("Now, pipe some URL's into > %s", FIFO);
    fifo->ev = event_new(base, sockfd, (EV_READ | EV_PERSIST), fifo_cb, fifo);
    event_add(fifo->ev, NULL);

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
main(void)
{
  http_th_info_t *th_info;
  fifo_info_t *fifo;
  struct event_base *base;

  unsetenv("http_proxy");
  base = event_base_new();

  th_info = create_http_th(base);
  fifo = create_fifo_info(th_info, base);

  event_base_dispatch(base);

  destroy_fifo_info(fifo);
  destroy_http_th(th_info);

  event_base_free(th_info->evbase);
  return 0;
}
