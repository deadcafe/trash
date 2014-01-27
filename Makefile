#
#

CC = gcc
AR = ar
RANLIB = ranlib

TREMA_INC = ../spot/src/lib
TREMA_LIB = ../spot/objects/lib

CPPFLAGS = -c -std=gnu99 -D_GNU_SOURCE -I$(TREMA_INC)
LDFLAGS = -L$(TREMA_LIB) -ltrema $(shell curl-config --libs) -lpthread



CFLAGS = \
         -g -O0 -std=gnu99 -D_GNU_SOURCE -fno-strict-aliasing -Werror -Wall \
         -Wextra -Wformat=2 -Wcast-qual -Wcast-align -Wwrite-strings \
         -Wconversion -Wfloat-equal -Wpointer-arith -pthread

TARGET = http
SRCS = main.c \
       http_client.c queue.c \
       func_queue.c

SRCS = hiperfifo.c hiper.c queue.c func_queue.c

#for libevent support
CPPFLAGS += -DENABLE_LIBEVENT -I./compat
LDFLAGS += -pthread -levent
SRCS += libevent_wrapper.c


OBJS = $(SRCS:.c=.o)
LIB_OBJS = $(LIB_SRCS:.c=.o)

DEPENDS = .depends

.PHONY: all clean

.SUFFIXES: .c .o

all: depend $(TARGET)

$(TARGET): $(OBJS) $(LIB)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(LIB): $(LIB_OBJS)
	@rm -f $(LIB)
	$(AR) -cq $(LIB) $(LIB_OBJS)
	$(RANLIB) $(LIB)

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

depend: 
	$(CC) -MM $(CPPFLAGS) $(SRCS) > $(DEPENDS)

clean:
	@rm -rf $(DEPENDS) $(OBJS) $(TARGET) $(LIB_OBJS) $(LIB) *~

-include $(DEPENDS)
