/*
 *	BIRD -- Table-to-messaging and messaging-to-Table Routing Protocol a.k.a MsgPipe
 *
 *	(c) 2019 Gil <gilwo@null.net>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

/**
 * DOC: MsgPipe
 *
 * msgpipe is based opon pipe protocol.
 * basically instead of passing the routes between tables, the protocol perform two operations:
 *
 * updates on the main table recieved by protocol and are translated and passed
 * to the publish messaging channel defined in the protocol configuration.
 *
 * upon recieving messages from the subscribed messaging channel the messages are translated and
 * checked:
 * 1. if the message is manual then a manual update of route information is updated on the peer table 
 * 2. otherwise the route inforamtion are checked against the main table (to check if exists) and then 
 * are passed to the peer table.
 * 
 * this is a kind of conditional updates of route infomration where the actual decision is taken place
 * in another scope.
 *
 */

#undef LOCAL_DEBUG

#include "nest/bird.h"
#include "nest/iface.h"
#include "nest/protocol.h"
#include "nest/route.h"
#include "nest/cli.h"
#include "conf/conf.h"
#include "filter/filter.h"
#include "lib/string.h"

#include "msgpipe.h"
#include "msgpipe_io.h"

#include "proto/bgp/bgp.h"

#include <signal.h>

#define MAX_SIZE 1024
static void _pubAckHandler(const char *guid, const char *error, void *closure);
static void onMsg(stanConnection *sc, stanSubscription *sub, const char *channel, stanMsg *msg, void *closure);

#define NATS_STATUS_STR(x) natsStatus_GetText(x)

#define MPACK_BUF_MAXSIZE 4096
static char mpackbuf[MPACK_BUF_MAXSIZE];

mpack_error_t parse_msgpack_element(mpack_reader_t *r, struct msgpipe_message *msgp)
{
  mpack_error_t mperr = mpack_ok;
  mpack_tag_t tag;
  mpack_type_t tagtype;
  
  #define WHICH 'w'
  #define BEGIN 's'
  #define END 'e'
  #define BEST 'b'
  #define PREFIX 'p'

  tag = mpack_read_tag(r);
  if ( (tagtype = mpack_tag_type(&tag)) != mpack_type_nil &&
    (mperr = mpack_reader_error(r)) == mpack_ok)
  {
    switch (tagtype)
    {
    case mpack_type_map:
      for (uint32_t i = mpack_tag_map_count(&tag); i > 0; --i)
      {
        debug("tag count : %d err: %s\n", i, mpack_error_to_string(mpack_reader_error(r)));
        if ((mperr = parse_msgpack_element(r, msgp)) != mpack_ok)
          return mperr;
      }
        debug("map done: reader err: %s\n", mpack_error_to_string(mpack_reader_error(r)));
      mpack_done_map(r);
        debug("map done: reader err: %s\n", mpack_error_to_string(mpack_reader_error(r)));
      break;
    case mpack_type_array:
      // TODO: fix this - this should not actually be done - need to address each BA field specifically ...
      for (uint32_t i = mpack_tag_array_count(&tag); i > 0; --i)
      {
          parse_msgpack_element(r, msgp);
      }
      break;
    case mpack_type_str:
    {
      char buf[5] = {0};
      int numtag;
      int count = mpack_tag_str_length(&tag);
      mpack_read_cstr(r, buf, 4, count < 4 ? count : 1);
      debug("mpack tags cstring: %s\n", buf);
      if ((mperr = mpack_reader_error(r)) != mpack_ok)
        return  mperr;
      switch ((numtag = atoi(buf)))
      {
        case BA_NEXT_HOP:
        {
          tag = mpack_read_tag(r);
          if ((tagtype = mpack_tag_type(&tag)) != mpack_type_array)
            return mpack_error_invalid;
          if (mpack_tag_array_count(&tag) != 4)
            return mpack_error_invalid;
          {
            uint32_t iparr[4];

            for (int i = 0; i < 4; i++)
            {
              tag = mpack_read_tag(r);
              if ((tagtype = mpack_tag_type(&tag)) != mpack_type_uint)
                return mpack_error_invalid;
              iparr[i] = mpack_tag_uint_value(&tag);
            }

            {
              ip_addr *ref = (ip_addr *)&msgp->nexthop;
              debug(" !! GA: %#8x %#8x %#8x %#8x\n", iparr[0], iparr[1], iparr[2], iparr[3]);
              *ref = ipa_build6(iparr[0], iparr[1], iparr[2], iparr[3]);
              // temp debug TODO: remove following scope
              {
                char buf[128] = {0};
                bsnprintf(buf, 128, "%I", msgp->nexthop);
                debug("!! giig: nexthop: %s\n", buf);
              }
            }
          }
          mpack_done_array(r);
        }
        break;
        case BA_ORIGIN:
        case BA_AS_PATH:
        case BA_MULTI_EXIT_DISC:
        case BA_LOCAL_PREF:
        case BA_ATOMIC_AGGR:
        case BA_AGGREGATOR:
        case BA_COMMUNITY:
        case BA_ORIGINATOR_ID:
        case BA_CLUSTER_LIST:
        case BA_MP_REACH_NLRI:
        case BA_MP_UNREACH_NLRI:
        case BA_EXT_COMMUNITY:
        case BA_AS4_PATH:
        case BA_AS4_AGGREGATOR:
        case BA_LARGE_COMMUNITY:
        // skip all the above
        //just skip it
        parse_msgpack_element(r, msgp);
        
        // extend below to parse appropriatly
        if (0)
        {
          uint32_t arr_count;
          tag = mpack_read_tag(r);
          tagtype = mpack_tag_type(&tag);
          if (tagtype == mpack_type_uint)
          {
              tag = mpack_read_tag(r);
              debug("skipping tag uint: %s (%d)\n", buf, mpack_tag_uint_value(&tag));
              break;
          }
          if (tagtype == mpack_type_array)
          {
            arr_count = mpack_tag_array_count(&tag);
            for (uint i = 0; i < arr_count; i++)
            {
              tag = mpack_read_tag(r);
              //....
            }
          }
            return mpack_error_invalid;
        }
        break;

        default: // non numbers
        {
          const char b = buf[0];
          if (b == BEGIN || b == END || b == WHICH)
          {
            tag = mpack_read_tag(r);
            if ((tagtype = mpack_tag_type(&tag)) != mpack_type_uint)
              return mpack_error_invalid;
            if (b == BEGIN && mpack_tag_uint_value(&tag) != 0xdeadbeaf)
              return mpack_error_invalid;
            if (b == END && mpack_tag_uint_value(&tag) != 0xbeafdead)
              return mpack_error_invalid;
            if (b == WHICH)
            {
              int which_val = (int)mpack_tag_uint_value(&tag);
              msgp->is_manual = msgp->is_withdraw = false;
              debug("which flag: %d\n", which_val);
              if (which_val > 4 || which_val < 1)
                return mpack_error_invalid;
              if (which_val == 2)
                msgp->is_withdraw = true;
              if (which_val == 4)
                msgp->is_manual = true;
            }
            break;
          }
          if (b == PREFIX)
          {
            tag = mpack_read_tag(r);
            if ((tagtype = mpack_tag_type(&tag)) != mpack_type_array)
              return mpack_error_invalid;
            if (mpack_tag_array_count(&tag) != 5)
              return mpack_error_invalid;
            {
              uint32_t iparr[4];
              uint32_t prefix_len;

              {
                tag = mpack_read_tag(r);
                if ((tagtype = mpack_tag_type(&tag)) != mpack_type_uint)
                  return mpack_error_invalid;
                prefix_len = mpack_tag_uint_value(&tag);
              }

              for (int i = 0; i < 4; i++)
              {
                tag = mpack_read_tag(r);
                if ((tagtype = mpack_tag_type(&tag)) != mpack_type_uint)
                  return mpack_error_invalid;
                iparr[i] = mpack_tag_uint_value(&tag);
              }

              {
                ip_addr d = {0};
                d = ipa_build6(iparr[0], iparr[1], iparr[2], iparr[3]);
                net_fill_ipa((net_addr *)&msgp->prefix, d, prefix_len);
                {
                  char buf[128] = {0};
                  bsnprintf(buf, 128, "%N", &msgp->prefix);
                  debug("!! giig: prefix: %s\n", buf);
                }
              }
            }
            mpack_done_array(r);
            break;
          }
          // we need to ignore best becuase of how we pack it in the other side and it is not needed for manual update
          if (b == BEST)
          {
            tag = mpack_read_tag(r);
            if ((tagtype = mpack_tag_type(&tag)) != mpack_type_array)
              return mpack_error_invalid;
            if (mpack_tag_array_count(&tag) != 4)
              return mpack_error_invalid;

            for (int i = 0; i < 4; i++)
            {
              tag = mpack_read_tag(r);
              if ((tagtype = mpack_tag_type(&tag)) != mpack_type_uint)
                return mpack_error_invalid;
              // do nothing 
            }
            mpack_done_array(r);
            break;
          }
        }
        break;

      }
    }

    break;
    default:
    debug("unknown tag type: %s\n", mpack_type_to_string(tagtype));
      break;
    }
  }

  if (mpack_reader_error(r) != mpack_ok)
      debug("mpack reader errorr: %s\n", mpack_error_to_string(mpack_reader_error(r)));
  if (tagtype == mpack_type_nil && mperr == mpack_ok)
      return mpack_ok;
  return tagtype == mpack_type_nil ? mpack_error_type : mperr;
}


