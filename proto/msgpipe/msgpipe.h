/*
 *	BIRD -- Table-to-messaging and messaging-to-Table Routing Protocol a.k.a MsgPipe
 *
 *	(c) 2019 Gil <gilwo@null.net>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#ifndef _BIRD_MSGPIPE_H_
#define _BIRD_MSGPIPE_H_

#define NATS_HAS_STREAMING
#include <nats/nats.h>
#include <mpack.h>

#include <pthread.h>

struct msgpipe_config {
  struct proto_config c;
  struct rtable_config *peer;		/* Table we're connected to */

  char *pubDataName;
  char *pubControlName;
  char *subName;
};

struct msgpipe_proto {
  struct proto p;
  struct channel *pri;
  struct channel *sec;

  stanConnection     *conn;
  stanSubscription   *sub;
  bool                connLost;
  char *pubDataName;
  char *pubControlName;
  char *subName;
  bool shutdownAck;

  long long msgPubUpdates;
  long long msgPubAcked;
  long long msgPubErrored;

  long long msgSubUpdates;
  long long msgSubAcked;
  long long msgSubSkipped;


  struct mploop *loop;
  pool *tpool;
  pthread_spinlock_t lock;
  slab *msg_slab;

};

struct msgpipe_message {
  net_addr_union prefix;
  ip_addr nexthop;
  rte *new;
  rte *old;
  bool is_manual;
};

#endif
