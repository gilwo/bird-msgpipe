
#ifndef _MSGPIPE_IO_
#define _MSGPIPE_IO_

struct mploop *mploop_new(void);
void mploop_start(struct mploop *loop);
void mploop_stop(struct mploop *loop);
void mploop_free(struct mploop *loop);

void mploop_enter(struct mploop *loop);
void mploop_leave(struct mploop *loop);
void mploop_mask_wakeups(struct mploop *loop);
void mploop_unmask_wakeups(struct mploop *loop);

#endif /* _MSGPIPE_IO_ */