void DumpHex(const void* data, size_t size)
{
    char ascii[17];
    size_t i, j;
    ascii[16] = '\0';
    for (i = 0; i < size; ++i) {
        debug("%02X ", ((unsigned char*)data)[i]);
        if (((unsigned char*)data)[i] >= ' ' && ((unsigned char*)data)[i] <= '~') {
            ascii[i % 16] = ((unsigned char*)data)[i];
        } else {
            ascii[i % 16] = '.';
        }
        if ((i+1) % 8 == 0 || i+1 == size) {
            debug(" ");
            if ((i+1) % 16 == 0) {
                debug("|  %s \n", ascii);
            } else if (i+1 == size) {
                ascii[(i+1) % 16] = '\0';
                if ((i+1) % 16 <= 8) {
                    debug(" ");
                }
                for (j = (i+1) % 16; j < 16; ++j) {
                    debug("   ");
                }
                debug("|  %s \n", ascii);
            }
        }
    }
}

static bool
fill2_msgp(struct msgpipe_message *msgp, const char *msg, size_t msglen)
{
  mpack_error_t mperr;
  mpack_reader_t reader, *r = &reader;
  mpack_reader_init_data(r, msg, msglen);

  DumpHex(msg, msglen);
  if ((mperr = parse_msgpack_element(r, msgp)) != mpack_ok)
  {
    log(L_ERR "failed to parse recieved message");
    debug("failed to parse msg %s\n", mpack_error_to_string(mperr));
    return false;
  }

  if ( (mperr = mpack_reader_destroy(r)) != mpack_ok)
  {
    log(L_ERR "failed to parse recieved message - destroy");
    debug("failed to destroy reader %s\n", mpack_error_to_string(mperr));
    return false;
  }
  debug("parse msgpack success\n");
  return true;
}

