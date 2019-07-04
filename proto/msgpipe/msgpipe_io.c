

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

#include "msgpipe_io.h"

#include "nest/bird.h"

#include "lib/buffer.h"
#include "lib/lists.h"
#include "lib/resource.h"
#include "lib/event.h"
#include "lib/timer.h"
#include "lib/socket.h"

struct mploop {

    pool *pool;
    pthread_t thread;
    pthread_mutex_t mutex;

    u8 stop_called;
    struct timeloop time;

    // TODO: try to optimize with double event list
    list event_list;
    u8 processing_list;
};

static pthread_key_t current_loop_key;
extern pthread_key_t current_time_key;

static inline struct mploop *
mploop_current(void)
{
    return pthread_getspecific(current_loop_key);
}

static inline void
mploop_set_current(struct mploop *loop)
{
    pthread_setspecific(current_loop_key, loop);
    pthread_setspecific(current_time_key, loop ? &loop->time : &main_timeloop);
}

static inline void
mploop_init_current(void)
{
    pthread_key_create(&current_loop_key, NULL);
}

static inline void
events_init(struct mploop *loop)
{
  init_list(&loop->event_list);
}

static inline uint 
events_waiting(struct mploop* loop)
{
  return !EMPTY_LIST(loop->event_list);
}

static void
events_fire(struct mploop *loop)
{
  ev_run_list(&loop->event_list);
}

event *
mploop_new_event(struct mploop *loop, void (*hook)(void *), void *data)
{
  event *e = ev_new(loop->pool);
  e->hook = hook;
  e->data = data;
  return e;
}

void
mloop_qev(struct mploop *loop, event *ev)
{
  ev_enqueue(&loop->event_list, ev);
}

struct mploop *
mploop_new(void)
{
  static int init = 0;
  if (!init)
    { mploop_init_current(); init = 1; }

  pool *p = rp_new(NULL, "Birdloop root");
  struct mploop *loop = mb_allocz(p, sizeof(struct mploop));
  loop->pool = p;
  pthread_mutex_init(&loop->mutex, NULL);

//   wakeup_init(loop);

  events_init(loop);
  timers_init(&loop->time, p);
//   sockets_init(loop);

  return loop;
}

static void *mploop_main();

void
mploop_start(struct mploop *loop)
{
    int rv = pthread_create(&loop->thread, NULL, mploop_main, loop);
    if (rv)
      die("pthread_create(): %M", rv);
}

void
mploop_stop(struct mploop *loop)
{
    pthread_mutex_lock(&loop->mutex);
    loop->stop_called = 1;
    pthread_mutex_unlock(&loop->mutex);

    int rv = pthread_join(loop->thread, NULL);
    if (rv)
      die("pthread_join(): %M", rv);
}

void
mploop_free(struct mploop* loop)
{
    rfree(loop->pool);
}

void
mploop_enter(struct mploop *loop)
{
  pthread_mutex_lock(&loop->mutex);
  mploop_set_current(loop);
}

void
mploop_leave(struct mploop *loop)
{
  mploop_set_current(NULL);
  pthread_mutex_unlock(&loop->mutex);
}

static void *
mploop_main(void *arg)
{
  struct mploop *loop = arg;
  timer *t;
  int rv, timeout;

  mploop_set_current(loop);

  pthread_mutex_lock(&loop->mutex);

  while (1)
  {
    // events_fire(loop);
    pthread_mutex_unlock(&loop->mutex);
    sleep(1);
    pthread_mutex_lock(&loop->mutex);
    if (loop->stop_called)
      break;
  }
  loop->stop_called = 0;
  pthread_mutex_unlock(&loop->mutex);
}
