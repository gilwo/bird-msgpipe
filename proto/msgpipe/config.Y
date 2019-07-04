/*
 *	BIRD -- Table-to-Table Protocol Configuration
 *
 *	(c) 1999 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

CF_HDR

#include "proto/msgpipe/msgpipe.h"

CF_DEFINES

#define MSGPIPE_CFG ((struct msgpipe_config *) this_proto)

CF_DECLS

CF_KEYWORDS(MSGPIPE, PEER, TABLE, PUBDATA, PUBCONTROL, SUB)

CF_GRAMMAR

proto: msgpipe_proto '}' { this_channel = NULL; }  ;

msgpipe_proto_start: proto_start MSGPIPE
{
  this_proto = proto_config_new(&proto_msgpipe, $1);
}
proto_name
{
  this_channel = proto_cf_main_channel(this_proto);
  if (!this_channel) {
    this_channel = channel_config_new(NULL, NULL, 0, this_proto);
    this_channel->in_filter = FILTER_ACCEPT;
    this_channel->out_filter = FILTER_ACCEPT;
  }
};

msgpipe_proto:
   msgpipe_proto_start '{'
 | msgpipe_proto proto_item ';'
 | msgpipe_proto channel_item ';'
 | msgpipe_proto PEER TABLE rtable ';' { MSGPIPE_CFG->peer = $4; }
 | msgpipe_proto PUBDATA text ';' { MSGPIPE_CFG->pubDataName = $3; }
 | msgpipe_proto PUBCONTROL text ';' { MSGPIPE_CFG->pubControlName = $3; }
 | msgpipe_proto SUB text ';' { MSGPIPE_CFG->subName = $3; }
 ;

CF_CODE

CF_END