static void
update_msgp(struct msgpipe_proto *p, struct msgpipe_message *msgp)
{
  struct channel *dst = p->sec;
  struct channel *src_ch = p->pri;
  struct rte_src *src;

  rte *new;// = msgp->new;
  rte *old;// = msgp->old;

  rte *e = NULL;
  rta *a = NULL;

  net *n, *nexist;

  // debug("new: '%x', old: '%x'\n", new, old);
  debug("looking for prefix %-1N\n", &msgp->prefix);

  if (dst->table->msgpipe_busy)
  {
    log(L_ERR "MsgPipe loop detected when sending %N to table %s",
        msgp->prefix, dst->table->name);
    return;
  }

  // does the prefix exist already in destination 
  nexist = net_find(dst->table, &msgp->prefix);
  if (!nexist)
  {
    log(L_ERR "no entry found for prefix %N in destination", &msgp->prefix);
    debug(L_ERR "no entry found for prefix %N in destination\n", &msgp->prefix);
  }
  else
  {
    debug(L_ERR "found prefix %N in destination\n", &msgp->prefix);
    for (rte *e = nexist->routes; e; e = e->next)
      rte_dump(e);
  }

  if (msgp->is_manual)
  {
    debug("manual update ... \n");
    if (!ipa_zero2(msgp->nexthop))
    {
      u32 path_id = net_hash(&msgp->prefix);
      a = allocz(RTA_MAX_SIZE);
      //struct nexthop *nhs =  NULL;
      //struct nexthop *nh = allocz(NEXTHOP_MAX_SIZE);

      a->src = p->p.main_source;
      a->source = RTS_MSGPIPE;
      a->scope = SCOPE_UNIVERSE;
      a->dest = RTD_UNREACHABLE;
      a->from = msgp->nexthop;
      //nh->gw = msgp->nexthop;
      //nexthop_insert(&nhs, nh);
      //nexthop_link(a, nhs);
      // a->nh = NULL;

      e = rte_get_temp(a);
      src = a->src;
    }
  }
  else
  {
    // is the prefix exist in the source table
    // if the message is for update for known route
    n = net_find(src_ch->table, &msgp->prefix);
    if (!n)
    {
      log(L_ERR "no entry found for prefix %N", &msgp->prefix);
      debug(L_ERR "no entry found for prefix %N in source\n", &msgp->prefix);
      return;
    }

    e = n->routes;
    //TODO: change to for loop
    while (e)
    {
      rte_dump(e);
      {
        eattr *newnhattr = ea_find(e->attrs->eattrs, EA_CODE(PROTOCOL_BGP, BA_NEXT_HOP));
        ip_addr nh = *(ip_addr *)newnhattr->u.ptr->data;
        if (ipa_equal(nh, msgp->nexthop))
          break;
      }
      if (e)
        e = e->next;
    }

    // new = e;
    if ((new = e) != NULL)
    {
      debug("new entry\n");
      a = alloca(rta_size(new->attrs));
      memcpy(a, new->attrs, rta_size(new->attrs));

      a->aflags = 0;
      a->hostentry = NULL;
      e = rte_get_temp(a);
      e->pflags = 0;

      /* Copy protocol specific embedded attributes. */
      memcpy(&(e->u), &(new->u), sizeof(e->u));
      e->pref = new->pref;
      e->pflags = new->pflags;

#ifdef CONFIG_BGP
      /* Hack to cleanup cached value */
      if (e->attrs->src->proto->proto == &proto_bgp)
        e->u.bgp.stale = -1;
#endif

      src = a->src;
    }
    // else
    // {
    //   e = NULL;
    //   src = old->attrs->src;
    // }
  }
  // first removed current route for prefix if exist in dst
  dst->table->msgpipe_busy = 1;
  // does the prefix exist already in destination
  if (nexist && nexist->routes && nexist->routes->attrs && nexist->routes->attrs->src)
    rte_update2(dst, &msgp->prefix, NULL, nexist->routes->attrs->src);
  else if (nexist)
    rte_update2(dst, &msgp->prefix, NULL, p->p.main_source);

  dst->table->msgpipe_busy = 0;
  // now update what we want
  src_ch->table->msgpipe_busy = 1;
  if (e && !msgp->is_withdraw)
  {
    debug("adding:\n");
    // rte_dump(e);
    rte_update2(dst, &msgp->prefix, e, src);
  }
  //rte_update(dst, &msgp->prefix, e);
  src_ch->table->msgpipe_busy = 0;

  debug("!! done updating\n");
  return;
}

int msgpack_bgp_attrs(rte *rte, mpack_writer_t *w, int *count)
{
  struct protocol *p;
  char tbuf[128] ={0}, *pos = tbuf, *end = tbuf+sizeof(tbuf);
  eattr *e = NULL;
  int i;
  if (!w && !count)
    return 1;

  for (ea_list *eal = rte->attrs->eattrs; eal; eal = eal->next)
  {
    for (i = 0, e = &eal->attrs[0] ; i < eal->count; i++, e++)
    {
      if (EA_PROTO(e->id) != PROTOCOL_BGP)
        return 2;
      if (count)
        (*count)++;
      if (w)
      {

        int status = GA_UNKNOWN;
        struct adata *ad = (e->type & EAF_EMBEDDED) ? NULL : e->u.ptr;

        {
          bsprintf(tbuf, "%u", EA_ID(e->id));
          // mpack_write_u8(w, EA_ID(e->id));
          mpack_write_cstr(w, tbuf);
          debug("attrs2(): id %u\n", EA_ID(e->id));
          switch (e->type & EAF_TYPE_MASK)
          {
            case EAF_TYPE_INT:
              mpack_write_u32(w, e->u.data);
              debug("attrs2(): type int  %d\n", e->u.data);
              break;
            case EAF_TYPE_OPAQUE:
              {

              debug("attrs2(): type opaque length %d\n", ad->length);
                mpack_start_array(w, ad->length);
                for (int i = 0; i < ad->length; i++)
                {
                  mpack_write_uint(w, ad->data[i]);
                  debug("attrs2(): type opaque e[%d] %d\n", i, ad->data[i]);
                }
                mpack_finish_array(w);
              debug("attrs2(): type opaque done\n");
              }
              break;
            case EAF_TYPE_IP_ADDRESS:
              {
                ip_addr *ip = (ip_addr *)ad->data;
              debug("attrs2(): type ip length %d\n", 4);
                mpack_start_array(w, 4);
                for (int i = 0; i < 4; i++)
                {
                    mpack_write_u32(w, ip->addr[i]);
                  debug("attrs2(): type ip e[%d] %u\n", i, ip->addr[i]);
              }
                mpack_finish_array(w);
              }
              break;
            case EAF_TYPE_ROUTER_ID:
              {
                ip_addr *ip = (ip_addr *)ad->data;
                mpack_start_array(w, 4);
              debug("attrs2(): type routerid length %d\n", 4);
                for (int i = 0; i < 4; i++)
                {
                    mpack_write_u32(w, ip->addr[i]);
                  debug("attrs2(): type routerid e[%d] %u\n", i, ip->addr[i]);
                }
                mpack_finish_array(w);
              }
            case EAF_TYPE_AS_PATH:
            // mpack_write_cstr(w, "as empty");
            // break;
            {
              char databuf[200];
              as_path_format(ad, databuf, 200);
              debug("databuf: %s\n", databuf);
            }
            {
              int as_elements_count = 0;
              const byte *pos = ad->data;
              const byte *end = pos + ad->length;

              while (pos < end) 
              {
                uint len = pos[1];
                pos += 2;
                as_elements_count++;
                pos += 4*len;
              }
              debug("attrs2(): as_path elements count: %d\n", as_elements_count);

              pos = ad->data;
              end = pos + ad->length;
              mpack_start_array(w, as_elements_count);
              while (as_elements_count--)
              {
                uint type = pos[0];
                uint len = pos[1];
                pos +=2;

                debug("attrs2(): type as path type: %d, length %d\n", type, len);
                mpack_start_array(w, len+1);
                mpack_write_u32(w, type);
                while (len--)
                {
                  uint asn = get_u32(pos);
                  pos+=4;
                  mpack_write_u32(w, asn);
                  debug("attrs2(): type asn : %u\n", asn);
                }
                mpack_finish_array(w);
              }
              mpack_finish_array(w);
            }
            break;
            case EAF_TYPE_INT_SET:
            // mpack_write_cstr(w, "int set empty");
            // break;
            {
              char databuf[200];
              char *buf = databuf, *pos = buf, *end = buf + 200;
              int i = int_set_format(ad, 1, 0, pos, end - pos);
              while (i)
                i = int_set_format(ad, 1, i, buf, end - buf - 1);
              debug("int set databuf: %s\n", databuf);
            }
            {
              u32 *z = ad->data;
              uint set_len = ad->length / 4;
              debug("attrs2(): type int set length %d\n", set_len);
              mpack_start_array(w, set_len);
              for( int i = 0; i < set_len; i++)
              {
                mpack_start_array(w, 2);
                mpack_write_u16(w, z[i] >> 16);
                mpack_write_u16(w, z[i] & 0xffff);

                debug("attrs2(): type int set [%d] : %u, %u\n", i, z[i] >> 16, z[i] & 0xffff);
                mpack_finish_array(w);
              }
              mpack_finish_array(w);

            }
            break;
            case EAF_TYPE_EC_SET:
            // mpack_write_cstr(w, "ec empty");
            // break;
            {
              char databuf[200];
              char *buf = databuf, *pos = buf, *end = buf + 200;
              int i = ec_set_format(ad, 0, pos, end - pos);
              while (i)
                i = ec_set_format(ad, i, buf, end - buf - 1);
              debug("rc set databuf: %s\n", databuf);
            }
            {
              u32 *z = ad->data;
              int set_len = ad->length / 8;
              debug("attrs2(): type ec set length %d\n", set_len);
              mpack_start_array(w, set_len);
              for( int i = 0; i < set_len * 2; i+=2)
              {
                mpack_write_u64(w, ((u64) z[i] << 32) | z[i+1]);

                debug("attrs2(): type ec set [%d,%d] : %lu\n", i, i+1, ((u64) z[i] << 32) | z[i+1]);
              }
              mpack_finish_array(w);
            }
            break;
            case EAF_TYPE_LC_SET:
            // mpack_write_cstr(w, "lc empty");
            // break;
            {
              char databuf[200];
              char *buf = databuf, *pos = buf, *end = buf + 200;
              int i = lc_set_format(ad, 0, pos, end - pos);
              while (i)
                i = lc_set_format(ad, i, buf, end - buf - 1);
              debug("lc set databuf: %s\n", databuf);
            }
            {
              u32 *z = ad->data;
              uint lc_set_len = ad->length / 4 / 3; // 4 bytes for each value and 3 values
              debug("attrs2(): type lc set length %d\n", lc_set_len);
              mpack_start_array(w, lc_set_len);
              for( int i = 0; i < lc_set_len ; i++)
              {
                mpack_start_array(w, 3);
                mpack_write_u32(w, z[i*3]);
                mpack_write_u32(w, z[i*3 + 1]);
                mpack_write_u32(w, z[i*3 + 2]);

                debug("attrs2(): type lc set [%d] : %u, %u, %u\n", i, z[i], z[i+1], z[i+2]);
                mpack_finish_array(w);
              }
              mpack_finish_array(w);
            }
            break;
          }
        }
      }
    }
  }

  return 0;
}


static void
msgpipe_rt_notify(struct proto *P, struct channel *src_ch, net *n, rte *new, rte *old)
{
  struct msgpipe_proto *p = (void *) P;
  struct channel *dst = (src_ch == p->pri) ? p->sec : p->pri;
  struct rte_src *src;

  rte *e;
  rta *a;

  if (!new && !old)
    return;

  debug("new: '%lx', old: '%lx'\n", new, old);
  if (dst->table->msgpipe_busy)
    {
      log(L_ERR "MsgPipe loop detected when sending %N to table %s",
	  n->n.addr, dst->table->name);
      return;
    }

  if (new)
  {
    a = alloca(rta_size(new->attrs));
    memcpy(a, new->attrs, rta_size(new->attrs));

    a->aflags = 0;
    a->hostentry = NULL;
    e = rte_get_temp(a);
    e->pflags = 0;

    /* Copy protocol specific embedded attributes. */
    memcpy(&(e->u), &(new->u), sizeof(e->u));
    e->pref = new->pref;
    e->pflags = new->pflags;

#ifdef CONFIG_BGP
    /* Hack to cleanup cached value */
    if (e->attrs->src->proto->proto == &proto_bgp)
      e->u.bgp.stale = -1;

#endif

    src = a->src;
  }
  else
    {
      e = NULL;
      src = old->attrs->src;
    }

#if 0 // TODO: we dont really need to update destination table (channel) for msg pipe ... do we ?
  src_ch->table->msgpipe_busy = 1;
  rte_update2(dst, n->n.addr, e, src);
  src_ch->table->msgpipe_busy = 0;
#endif

  /* get routing info only when updates from main channel */
  if (p->conn)
  {
    if (src_ch == p->pri)
    {
      byte data[MAX_SIZE] = {0};
      int written = 0;
      eattr *newnhattr = NULL, *oldnhattr = NULL, *bestnhattr = NULL;
      ip_addr newnhip = {0}, oldnhip = {0}, bestnhip = ipa_is_ip4(net_prefix(n->n.addr)) ? IPA_NONE4 : IPA_NONE6;
      enum {add, del, rem} which = add;
      // char ADD[] = "add", DEL[] = "del", REM[] = "rem";
      const char * whichTxt[] = {"add", "del", "rem"};

      if (new)
      {
        // get BGP next hop address - get vlaue for new
        newnhattr = ea_find(new->attrs->eattrs, EA_CODE(PROTOCOL_BGP, BA_NEXT_HOP));
        if (newnhattr != NULL)
          newnhip = *(ip_addr *)newnhattr->u.ptr->data;

        // get BGP next hop address - get vlaue for best
        bestnhattr = ea_find(n->routes->attrs->eattrs, EA_CODE(PROTOCOL_BGP, BA_NEXT_HOP));
        if (bestnhattr != NULL)
          bestnhip = *(ip_addr *)bestnhattr->u.ptr->data;

        // written = bsnprintf(data, MAX_SIZE, "new %N -> %I, best -> %I", n->n.addr, newnhip, bestnhip);
        which = add;

        if (0) // testing how to convert to from ip_addr and net_addr to char *
        {
          byte td[MAX_SIZE] = {0};
          byte tdnet[20] = {0};
          byte tdip[20] = {0};
          int len = 0;
          written = bsnprintf(td, MAX_SIZE, "prefix: %N nexthop: %I", n->n.addr, newnhip);
          debug("special: '%s'\n", td);
          if (get_buf_prefix(td, tdnet, &len))
            debug("prefix: '%s/%d'\n", tdnet, len);
          if (get_buf_nexthop(td, tdip))
            debug("nexthop: '%s'\n", tdip);

          {
            struct msgpipe_message msg;

            net_addr_ip4 *ref = (net_addr_ip4 *)&msg.prefix;

            // ip4_pton(tdip, &msg.nexthop);
            // TODO: check this fix
            ip4_pton(tdip, (ip4_addr *)&msg.nexthop);

            ip4_pton(tdnet, &ref->prefix);
            // net_fill_ip4(&msg.prefix, ref->prefix, len);
            // TODO: check this fix
            net_fill_ip4((net_addr *)&msg.prefix, ref->prefix, len);

            // written = bsnprintf(td, MAX_SIZE, "prefix: %N nexthop: %I", msg.prefix, msg.nexthop);
            written = bsnprintf(td, MAX_SIZE, "nexthop: %I4", &msg.nexthop);
            debug("special2: '%s'\n", td);
            written = bsnprintf(td, MAX_SIZE, "prefix: %N", &msg.prefix);
            debug("special3: '%s'\n", td);
          }

        }
      }
      else if (old)
      {
        // get BGP next hop address - get vlaue for old
        oldnhattr = ea_find(old->attrs->eattrs, EA_CODE(PROTOCOL_BGP, BA_NEXT_HOP));
        if (oldnhattr != NULL)
          oldnhip = *(ip_addr *)oldnhattr->u.ptr->data;

        if (!n->routes) // no routes for prefix
        {
          // written = bsnprintf(data, MAX_SIZE, "rem %N ->%I", n->n.addr, oldnhip);
          which = rem;
        }
        else
        {
          // get BGP next hop address - get vlaue for best
          bestnhattr = ea_find(n->routes->attrs->eattrs, EA_CODE(PROTOCOL_BGP, BA_NEXT_HOP));
          if (bestnhattr != NULL)
            bestnhip = *(ip_addr *)bestnhattr->u.ptr->data;

          // written = bsnprintf(data, MAX_SIZE, "del %N ->%I, best -> %I", n->n.addr, oldnhip, bestnhip);
          which = del;
        }
      }

      if (p->pubDataName)
      {
        natsStatus s;
        log(L_DEBUG "update: %s\n", data);

        if (0) // use msgpack data
        {
          debug("%s\n", data);
          s = stanConnection_PublishAsync(p->conn, p->pubDataName, (const void *)data, written, _pubAckHandler, p);
          if (s != NATS_OK)
          {
            log(L_ERR "stanConnection_PublishAsync message failed: %s", NATS_STATUS_STR(s));
            //p->missed++;
            //p->total_missed++;
            p->msgPubErrored++;
          }
          else
          {
            //p->missed++;
            p->msgPubUpdates++;
          }
        }

        // msgpack for bgp attributes ...
#if 1
        {
          int count = 0;
          size_t mpwrite_size;
          mpack_error_t mperr;
          mpack_writer_t writer, *w = &writer;
          mpack_writer_init(&writer, mpackbuf, sizeof(mpackbuf)); //MPACK_BUF_MAXSIZE);

          if (0 != msgpack_bgp_attrs(new ? new : old, NULL, &count))
          {
              log(L_ERR "get_bgp_attrs failed to retrieve count");
                  return;
          }
          debug("count is: %d\n", count);
          // bgp attributes + change ...
          mpack_start_map(w, count+5);
          // tag begin
          mpack_write_cstr(w, "s");
          mpack_write_u32(w, 0xdeadbeaf);
          // prefix(network)
          mpack_write_cstr(w, "p");
          if (!net_is_ip(n->n.addr))
          {
            log(L_ERR "network is not ip address");
            debug("network is not ip address");
          }
          {
            ip_addr ip = net_prefix(n->n.addr);
            debug("attrs2(): type ip length %d\n", 4);
            mpack_start_array(w, 5);
            mpack_write_u32(w, net_pxlen(n->n.addr));
            for (int i = 0; i < 4; i++)
            {
              mpack_write_u32(w, ip.addr[i]);
              debug("attrs2(): type ip e[%d] %u\n", i, ip.addr[i]);
            }
            mpack_finish_array(w);
          }

          // put update type
          {
            mpack_write_cstr(w, "w");
            // add (update)
            if (new)
              mpack_write_u8(w, 0);
            // dele (update withdraw)
            else if (n->routes)
              mpack_write_u8(w, 1);
            // remove (update withdraw and no other routes)
            else 
              mpack_write_u8(w, 2);
          }
          // put best
          {
            mpack_write_cstr(w, "b");
            eattr *best = n->routes ? ea_find(n->routes->attrs->eattrs, EA_CODE(PROTOCOL_BGP, BA_NEXT_HOP)) : NULL;
            if (best)
            {
              ip_addr *ip = (ip_addr *)bestnhattr->u.ptr->data;
              debug("attrs2(): type ip length %d\n", 4);
              mpack_start_array(w, 4);
              for (int i = 0; i < 4; i++)
              {
                mpack_write_u32(w, ip->addr[i]);
                debug("attrs2(): type ip e[%d] %u\n", i, ip->addr[i]);
              }
              mpack_finish_array(w);
            }
            else
            {
              mpack_start_array(w, 4);
              mpack_write_u32(w, 0);
              mpack_write_u32(w, 0);
              mpack_write_u32(w, 0);
              mpack_write_u32(w, 0);
              mpack_finish_array(w);
            }
          }

          if (0 != msgpack_bgp_attrs(new ? new : old, w, NULL))
          {
            log(L_ERR "get_bgp_attrs failed to retrieve fields");
            mperr = mpack_writer_destroy(w);
            if (mperr != mpack_ok)
            {
              log(L_ERR "failed retrieval of bgp arrts - mpack destroy failed: %s", mpack_error_to_string(mperr));
            }
            return;
          }
          mpack_write_cstr(w, "e");
          mpack_write_u32(w, 0xbeafdead);
          mpack_finish_map(w);

          mpwrite_size = mpack_writer_buffer_used(w);
          mperr = mpack_writer_destroy(w);
          if (mperr != mpack_ok)
          {
            log(L_ERR "mpack destroy failed: %s", mpack_error_to_string(mperr));
            return;
          }
          s = stanConnection_PublishAsync(p->conn, p->pubDataName, (const void *)mpackbuf, mpwrite_size, _pubAckHandler, p);
          if (s != NATS_OK)
          {
            log(L_ERR "stanConnection_PublishAsync message failed: %s", NATS_STATUS_STR(s));
          }
        }
#endif
      }
    }
  }
}

static int
msgpipe_preexport(struct proto *P, rte **ee, struct linpool *p UNUSED)
{
  struct proto *pp = (*ee)->sender->proto;

  if (pp == P)
    return -1;	/* Avoid local loops automatically */

  return 0;
}

static void
msgpipe_reload_routes(struct channel *C)
{
  struct msgpipe_proto *p = (void *) C->proto;

  /* Route reload on one channel is just refeed on the other */
  channel_request_feeding((C == p->pri) ? p->sec : p->pri);
}


static void
msgpipe_postconfig(struct proto_config *CF)
{
  struct msgpipe_config *cf = (void *) CF;
  struct channel_config *cc = proto_cf_main_channel(CF);

  if (!cc->table)
    cf_error("Primary routing table not specified");

  if (!cf->peer)
    cf_error("Secondary routing table not specified");

  // if (!cf->pubName)
  //   cf_error("publish name no specified");

  // if (!cf->subName)
  //   cf_error("subscriber name no specified");

  if (cc->table == cf->peer)
    cf_error("Primary table and peer table must be different");

  if (cc->table->addr_type != cf->peer->addr_type)
    cf_error("Primary table and peer table must have the same type");

  if (cc->rx_limit.action)
    cf_error("MsgPipe protocol does not support receive limits");

  if (cc->in_keep_filtered)
    cf_error("MsgPipe protocol prohibits keeping filtered routes");

}

static int
msgpipe_configure_channels(struct msgpipe_proto *p, struct msgpipe_config *cf)
{
  struct channel_config *cc = proto_cf_main_channel(&cf->c);

  struct channel_config pri_cf = {
    .name = "pri",
    .channel = cc->channel,
    .table = cc->table,
    .out_filter = cc->out_filter,
    .in_limit = cc->in_limit,
    .ra_mode = RA_ANY
  };

  struct channel_config sec_cf = {
    .name = "sec",
    .channel = cc->channel,
    .table = cf->peer,
    .out_filter = cc->in_filter,
    .in_limit = cc->out_limit,
    .ra_mode = RA_ANY
  };

  return
    proto_configure_channel(&p->p, &p->pri, &pri_cf) &&
    proto_configure_channel(&p->p, &p->sec, &sec_cf);
}

static struct proto *
msgpipe_init(struct proto_config *CF)
{
  struct proto *P = proto_new(CF);
  struct msgpipe_proto *p = (void *) P;
  struct msgpipe_config *cf = (void *) CF;

  P->rt_notify = msgpipe_rt_notify;
  P->preexport = msgpipe_preexport;
  P->reload_routes = msgpipe_reload_routes;

  p->pubDataName = cf->pubDataName;
  p->pubControlName = cf->pubControlName;
  p->subName = cf->subName;

  msgpipe_configure_channels(p, cf);

  return P;
}

static int
msgpipe_reconfigure(struct proto *P, struct proto_config *CF)
{
  struct msgpipe_proto *p = (void *) P;
  struct msgpipe_config *cf = (void *) CF;

  return msgpipe_configure_channels(p, cf);
}

static volatile int ackCount = 0;
static volatile int errCount = 0;

static void
_pubAckHandler(const char *guid, const char *error, void *closure)
{
    struct msgpipe_proto *p = (struct msgpipe_proto *)closure;
    mploop_enter(p->loop);
  // This callback can be invoked by different threads for the
  // same connection, so access should be protected. For this
  // example, we don't.
  ackCount++;
  p->msgPubAcked++;
  if (error != NULL)
  {
    log(L_ERR "pub ack for guid:%s error=%s\n", guid, error);
    errCount++;
  }
  debug("message ack handler : ack: %ld, err: %ld\n", ackCount, errCount);
  // log(L_TRACE "message ack handler : ack: %ld, err: %ld\n", ackCount, errCount);

    mploop_leave(p->loop);
}

static void
onMsg(stanConnection *sc UNUSED, stanSubscription *sub, const char *channel, stanMsg *msg, void *closure)
{
  struct msgpipe_proto *p = (struct msgpipe_proto *)closure;

  event *ev;
  byte data[MAX_SIZE] = {0};
  int written = 0;

  struct msgpipe_message *msgp;
  
  ev = ev;

  // raise(SIGINT);
  debug("onMsg(): closure: %x, p: %x\n", closure, p);
  if (!closure || !p)
  {
    debug("msgpipe proto not ready");
    return;
  }
  mploop_enter(p->loop);
  debug("onMsg(): p->loop: %x\n", p->loop);

  written = bsnprintf(data, MAX_SIZE, "Received on [%s]: sequence:%" PRIu64 " data:%.*s timestamp:%" PRId64 " redelivered: %s\n",
                      channel,
                      stanMsg_GetSequence(msg),
                      stanMsg_GetDataLength(msg),
                      stanMsg_GetData(msg),
                      stanMsg_GetTimestamp(msg),
                      stanMsg_IsRedelivered(msg) ? "yes" : "no");

  log(L_DEBUG "data(%d): %s", written, data);
  //debug("data: %s\n", data);

  p->msgSubUpdates++;
  msgp = sl_alloc(p->msg_slab);

/*
  if (fill_msgp(msgp, stanMsg_GetData(msg)))
    update_msgp(p, msgp);
  else */if (fill2_msgp(msgp, stanMsg_GetData(msg), stanMsg_GetDataLength(msg)))
    update_msgp(p, msgp);
  else
    log(L_ERR "message failed to parse\n");

#if 0
  ev = mploop_new_event(p->loop, NULL, msgp);
  if (!ev)
  {
    log(L_ERR "onMsg(): failed to create new event\n");
    return;
  }
#endif

  sl_free(p->msg_slab, msgp);
  p->msgSubAcked++;
  mploop_leave(p->loop);
  stanSubscription_AckMsg(sub, msg);
  stanMsg_Destroy(msg);
}

static void
connectionLostCB(stanConnection *sc UNUSED, const char *errTxt, void *closure)
{
  struct msgpipe_proto *p = (struct msgpipe_proto *)closure;

if (!p->loop)
{
debug("connection lost: %s\n", errTxt);
return;
}
  mploop_enter(p->loop);
  log(L_ERR "stan Connection lost: %s", errTxt);
  p->connLost = true;
  mploop_leave(p->loop);
}

int
msgpipe_shutdown(struct proto *P)
{
  struct msgpipe_proto *p = (struct msgpipe_proto *)P;
  natsStatus s;

#if 1 // close subsciption
  if (p->subName)
  {
    s = stanSubscription_Unsubscribe(p->sub);
    if (s != NATS_OK)
    {
      log(L_ERR "stanSubscription_Unsubscribe failed: %s", NATS_STATUS_STR(s));
    }
    p->sub = NULL;
    p->msgSubUpdates = p->msgSubAcked = p->msgSubSkipped = 0;
  }
#endif

#if 1 // TODO: message to inform of shutdown ... revised ? 
  if (p->pubControlName)
  {
    const char *initialMessage = "{\"which\": \"stop\"}";
    int initialMessageLen = strlen(initialMessage);

    log(L_ERR "msgpipe pubname: %s", p->pubControlName);
    s = stanConnection_Publish(p->conn, p->pubControlName, (const void *)initialMessage, initialMessageLen);
    if (s != NATS_OK)
    {
      log(L_ERR "stanConnection_Publish initial message failed: %s", NATS_STATUS_STR(s));
      s = stanConnection_Close(p->conn);
      if (s != NATS_OK)
        log(L_ERR "stanConnOptions_Close failed: %s", NATS_STATUS_STR(s));
    }

    p->msgPubUpdates = p->msgPubAcked = p->msgPubErrored = 0;
  }
#endif
  
#if 1 // FIXME: closing connection here ?
  if (!p->connLost && p->conn != NULL)
  {
    log(L_INFO "msgpipe closing connection");
    natsStatus s = stanConnection_Close(p->conn);
    if (s != NATS_OK)
    {
      log(L_ERR "stanConnOptions_Close failed: %s", NATS_STATUS_STR(s));
    }
  }
  stanConnection_Destroy(p->conn);
  p->conn = NULL;
  // nats_Close();
#endif

  mploop_stop(p->loop);
  mploop_enter(p->loop);
  rfree(p->tpool);
  mploop_leave(p->loop);
  mploop_free(p->loop);
  p->loop = NULL;
  return PS_DOWN;
}


static bool
configure_nats_connection(struct proto *P)
{
  struct msgpipe_proto *p = (struct msgpipe_proto *)P;

  // TODO: move cluster and clientID to configuration
  const char *cluster = "test-cluster";
  const char *clientID = "client-bird";

  const char *initialMessage = "{\"which\":\"start\"}";
  int initialMessageLen = strlen(initialMessage);
  natsStatus s = 0;
  natsOptions *opts = NULL;
  stanConnOptions *connOpts = NULL;
  stanSubOptions *subOpts = NULL;
  bool stan_ok = false;
  do {
    do
    {
      // TODO: move to configuration ??

      s = stanConnOptions_Create(&connOpts);
      if (s != NATS_OK)
      {
        log(L_ERR "stanConnOptions_Create failed: %s", NATS_STATUS_STR(s));
        break;
      }

      s = natsOptions_Create(&opts);
      if (s != NATS_OK)
      {
        log(L_ERR "stanConnOptions_Create failed: %s", NATS_STATUS_STR(s));
        break;
      }

      s = stanConnOptions_SetNATSOptions(connOpts, opts);
      if (s != NATS_OK)
      {
        log(L_ERR "stanConnOptions_SetNATSOptions failed: %s", NATS_STATUS_STR(s));
        break;
      }
      s = stanConnOptions_SetConnectionLostHandler(connOpts, connectionLostCB, P);
      if (s != NATS_OK)
      {
        log(L_ERR "stanConnOptions_SetConnectionLostHandler failed: %s", NATS_STATUS_STR(s));
        break;
      }
      s = stanConnection_Connect(&p->conn, cluster, clientID, connOpts);
      if (s != NATS_OK)
      {
        log(L_ERR "stanConnection_Connect failed: %s", NATS_STATUS_STR(s));
        break;
      }
#if 1 // TODO: inform start message ... revised 
      if (p->pubControlName)
      {
        s = stanConnection_Publish(p->conn, p->pubControlName, (const void *)initialMessage, initialMessageLen);
        if (s != NATS_OK)
        {
          log(L_ERR "stanConnection_Publish initial message failed: %s", NATS_STATUS_STR(s));
          s = stanConnection_Close(p->conn);
          if (s != NATS_OK)
            log(L_ERR "stanConnOptions_Close failed: %s", NATS_STATUS_STR(s));

          break;
        }
        // force some time gap between control message of start to actual sending update messages
        nats_Sleep(200);
      }
      if (p->subName)
      {
        s = stanSubOptions_Create(&subOpts);
        if (s != NATS_OK)
        {
          log(L_ERR "stanSubOptions_Create failed: %s", NATS_STATUS_STR(s));
          break;
        }
        //s = stanSubOptions_DeliverAllAvailable(subOpts);
        //s = stanSubOptions_StartWithLastReceived(subOpts);
        s = stanSubOptions_StartAtTimeDelta(subOpts, 10000);
        if (s != NATS_OK)
        {
          log(L_ERR "stanSubOptions_DeliverAllAvailable failed: %s", NATS_STATUS_STR(s));
          break;
        }
        s = stanSubOptions_SetManualAckMode(subOpts, true);
        if (s != NATS_OK)
        {
          log(L_ERR "stanSubOptions_SetManualAckMode failed: %s", NATS_STATUS_STR(s));
          break;
        }
        s = stanSubOptions_SetAckWait(subOpts, 1000);
        if (s != NATS_OK)
        {
          log(L_ERR "stanSubOptions_SetAckWait failed: %s", NATS_STATUS_STR(s));
          break;
        }
        debug("configure_nats_connection(): cbClosure: %x\n", p);
        s = stanConnection_Subscribe(&p->sub, p->conn, p->subName,
                                     onMsg, p, subOpts);
        if (s != NATS_OK)
        {
          log(L_ERR "stanConnection_Subscribe on %s failed: %s", p->subName, NATS_STATUS_STR(s));
          break;
        }
      }
#endif
      stan_ok = true;
    } while (0);
    if (!stan_ok)
    {
      log(L_ERR "setting up stan client failed");
      p->conn = NULL;
    }
    if (opts != NULL)
      natsOptions_Destroy(opts);
    if (subOpts != NULL)
      stanSubOptions_Destroy(subOpts);
    if (connOpts != NULL)
      stanConnOptions_Destroy(connOpts);

  } while (0);

  return p->conn == NULL ? false : true;
}

int
msgpipe_start(struct proto *P)
{
  struct msgpipe_proto *p = (struct msgpipe_proto *)P;

  p->loop = mploop_new();
  debug("msgpipe_start(): p->loop: %x\n", p->loop);
  p->tpool = rp_new(NULL, "msgpipe thread root");
  pthread_spin_init(&p->lock, PTHREAD_PROCESS_PRIVATE);
  p->msg_slab = sl_new(p->tpool, sizeof(struct msgpipe_message));

  mploop_start(p->loop);

  if (!configure_nats_connection(P))
    return PS_DOWN;

  return PS_UP;
}

static void
msgpipe_copy_config(struct proto_config *dest UNUSED, struct proto_config *src UNUSED)
{
  /* Just a shallow copy, not many items here */
}

static void
msgpipe_get_status(struct proto *P, byte *buf)
{
  struct msgpipe_proto *p = (void *) P;

  bsprintf(buf, "%s <=> %s", p->pri->table->name, p->sec->table->name);
}

static void
msgpipe_show_stats(struct msgpipe_proto *p)
{
  struct proto_stats *s1 = &p->pri->stats;
  struct proto_stats *s2 = &p->sec->stats;

  /*
   * MsgPipe stats (as anything related to msgpipes) are a bit tricky. There
   * are two sets of stats - s1 for ahook to the primary routing and
   * s2 for the ahook to the secondary routing table. The user point
   * of view is that routes going from the primary routing table to
   * the secondary routing table are 'exported', while routes going in
   * the other direction are 'imported'.
   *
   * Each route going through a msgpipe is, technically, first exported
   * to the msgpipe and then imported from that msgpipe and such operations
   * are counted in one set of stats according to the direction of the
   * route propagation. Filtering is done just in the first part
   * (export). Therefore, we compose stats for one directon for one
   * user direction from both import and export stats, skipping
   * immediate and irrelevant steps (exp_updates_accepted,
   * imp_updates_received, imp_updates_filtered, ...).
   *
   * Rule of thumb is that stats s1 have the correct 'polarity'
   * (imp/exp), while stats s2 have switched 'polarity'.
   */

  cli_msg(-1006, "  Routes:         %u imported, %u exported",
	  s1->imp_routes, s2->imp_routes);
  cli_msg(-1006, "  Route change stats:     received   rejected   filtered    ignored   accepted");
  cli_msg(-1006, "    Import updates:     %10u %10u %10u %10u %10u",
	  s2->exp_updates_received, s2->exp_updates_rejected + s1->imp_updates_invalid,
	  s2->exp_updates_filtered, s1->imp_updates_ignored, s1->imp_updates_accepted);
  cli_msg(-1006, "    Import withdraws:   %10u %10u        --- %10u %10u",
	  s2->exp_withdraws_received, s1->imp_withdraws_invalid,
	  s1->imp_withdraws_ignored, s1->imp_withdraws_accepted);
  cli_msg(-1006, "    Export updates:     %10u %10u %10u %10u %10u",
	  s1->exp_updates_received, s1->exp_updates_rejected + s2->imp_updates_invalid,
	  s1->exp_updates_filtered, s2->imp_updates_ignored, s2->imp_updates_accepted);
  cli_msg(-1006, "    Export withdraws:   %10u %10u        --- %10u %10u",
	  s1->exp_withdraws_received, s2->imp_withdraws_invalid,
	  s2->imp_withdraws_ignored, s2->imp_withdraws_accepted);
}

static const char *msgpipe_feed_state[] = { [ES_DOWN] = "down", [ES_FEEDING] = "feed", [ES_READY] = "up" };

static void
msgpipe_show_proto_info(struct proto *P)
{
  struct msgpipe_proto *p = (void *) P;

  cli_msg(-1006, "  Channel %s", "main");
  cli_msg(-1006, "    Table:          %s", p->pri->table->name);
  cli_msg(-1006, "    Peer table:     %s", p->sec->table->name);
  cli_msg(-1006, "    Import state:   %s", msgpipe_feed_state[p->sec->export_state]);
  cli_msg(-1006, "    Export state:   %s", msgpipe_feed_state[p->pri->export_state]);
  cli_msg(-1006, "    Import filter:  %s", filter_name(p->sec->out_filter));
  cli_msg(-1006, "    Export filter:  %s", filter_name(p->pri->out_filter));
  cli_msg(-1006, "    Nats pub updates:  %10u", p->msgPubUpdates);
  cli_msg(-1006, "    Nats pub acked:    %10u", p->msgPubAcked);
  cli_msg(-1006, "    Nats pub errored:  %10u", p->msgPubErrored);
  cli_msg(-1006, "    Nats sub updates:  %10u", p->msgSubUpdates);
  cli_msg(-1006, "    Nats sub acked:    %10u", p->msgSubUpdates);
  cli_msg(-1006, "    Nats sub sckipped: %10u", p->msgSubSkipped);

  channel_show_limit(&p->pri->in_limit, "Import limit:");
  channel_show_limit(&p->sec->in_limit, "Export limit:");

  if (P->proto_state != PS_DOWN)
    msgpipe_show_stats(p);
}


struct protocol proto_msgpipe = {
  .name =		"MsgPipe",
  .template =		"msgpipe%d",
  .class =		PROTOCOL_MSGPIPE,
  .proto_size =		sizeof(struct msgpipe_proto),
  .config_size =	sizeof(struct msgpipe_config),
  .postconfig =		msgpipe_postconfig,
  .init =		msgpipe_init,
  .start =      msgpipe_start,
  .shutdown =      msgpipe_shutdown,
  .reconfigure =	msgpipe_reconfigure,
  .copy_config = 	msgpipe_copy_config,
  .get_status = 	msgpipe_get_status,
  .show_proto_info = 	msgpipe_show_proto_info
};
