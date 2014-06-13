/*
** Copyright 2005-2012  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/**************************************************************************\
*//*! \file
** <L5_PRIVATE L5_SOURCE>
** \author  djr
**  \brief  TCP sendmsg() etc.
**   \date  2003/09/02
**    \cop  (c) Level 5 Networks Limited.
** </L5_PRIVATE>
*//*
\**************************************************************************/

/*! \cidoxg_lib_transport_ip */

#include "ip_internal.h"

#if !defined(__KERNEL__)
#include <sys/socket.h>
#include <onload/extensions_zc.h>
#endif

#include <onload/pkt_filler.h>

#define LPF "TCP SEND "


#if defined(__KERNEL__) && defined(__linux__)
# define OO_EINTR  ERESTARTSYS
#else
# define OO_EINTR  EINTR
#endif


/* If not locked then trylock, and if successful set locked flag and (in
 * some cases) increment the counter.  Return true if lock held, else
 * false.  si_ variants take a [struct udp_send_info*].
 */

#define trylock(ni, locked)                                     \
  ((locked) || (ci_netif_trylock(ni) && ((locked) = 1)))
#define si_trylock(ni, sinf)                    \
  trylock((ni), (sinf)->stack_locked)

struct tcp_send_info {
  int rc;
  ci_uint32 timeout;
  ci_uint64 start_frc;
  int set_errno;
  int stack_locked;
  int total_unsent;
  int total_sent;
  int n_needed;
  int n_filled;
  int fill_list_bytes;
  unsigned tcp_send_spin;
  ci_ip_pkt_fmt* fill_list;
  struct oo_pkt_filler pf;
};


static void ci_tcp_tx_advance_nagle(ci_netif* ni, ci_tcp_state* ts)
{
  /* Nagle's algorithm (rfc896).  Summary: when user pushes data, don't
  ** send it if there is less than an MSS and we have unacknowledged data
  ** in the network.  Exceptions: we do want to push SYN/FINs, and we must
  ** push urgent data.
  */
  ci_ip_pkt_queue* sendq = &ts->send;
  ci_ip_pkt_fmt* pkt = NULL;

  ci_assert(ci_ip_queue_is_valid(ni, sendq, CI_TRUE));
  ci_assert(! ci_ip_queue_is_empty(sendq));

  if( (sendq->num != 1) | (ci_tcp_inflight(ts) == 0) |
      OO_SP_NOT_NULL(ts->s.local_peer)) {
  advance_now:
    /* NB. We call advance() before poll() to get best latency. */
    ci_ip_time_resync(IPTIMER_STATE(ni));
    ci_tcp_tx_advance(ts, ni);
    goto poll_and_out;
  }

  /* There can't be a SYN, because connection is established, so the SYN
  ** must already be acked.  There can't be a FIN, because if there was
  ** tx_errno would be non zero, and we would not have attempted to
  ** enqueue data.
  */
  pkt = PKT_CHK(ni, sendq->head);
  ci_assert(!(TX_PKT_TCP(pkt)->tcp_flags & (CI_TCP_FLAG_SYN|CI_TCP_FLAG_FIN)));

  if( (PKT_TCP_TX_SEQ_SPACE(pkt) >= tcp_eff_mss(ts)) |
      (SEQ_LT(tcp_snd_una(ts), tcp_snd_up(ts))     ) )
    goto advance_now;

  if( ts->s.s_aflags & CI_SOCK_AFLAG_NODELAY ) {
    /* With nagle off it is possible for a sender to push zillions of tiny
     * packets onto the network, which consumes loads of memory.  To
     * prevent this we choose not to advance if many packets are already
     * inflight, and on average they are less than half full.  This
     * behaviour can be disabled by setting [nonagle_inflight_max] to a
     * large value.
     */
    if( ts->retrans.num < NI_OPTS(ni).nonagle_inflight_max ||
        (ts->eff_mss * ts->retrans.num < ci_tcp_inflight(ts) * 2) )
      goto advance_now;
  }

  LOG_TV(log(LPF "%d Nagle snd=%08x-%08x-%08x enq=%08x pkt=%x-%x",
             S_FMT(ts), tcp_snd_una(ts), tcp_snd_nxt(ts),
             ts->snd_max, tcp_enq_nxt(ts),
             pkt->pf.tcp_tx.start_seq, pkt->pf.tcp_tx.end_seq));
  ++ts->stats.tx_stop_nagle;

 poll_and_out:
  if( ci_netif_may_poll(ni) && ci_netif_has_event(ni) )
    ci_netif_poll(ni);
}


static 
int ci_tcp_sendmsg_fill_pkt(ci_netif* ni, struct tcp_send_info* sinf,
                            ci_iovec_ptr* piov, int hdrlen,
                            int maxlen
                            CI_KERNEL_ARG(ci_addr_spc_t addr_spc))
{
  /* Initialise and fill a packet buffer from an iovec. */
  int n;
  ci_ip_pkt_fmt* pkt = oo_pkt_filler_next_pkt(ni, &sinf->pf);

  ci_assert(! ci_iovec_ptr_is_empty_proper(piov));
  ci_tcp_tx_pkt_init(pkt, hdrlen, maxlen);
  oo_pkt_filler_init(&sinf->pf, pkt, oo_tx_ether_data(pkt) + hdrlen);

#ifndef NDEBUG
  ci_assert_equal(pkt->n_buffers, 1);
  ci_assert_equal(pkt->buf_len, TX_PKT_LEN(pkt));
#endif

  n = sinf->total_unsent - sinf->fill_list_bytes;
  n = CI_MIN(maxlen, n);
  sinf->rc = oo_pkt_fill(ni, NULL, &sinf->pf, piov, n CI_KERNEL_ARG(addr_spc));
  if( CI_UNLIKELY(oo_pkt_fill_failed(sinf->rc)) ) 
    goto fill_failed;

  /* This assumes that packet filler only used a single buffer.
   * offbuf use on the TCP send path needs to go long term 
   */
  ci_assert_ge(oo_offbuf_left(&pkt->buf), n);
  oo_offbuf_advance(&pkt->buf, n);

  /* We should have either filled the segment, or run out of data. */
  LOG_TV(log("%s: iov.len=%d iovlen=%d n=%d pkt=%d left=%d", __FUNCTION__,
             (int) CI_IOVEC_LEN(&piov->io), piov->iovlen, n,
             OO_PKT_FMT(pkt), oo_offbuf_left(&pkt->buf)));
#ifndef __KERNEL__
  /* This can fail in the kernel due to bad user-level pointer, so
     can't assert this */
  ci_assert(ci_iovec_ptr_is_empty_proper(piov) ||
            oo_offbuf_left(&pkt->buf) == 0 ||
            pkt->n_buffers == CI_IP_PKT_SEGMENTS_MAX);
#else
# ifndef NDEBUG
  if(!(ci_iovec_ptr_is_empty_proper(piov) ||
       oo_offbuf_left(&pkt->buf) == 0 ||
       pkt->n_buffers == CI_IP_PKT_SEGMENTS_MAX))
    LOG_U(ci_log("%s: couldn't copy data, probably bad user-level pointer",
                 __FUNCTION__));
# endif
#endif

  /* We must remember the header length the packet was initialised with, and
  ** the amount of data we added.  The sequence number fields are a reasonable
  ** place for this, as they have to be fixed up when the packet is moved from
  ** the prequeue to the send queue in any case.
  */
  pkt->pf.tcp_tx.end_seq = n;

  ci_assert_equal(TX_PKT_LEN(pkt),
                  oo_offbuf_ptr(&pkt->buf) - PKT_START(pkt));
  return n;

 fill_failed:
  LOG_U(ci_log("%s: fill failed\n", __FUNCTION__));
  ci_assert(0);
  return 0;
}


int ci_tcp_fill_stolen_buffer(ci_netif* ni, ci_ip_pkt_fmt* pkt,
                              ci_iovec_ptr* piov 
                              CI_KERNEL_ARG(ci_addr_spc_t addr_spc))
{
  /* Fill a single packet, which must be initialised already (and may
  ** contain data), from an iovec.  Used for the "stolen packet" case.
  */
  int n;

  n = ci_ip_copy_pkt_from_piov(ni, pkt, piov, addr_spc);

  /* We should have either filled the segment, or run out of data. */
  LOG_TV(log("%s: iov.len=%d iovlen=%d n=%d pkt=%d left=%d", __FUNCTION__,
             (int) CI_IOVEC_LEN(&piov->io), piov->iovlen, n,
             OO_PKT_FMT(pkt), oo_offbuf_left(&pkt->buf)));
#ifndef __KERNEL__ 
  /* This can fail in the kernel due to bad user-level pointer, so
     can't assert this */
  ci_assert(ci_iovec_ptr_is_empty(piov) ||
            oo_offbuf_left(&pkt->buf) == 0 ||
            pkt->n_buffers == CI_IP_PKT_SEGMENTS_MAX);
#else
# ifndef NDEBUG
  if(!(ci_iovec_ptr_is_empty(piov) ||
       oo_offbuf_left(&pkt->buf) == 0 ||
       pkt->n_buffers == CI_IP_PKT_SEGMENTS_MAX))
    LOG_U(ci_log("%s: couldn't copy data, probably bad user-level pointer",
                 __FUNCTION__));
# endif
#endif
  /* Fixup the packet meta-data. */
  pkt->pf.tcp_tx.end_seq += n;

  return n;
}


static
void ci_tcp_tx_fill_sendq_tail(ci_netif* ni, ci_tcp_state* ts,
                               ci_iovec_ptr* piov, 
                               struct tcp_send_info* sinf
                               CI_KERNEL_ARG(ci_addr_spc_t addr_spc))
{
  ci_ip_pkt_queue* sendq = &ts->send;
  ci_ip_pkt_fmt* pkt;
  int n;

  if( ci_ip_queue_not_empty(sendq) && ts->s.tx_errno == 0 ) {
    pkt = PKT_CHK(ni, sendq->tail);
    if( oo_offbuf_left(&pkt->buf) > 0 ) {
      n = ci_tcp_fill_stolen_buffer(ni, pkt, piov  CI_KERNEL_ARG(addr_spc));
      LOG_TV(ci_log("%s: "NT_FMT "sq=%d if=%d bytes=%d piov.left=%d "
                    "pkt.left=%d", __FUNCTION__, NT_PRI_ARGS(ni, ts),
                    SEQ_SUB(tcp_enq_nxt(ts), tcp_snd_nxt(ts)),
                    ci_tcp_inflight(ts), n, ci_iovec_ptr_bytes_count(piov),
                    oo_offbuf_left(&pkt->buf)));
      tcp_enq_nxt(ts) += n;
      sinf->total_sent += n;
      sinf->total_unsent -= n;
    }

    /* The fact that there is something in the send queue means that it
    ** is being advanced.  So there is really no point whatsoever in us
    ** attempting to advance the send queue now.  If it could have been
    ** advanced further, it already would have.  We just need to poll
    ** (which may cause the data to go out...not our problem).  This is
    ** nagle compliant!
    */
  }
}


ci_inline void ci_tcp_sendmsg_prep_pkt(ci_netif* ni, ci_tcp_state* ts,
                                       ci_ip_pkt_fmt* pkt, unsigned seq)
{
  int orig_hdrlen, extra_opts;

  /* Copy in the headers */
  ci_pkt_init_from_ipcache(pkt, &ts->s.pkt);

  /* Recover the original header length that we initialised the packet with,
  ** before we correct the sequence numbers (we stashed it away in [start_seq]
  ** when the buffer was filled).
  */
  orig_hdrlen = (int)pkt->pf.tcp_tx.start_seq;

  /* Sequence numbers in packet are 0...n, so we need to fix them up.
  ** (Note that, in the stolen packet case, the sequence numbers are OK and
  ** <n> was set earlier.)
  */
  pkt->pf.tcp_tx.start_seq = seq;
  pkt->pf.tcp_tx.end_seq += seq;

  LOG_TV(log(LPF "%s: %d: %x-%x", __FUNCTION__, OO_PKT_FMT(pkt),
             pkt->pf.tcp_tx.start_seq, pkt->pf.tcp_tx.end_seq));

  /* It's possible that we thought we didn't need space for TCP options when
  ** the buffer was initialised, but now it turns out that we do.  (The dup
  ** tester can send from one thread to a socket that is still in the middle of
  ** being connected from another thread: when this happens there is a race
  ** condition between connection setup and ci_tcp_sendmsg().  Note that no
  ** sane app would do this!)  So, if the setting we saved away on buffer
  ** intialisation does not match the current setting, the packet must be fixed
  ** up.
  */
  extra_opts = ts->outgoing_hdrs_len - orig_hdrlen;
  if( extra_opts )
    ci_tcp_tx_insert_option_space(ni, ts, pkt, 
                                  orig_hdrlen + oo_ether_hdr_size(pkt),
                                  extra_opts);

  /* The sequence space consumed should match the bytes in the buffer. */
  ci_assert_equal((oo_offbuf_ptr(&pkt->buf) -
                   (PKT_START(pkt) + oo_ether_hdr_size(pkt) +
                    sizeof(ci_ip4_hdr) +
                    sizeof(ci_tcp_hdr) + CI_TCP_HDR_OPT_LEN(TX_PKT_TCP(pkt)))),
                  SEQ_SUB(pkt->pf.tcp_tx.end_seq,pkt->pf.tcp_tx.start_seq));

  /* Correct offbuf end as might have been constructed with diff eff_mss */
  ci_tcp_tx_pkt_set_end(ts, pkt);
}


void ci_tcp_sendmsg_prep_pkt_ool(ci_netif* ni, ci_tcp_state* ts,
                                 ci_ip_pkt_fmt* pkt, unsigned seq)
{
  ci_tcp_sendmsg_prep_pkt(ni, ts, pkt, seq);
}


static void ci_tcp_sendmsg_enqueue(ci_netif* ni, ci_tcp_state* ts,
                                   ci_ip_pkt_fmt* reverse_list,
                                   int total_bytes)
{
  ci_ip_pkt_queue* sendq = &ts->send;
  unsigned seq = tcp_enq_nxt(ts) + total_bytes;
  oo_pkt_p tail_pkt_id = OO_PKT_P(reverse_list);
  oo_pkt_p send_list = OO_PP_NULL;
  ci_ip_pkt_fmt* pkt;
  int n_pkts = 0;

  ci_assert(ci_netif_is_locked(ni));
  ci_assert_equal(ts->s.tx_errno, 0);

  do {
    pkt = reverse_list;
    reverse_list = (ci_ip_pkt_fmt *)CI_USER_PTR_GET(pkt->pf.tcp_tx.next);

    seq -= pkt->pf.tcp_tx.end_seq;
    ci_tcp_sendmsg_prep_pkt(ni, ts, pkt, seq);

    pkt->next = send_list;
    send_list = OO_PKT_P(pkt);
    ++n_pkts;
  }
  while( reverse_list );

  ci_assert_equal(tcp_enq_nxt(ts), seq);
  tcp_enq_nxt(ts) += total_bytes;

  /* Append these packets to the send queue. */
  ni->state->n_async_pkts -= n_pkts;
  sendq->num += n_pkts;
  ts->send_in += n_pkts;
  if( OO_PP_IS_NULL(sendq->head) )
    sendq->head = send_list;
  else
    PKT_CHK(ni, sendq->tail)->next = send_list;
  sendq->tail = tail_pkt_id;

  LOG_TV(ci_log("%s: "NT_FMT "sendq.num=%d enq_nxt=%x",
                __FUNCTION__, NT_PRI_ARGS(ni, ts),
                sendq->num, tcp_enq_nxt(ts)));
  CHECK_TS(ni, ts);
}


void ci_tcp_tx_prequeue(ci_netif* ni, ci_tcp_state* ts,
                        ci_ip_pkt_fmt* fill_list)
{
  ci_ip_pkt_fmt* next;
  ci_ip_pkt_fmt* pkt;
  int n_pkts = 0;

  /* Walk the fill_list to convert pointers to indirected pointers. */
  pkt = fill_list;
  while( 1 ) {
    ++n_pkts;
    if( ! (next = CI_USER_PTR_GET(pkt->pf.tcp_tx.next)) )  break;
    pkt->next = OO_PKT_P(next);
    pkt = next;
  }

  oo_atomic_add(&ts->send_prequeue_in, n_pkts);

  /* Put [fill_list] onto the prequeue. */
  do
    OO_PP_INIT(ni, pkt->next, ts->send_prequeue);
  while( ci_cas32_fail(&ts->send_prequeue,
                       OO_PP_ID(pkt->next), OO_PKT_ID(fill_list)) );
}


void ci_tcp_sendmsg_enqueue_prequeue(ci_netif* ni, ci_tcp_state* ts)
{
  ci_ip_pkt_queue* sendq = &ts->send;
  ci_ip_pkt_fmt* pkt;
  oo_pkt_p tail_pkt_id, send_list, id;
  int bytes, n_pkts = 0;
  ci_assert(ci_netif_is_locked(ni));
  ci_assert(ts->s.tx_errno == 0);

  /* Grab the contents of the prequeue atomically. */
  do {
    OO_PP_INIT(ni, id, ts->send_prequeue);
    if( OO_PP_IS_NULL(id) )  return;
  } while( ci_cas32_fail(&ts->send_prequeue, OO_PP_ID(id), OO_PP_ID_NULL) );

  /* Reverse the list. */
  send_list = OO_PP_NULL;
  do {
    pkt = PKT_CHK(ni, id);
    id = pkt->next;
    pkt->next = send_list;
    send_list = OO_PKT_P(pkt);
    ++n_pkts;
  }
  while( OO_PP_NOT_NULL(id) );

  /* Prep each packet. */
  while( 1 ) {
    bytes = pkt->pf.tcp_tx.end_seq;
    ci_tcp_sendmsg_prep_pkt_ool(ni, ts, pkt, tcp_enq_nxt(ts));
    if( pkt->flags & CI_PKT_FLAG_TX_PSH )
      TX_PKT_TCP(pkt)->tcp_flags |= CI_TCP_FLAG_PSH;
    tcp_enq_nxt(ts) += bytes;

    if( OO_PP_IS_NULL(pkt->next) )  break;
    pkt = PKT_CHK(ni, pkt->next);
  }

  /* Append onto the sendq. */
  ni->state->n_async_pkts -= n_pkts;
  sendq->num += n_pkts;
  /* NB do not update ts->send_in here, as that does not include
   * things added via prequeue
   */
  tail_pkt_id = OO_PKT_P(pkt);
  if( OO_PP_IS_NULL(sendq->head) ) {
    sendq->head = send_list;
    pkt = PKT_CHK(ni, send_list);
  }
  else {
    pkt = PKT_CHK(ni, sendq->tail);
    pkt->next = send_list;
  }
  sendq->tail = tail_pkt_id;

  /* Merge small segments if we can.  We only copy data (ie. we won't move
  ** data here, so we won't get optimal packing.  This is a trade-off
  ** against cpu overhead. */
  while( OO_PP_NOT_NULL(pkt->next) ) {
    ci_ip_pkt_fmt* next = PKT_CHK(ni, pkt->next);
    if( oo_offbuf_left(&pkt->buf) >= PKT_TCP_TX_SEQ_SPACE(next) ) {
      LOG_TT(ci_log("%s: coalesce %d (bytes=%d) into %d (space=%d)",
		    __FUNCTION__, OO_PKT_FMT(next), PKT_TCP_TX_SEQ_SPACE(next),
		    OO_PKT_FMT(pkt), oo_offbuf_left(&pkt->buf)));
      ci_tcp_tx_coalesce(ni, ts, sendq, pkt, CI_TRUE);
      if( ! OO_PP_EQ(pkt->next, OO_PKT_P(next)) )  continue;
      if( OO_PP_IS_NULL(pkt->next) )  break;
      /* Didn't coalesce, presumably because we ran out of segments or
      ** something. */
      pkt = PKT_CHK(ni, pkt->next);
    }
    else
      pkt = next;
  }
}


static int ci_tcp_sendmsg_free_pkt_list(ci_netif* ni, ci_tcp_state* ts,
                                        oo_pkt_p pkt_list, int netif_locked,
                                        int check_aop)
{
  /* NB. Packets must be "asynchronous".  That is, accounted for in
   * [n_async_pkts].
   */
  ci_ip_pkt_fmt* pkt;
  int n_pkts = 0;

  ci_assert(OO_PP_NOT_NULL(pkt_list));
  ci_assert( ! netif_locked || ci_netif_is_locked(ni));

  if( ! netif_locked && ! ci_netif_trylock(ni) ) {
    do {
      pkt = PKT_NNL(ni, pkt_list);
      pkt_list = pkt->next;
      /* ?? TODO: cope with these cases */
      ci_assert_equal(pkt->refcount, 1);
      ci_assert(!(pkt->flags & CI_PKT_FLAG_RX));
      pkt->refcount = 0;
      __ci_netif_pkt_clean(pkt);
      ci_netif_pkt_free_nonb_list(ni, OO_PKT_P(pkt), pkt);
      ++n_pkts;
    } while( OO_PP_NOT_NULL(pkt_list) );
  }
  else {
    do {
      pkt = PKT_CHK(ni, pkt_list);
      pkt_list = pkt->next;
      ci_netif_pkt_release_1ref(ni, pkt);
      ++n_pkts;
    } while( OO_PP_NOT_NULL(pkt_list) );
    ni->state->n_async_pkts -= n_pkts;
    if( ! netif_locked )  ci_netif_unlock(ni);
  }

  return n_pkts;
}


/* Convert linked list using pointers to linked list using indirection.
 * Also, set pf.tcp_tx.aop_id to -1 -- ci_tcp_sendmsg_free_pkt_list()
 * needs it. */
static void ci_netif_pkt_convert_ptr_list(ci_netif* ni, ci_ip_pkt_fmt* list)
{
  ci_ip_pkt_fmt* next;
  while( CI_USER_PTR_GET(list->pf.tcp_tx.next) ) {
    next = (ci_ip_pkt_fmt*) CI_USER_PTR_GET(list->pf.tcp_tx.next);
    list->next = OO_PKT_P(next);
    list = next;
  }
  list->next = OO_PP_NULL;
}


void ci_tcp_tx_free_prequeue(ci_netif* ni, ci_tcp_state* ts,
                             int netif_locked)
{
  int n_pkts;
  oo_pkt_p id;

  ci_assert( ! netif_locked || ci_netif_is_locked(ni));

  /* Grab contents of prequeue atomically.  We might not be the only thread
  ** trying to free it! */
  do {
    OO_PP_INIT(ni, id, ts->send_prequeue);
    if( OO_PP_IS_NULL(id) )  return;
  } while( ci_cas32_fail(&ts->send_prequeue, OO_PP_ID(id), OO_PP_ID_NULL) );

  n_pkts = ci_tcp_sendmsg_free_pkt_list(ni, ts, id, netif_locked, 1);

  oo_atomic_add(&ts->send_prequeue_in, -n_pkts);
}


void ci_tcp_sendmsg_enqueue_prequeue_deferred(ci_netif* ni, ci_tcp_state* ts)
{
  ci_assert(ci_netif_is_locked(ni));

  if( ts->s.tx_errno ) {
    /* Ooops... an error occurred while the lock holder had the lock.  So
    ** we shouldn't attempt to do anything, except free up the prequeue.
    */
    LOG_TC(log("%s: "NTS_FMT "tx_errno=%d", __FUNCTION__,
               NTS_PRI_ARGS(ni, ts), ts->s.tx_errno));
    ci_tcp_tx_free_prequeue(ni, ts, CI_MSG_NETIF_LOCKED);
    return;
  }

  ci_tcp_sendmsg_enqueue_prequeue(ni, ts);

  if( ci_tcp_sendq_not_empty(ts) ) {
    /* This is called in the context of unlocking the netif, so it is highly
    ** likely that the stack has been polled recently.  So we don't want to
    ** poll it here. */
    ci_tcp_tx_advance(ts, ni);

    /* This may have freed space in the send queue, so we may need to wake a
    ** sender. */
    if( ci_tcp_tx_advertise_space(ts) )
      ci_tcp_wake_not_in_poll(ni, ts, CI_SB_FLAG_WAKE_TX);
  }
}


ci_inline void ci_tcp_sendmsg_free_unused_pkts(ci_netif* ni, 
                                               struct tcp_send_info* sinf)
{
  oo_pkt_filler_free_unused_pkts(ni, &sinf->stack_locked, &sinf->pf);
}


static int ci_tcp_sendmsg_notsynchronised(ci_netif* ni, ci_tcp_state* ts, 
                                          int flags, struct tcp_send_info* sinf)
{
  sinf->rc = 1;
  /* The same sanity check is done in intercept. This one here is to make
  ** sure (whether needed or not) that internal calls are checked.
  */
  if( ts->s.b.state == CI_TCP_CLOSED )
    sinf->rc = 0;  /* use tx_errno */
  /* State must be SYN-SENT, but can change under our feet as we don't have
  ** the netif lock.  If non-blocking, return EAGAIN.
  */
  else if( flags & MSG_DONTWAIT )
    sinf->rc = -EAGAIN;

  if( sinf->rc <= 0 )
    return -1;

#define CONNECT_IN_PROGRESS ((ts->s.b.state == CI_TCP_SYN_SENT) && \
                             ts->s.tx_errno == 0)

  if( !sinf->stack_locked ) {
    if( (sinf->rc = ci_netif_lock(ni)) )
      return -1;
    sinf->stack_locked = 1;
  }
  CI_TCP_SLEEP_WHILE(ni, ts, CI_SB_FLAG_WAKE_RX, ts->s.so.rcvtimeo_msec, 
                     CONNECT_IN_PROGRESS, &sinf->rc);
  if( sinf->rc != 0 || ts->s.tx_errno != 0 )
    return -1;

  return 0;
}


static void ci_tcp_sendmsg_handle_rc_or_tx_errno(ci_netif* ni, 
                                                 ci_tcp_state* ts, 
                                                 int flags, 
                                                 struct tcp_send_info* sinf)
{
  sinf->set_errno = 0;

  if( sinf->rc ) {
    sinf->rc = -sinf->rc;
    sinf->set_errno = 1;
  }

  if( sinf->total_sent ) {
    sinf->rc = sinf->total_sent;
    sinf->set_errno = 0;
  }
  else {
    if( ts->s.so_error ) {
      ci_int32 rc1 = ci_get_so_error(&ts->s);
      if( rc1 != 0 ) {
        sinf->rc = rc1;
        sinf->set_errno = 1;
      }
    }
    if( sinf->rc == 0 && ts->s.tx_errno ) {
      LOG_TC(log(LNT_FMT "tx_errno=%d flags=%x total_sent=%d",
                 LNT_PRI_ARGS(ni, ts), ts->s.tx_errno, flags, sinf->total_sent));
      sinf->rc = ts->s.tx_errno;
      sinf->set_errno = 1;
    }
  }
  ci_tcp_sendmsg_free_unused_pkts(ni, sinf);
  if( sinf->stack_locked ) {
    ci_netif_unlock(ni);
    sinf->stack_locked = 0;
  }
}


static void ci_tcp_sendmsg_handle_zero_or_tx_errno(ci_netif* ni, 
                                                   ci_tcp_state* ts, 
                                                   int flags, 
                                                   struct tcp_send_info* sinf)
{
  sinf->rc = 0;
  return ci_tcp_sendmsg_handle_rc_or_tx_errno(ni, ts, flags, sinf);
}


static void ci_tcp_sendmsg_free_fill_list(ci_netif* ni, ci_tcp_state* ts,
                                          int flags, 
                                          struct tcp_send_info* sinf)
{
  if( sinf->fill_list ) {
    ci_netif_pkt_convert_ptr_list(ni, sinf->fill_list);
    ci_tcp_sendmsg_free_pkt_list(ni, ts, OO_PKT_P(sinf->fill_list), 
                                 sinf->stack_locked, 0);
  }
}


static void ci_tcp_sendmsg_handle_tx_errno(ci_netif* ni, ci_tcp_state* ts, 
                                           int flags, 
                                           struct tcp_send_info* sinf)
{
  ci_tcp_sendmsg_free_fill_list(ni, ts, flags, sinf);
  ci_tcp_sendmsg_free_unused_pkts(ni, sinf);
  ci_tcp_tx_free_prequeue(ni, ts, sinf->stack_locked);
  return ci_tcp_sendmsg_handle_zero_or_tx_errno(ni, ts, flags, sinf);
}


static void ci_tcp_sendmsg_handle_sent_or_rc(ci_netif* ni, ci_tcp_state* ts, 
                                             int flags, 
                                             struct tcp_send_info* sinf)
{
  ci_tcp_sendmsg_free_fill_list(ni, ts, flags, sinf);
  ci_tcp_sendmsg_free_unused_pkts(ni, sinf);
  if( sinf->stack_locked ) {
    ci_netif_unlock(ni);
    sinf->stack_locked = 0;
  }
  if( sinf->total_sent ) {
    sinf->rc = sinf->total_sent;
    sinf->set_errno = 0;
  }
  else {
    sinf->rc = -sinf->rc;
    sinf->set_errno = 1;
  }
}


static int ci_tcp_sendmsg_no_pkt_buf(ci_netif* ni, ci_tcp_state* ts, 
                                     int flags, struct tcp_send_info* sinf)
{
  ci_ip_pkt_fmt* pkt;
  do {
    pkt = ci_netif_pkt_alloc_nonb(ni, sinf->stack_locked);
    if( pkt ) 
      oo_pkt_filler_add_pkt(&sinf->pf, pkt);
    else
      break;
  } while( --sinf->n_needed > 0 );

  if( sinf->n_needed == 0 )
    return 0;
  else {
    CITP_STATS_NETIF_INC(ni, tcp_send_nonb_pool_empty);
    if( !si_trylock(ni, sinf) ) {
      if( sinf->n_filled )
        return 1;
      if( (sinf->rc = ci_netif_lock(ni)) != 0 ) {
        ci_tcp_sendmsg_handle_sent_or_rc(ni, ts, flags, sinf);
        return -1;
      }
      sinf->stack_locked = 1;
      CITP_STATS_NETIF_INC(ni, tcp_send_ni_lock_contends);
    }
    ci_assert(ci_netif_is_locked(ni));

    if( ! ci_netif_pkt_tx_may_alloc(ni) )
      /* Bring us up-to-date before calling
       * ci_netif_pkt_alloc_slow() else it might be provoked to
       * allocate more memory when none is needed.
       */
      ci_netif_poll(ni);
    
    while( 1 ) {
      ci_assert(ci_netif_is_locked(ni));
      do {
        pkt = ci_netif_pkt_tx_tcp_alloc(ni);
        if( pkt ) {
          /* We would have preferred to have gotten this from the non
           * blocking pool.  So arrange for it to be freed to that pool.
           */
          pkt->flags = CI_PKT_FLAG_NONB_POOL;
          ++ni->state->n_async_pkts;
          oo_pkt_filler_add_pkt(&sinf->pf, pkt);
        }
        else if( sinf->n_filled ) {
          /* If we've filled any packets, push them out before blocking. */
          return 1;
        } 
        else
          break;
      } while( --sinf->n_needed > 0 );

      if( sinf->n_needed == 0 )
        return 0;

      ci_assert(sinf->fill_list == 0);

      sinf->rc = ci_netif_pkt_wait(ni, sinf->stack_locked ? 
                                   CI_SLEEP_NETIF_LOCKED : 0);
      sinf->stack_locked = 0;
      if( ci_netif_pkt_wait_was_interrupted(sinf->rc) ) {
        ci_tcp_sendmsg_handle_sent_or_rc(ni, ts, flags, sinf);
        return -1;
      }
      do {
        pkt = ci_netif_pkt_alloc_nonb(ni, CI_FALSE);
        if( pkt ) 
          oo_pkt_filler_add_pkt(&sinf->pf, pkt);
        else
          break;
      } while( --sinf->n_needed > 0 );

      if( ts->s.tx_errno ) {
        ci_tcp_sendmsg_handle_tx_errno(ni, ts, flags, sinf);
        return -1;
      }

      if( sinf->n_needed == 0 )
        return 0;

      /* Start of loop expects lock to be held */
      ci_assert(sinf->stack_locked == 0);
      if( !si_trylock(ni, sinf) ) {
        if( (sinf->rc = ci_netif_lock(ni)) != 0 ) {
          ci_tcp_sendmsg_handle_sent_or_rc(ni, ts, flags, sinf);
          return -1;
        }
        sinf->stack_locked = 1;
        CITP_STATS_NETIF_INC(ni, tcp_send_ni_lock_contends);
      }
    }
  }
  /* Can't get here */
  ci_assert(0);
  return -1;
}


ci_inline int ci_tcp_sendmsg_spin(ci_netif* ni, ci_tcp_state* ts, 
                                  int flags, struct tcp_send_info* sinf)
{
  ci_uint64 now_frc;
  ci_uint64 schedule_frc;
  ci_uint64 max_spin = ni->state->spin_cycles;
  int spin_limit_by_so = 0;
#ifndef __KERNEL__
  citp_signal_info* si = citp_signal_get_specific_inited();
#endif

  ci_frc64(&now_frc);
  schedule_frc = now_frc;

  if( ts->s.so.sndtimeo_msec ) {
    ci_uint64 max_so_spin = (ci_uint64)ts->s.so.sndtimeo_msec *
      IPTIMER_STATE(ni)->khz;
    if( max_so_spin <= max_spin ) {
      max_spin = max_so_spin;
      spin_limit_by_so = 1;
    }
  }

  do {
    if( ci_netif_may_poll(ni) ) {
      if( ci_netif_need_poll_spinning(ni, now_frc) && si_trylock(ni, sinf) ) {
        ci_netif_poll_n(ni, NI_OPTS(ni).evs_per_poll);
        sinf->n_needed = ts->send_max - ci_tcp_sendq_n_pkts(ts);
        if( sinf->n_needed > 0 ) {
          ni->state->is_spinner = 0;
          return 0;
        }
        if( ts->s.tx_errno ) {
          ni->state->is_spinner = 0;
          ci_tcp_sendmsg_handle_tx_errno(ni, ts, flags, sinf);
          return -1;
        }
      }
      else if( ! ni->state->is_spinner )
        ni->state->is_spinner = 1;
    }
    if( sinf->stack_locked ) {
      ci_netif_unlock(ni);
      sinf->stack_locked = 0;
    }
    ci_frc64(&now_frc);
    sinf->rc = OO_SPINLOOP_PAUSE_CHECK_SIGNALS(ni, now_frc, schedule_frc, 
                                               ts->s.so.sndtimeo_msec,
                                               NULL, si);
    if( sinf->rc != 0 ) {
      ni->state->is_spinner = 0;
      ci_tcp_sendmsg_handle_sent_or_rc(ni, ts, flags, sinf);
      return -1;
    }
  } while( now_frc - sinf->start_frc < max_spin );
  ni->state->is_spinner = 0;

  if( spin_limit_by_so && now_frc - sinf->start_frc >= max_spin ) {
    sinf->rc = -EAGAIN;
    ci_tcp_sendmsg_handle_sent_or_rc(ni, ts, flags, sinf);
    return -1;
  }

  if( sinf->timeout ) {
    ci_uint32 time_spin = NI_OPTS(ni).spin_usec >> 10;
    if( time_spin >= sinf->timeout ) {
      sinf->rc = -EAGAIN;
      ci_tcp_sendmsg_handle_sent_or_rc(ni, ts, flags, sinf);
      return -1;
    }
    sinf->timeout -= time_spin;
  }
  return 1;
}
                                  


static int ci_tcp_sendmsg_block(ci_netif* ni, ci_tcp_state* ts,
                                int flags, struct tcp_send_info* sinf)
{
  ci_uint64 sleep_seq;

  CI_IP_SOCK_STATS_INC_TXSTUCK( ts );

  /* Record the current [sleep_seq] and check again to ensure we do a
   * race-free block.
   */
  sleep_seq = ts->s.b.sleep_seq.all;
  ci_rmb();
  if( ts->send_max - ci_tcp_sendq_n_pkts(ts) > 0 )
    return 0;
  if( ts->s.tx_errno ) {
    ci_tcp_sendmsg_handle_tx_errno(ni, ts, flags, sinf);
    return -1;
  }

  CI_IP_SOCK_STATS_INC_TXSLEEP( ts );


  sinf->rc = 
    ci_sock_sleep(ni, &ts->s.b, CI_SB_FLAG_WAKE_TX,
                  sinf->stack_locked ? CI_SLEEP_NETIF_LOCKED : 0,
                  sleep_seq, &sinf->timeout);
  /* ci_sock_sleep drops lock */
  sinf->stack_locked = 0;

  if( sinf->rc < 0 ) {
    ci_tcp_sendmsg_handle_sent_or_rc(ni, ts, flags, sinf);
    return -1;
  }

  if( ! ts->s.tx_errno ) 
    return 0;
  else {
    ci_tcp_sendmsg_handle_tx_errno(ni, ts, flags, sinf);
    return -1;
  }
}


static int ci_tcp_sendmsg_slowpath(ci_netif* ni, ci_tcp_state* ts, 
                                   const struct msghdr* msg,
                                   int flags, struct tcp_send_info* sinf
                                   CI_KERNEL_ARG(ci_addr_spc_t addr_spc))
{
  /* Set NO_TX_ADVANCE flag out here in order to ensure that
   * ci_tcp_sendmsg can't really push any packets out; all it can do
   * is enqueue packets.  Then we set [snd_up] to the correct value
   * before unsetting the flag. 
   *
   * The whole point is that ci_tcp_sendmsg() can proceed without giving a
   * damn about urgent data.
   */
  int rc;
  unsigned enq_nxt_before;
  
  if( !sinf->total_unsent ) {
    sinf->rc = 0;
    return -1;
  }

  ci_assert(flags & MSG_OOB);

  rc = ci_netif_lock(ni);
  if( rc != 0 ) {
    sinf->rc = rc;
    return -1;
  }
  
  /* Poll first, so we have an accurate view of space in the send queue. */
  ci_netif_poll(ni);
  
  /* Set the urgent pointer on the assumption that we're going to send
   * everything.  Also save the current enq_nxt; we need it below.  I
   * think this is only necessary to deal with the case where there
   * might be a concurrent send while we drop the netif lock.
   */
  tcp_snd_up(ts) = tcp_enq_nxt(ts) + sinf->total_unsent;
  enq_nxt_before = tcp_enq_nxt(ts);
  
  ts->tcpflags |= CI_TCPT_FLAG_NO_TX_ADVANCE;

  ci_netif_unlock(ni);

  sinf->rc = ci_tcp_sendmsg(ni, ts, msg, (flags &~ MSG_OOB) 
                            CI_KERNEL_ARG(addr_spc));
  
  rc = ci_netif_lock(ni);
  if( rc != 0 ) {
    /* If this happens (should only be from the kernel, which can't
     * set MSG_OOB at the moment) and we couldn't send it all, then
     * tcp_send_up() won't be set correctly.
     */
    sinf->rc = rc;
    return -1;
  }

  /* If there was a concurrent send that raced with this, then
   * enq_nxt_before and so tcp_snd_up() could be completely wrong.
   * Not worth worrying about.
   */

  if( sinf->rc > 0 ) {
    /* Correct tcp_send_up() in case where we didn't sent it all */
    tcp_snd_up(ts) = enq_nxt_before + sinf->rc;
    ts->tcpflags &= ~CI_TCPT_FLAG_NO_TX_ADVANCE;
    ci_tcp_tx_advance(ts, ni);
  }

  ci_netif_unlock(ni);
  return 0;
}


/*! \todo Confirm */

int ci_tcp_sendmsg(ci_netif* ni, ci_tcp_state* ts, const struct msghdr* msg,
                   int flags 
                   CI_KERNEL_ARG(ci_addr_spc_t addr_spc))
{
  ci_ip_pkt_queue* sendq = &ts->send;
  ci_ip_pkt_fmt* pkt;
  ci_iovec_ptr piov;
  int m;
  unsigned eff_mss;
  struct tcp_send_info sinf;

  ci_assert(msg != NULL);
  ci_assert(msg->msg_iov != NULL);
  ci_assert_gt(msg->msg_iovlen, 0);
  ci_assert(ts);
  ci_assert(ts->s.b.state != CI_TCP_LISTEN);

  sinf.rc = 0;
  /* This signifies if this function has the lock, not whether the
   * lock is held (as caller may hold it)
   */
  sinf.stack_locked = 0;
  sinf.total_unsent = 0;
  sinf.total_sent = 0;
  sinf.pf.alloc_pkt = NULL;
  sinf.timeout = ts->s.so.sndtimeo_msec;
#ifndef __KERNEL__
  sinf.tcp_send_spin = 
    oo_per_thread_get()->spinstate & (1 << ONLOAD_SPIN_TCP_SEND);
  if( sinf.tcp_send_spin )
    ci_frc64(&sinf.start_frc);
#else
  sinf.tcp_send_spin = 0;
#endif


  if(CI_UNLIKELY( (~ts->s.b.state & CI_TCP_STATE_SYNCHRONISED) ))
    goto not_synchronised;

 is_sync:
  for( m = 0; m < (int)msg->msg_iovlen; ++m ) {
    sinf.total_unsent += CI_IOVEC_LEN(&msg->msg_iov[m]);
    if(CI_UNLIKELY( CI_IOVEC_BASE(&msg->msg_iov[m]) == NULL &&
                    CI_IOVEC_LEN(&msg->msg_iov[m]) > 0 )) {
      sinf.rc = -EFAULT;
      ci_tcp_sendmsg_handle_rc_or_tx_errno(ni, ts, flags, &sinf);
      if( sinf.set_errno ) CI_SET_ERROR(sinf.rc, sinf.rc);
      return sinf.rc;
    }
  }

  if(CI_UNLIKELY( ! sinf.total_unsent || (flags & MSG_OOB) ))  goto slow_path;

  ci_iovec_ptr_init_nz(&piov, msg->msg_iov, msg->msg_iovlen);


  eff_mss = tcp_eff_mss(ts);
  ci_assert(eff_mss <=
            CI_MAX_ETH_DATA_LEN - sizeof(ci_tcp_hdr) - sizeof(ci_ip4_hdr));

  if( si_trylock(ni, &sinf) && ci_ip_queue_not_empty(sendq) ) {
    /* Usually, non-empty sendq means we do not have any window to
     * send more data.  However, there is another case:
     * MSG_MORE/TCP_CORK.  In this case, we should really send some
     * data. */
    ci_tcp_tx_fill_sendq_tail(ni, ts, &piov, &sinf CI_KERNEL_ARG(addr_spc));
    /* If we have more data to send, do it. */
    if( sinf.total_unsent > 0 )
      goto try_again;
    
    /* This is last packet.  Set PUSH flag and MORE flag.
     * Send it if possible. */
    pkt = PKT_CHK(ni, sendq->tail);
    if( (flags & MSG_MORE) || (ts->s.s_aflags & CI_SOCK_AFLAG_CORK) )
      pkt->flags |= CI_PKT_FLAG_TX_MORE;
    else {
      pkt->flags &= ~CI_PKT_FLAG_TX_MORE;
      TX_PKT_TCP(pkt)->tcp_flags |= CI_TCP_FLAG_PSH;
    }
    
    /* We should somehow push the packet.  However, it was not pushed
     * before.  It means:
     * - we have no window, and zero window timer will wake us up;
     * - there was CI_PKT_FLAG_TX_MORE, and the CORK timer is going
     *   to wake us up.
     * - Nagle.
     * All the cases are nicely handled in ci_tcp_tx_advance_nagle(), so
     * just call it.
     */
    ci_tcp_tx_advance_nagle(ni, ts);

    if( sinf.stack_locked ) ci_netif_unlock(ni);
    return sinf.total_sent;
  }

  ci_assert(sinf.total_unsent > 0);
  ci_assert(! ci_iovec_ptr_is_empty_proper(&piov));

 try_again:
  while( 1 ) {
    /* Grab packet buffers and fill them with data. */
    ci_assert(sinf.total_unsent > 0);
    ci_assert(! ci_iovec_ptr_is_empty_proper(&piov));

    /* How much space is there in the send queue? */
    m = ts->send_max - ci_tcp_sendq_n_pkts(ts);
    if( m <= 0 )  goto send_q_full;

    sinf.n_needed = ci_tcp_tx_n_pkts_needed(eff_mss, sinf.total_unsent, 
                                            CI_CFG_TCP_TX_BATCH, m);
    m = sinf.n_needed;
    sinf.fill_list = 0;
    sinf.fill_list_bytes = 0;
    sinf.n_filled = 0;

    do {
      if( si_trylock(ni, &sinf) ) {
        if( (pkt = ci_netif_pkt_tx_tcp_alloc(ni)) ) {
          ++ni->state->n_async_pkts;
          oo_pkt_filler_add_pkt(&sinf.pf, pkt);
        }
        else
          goto no_pkt_buf;
      } else 
        goto no_pkt_buf;
    } while( --sinf.n_needed > 0 );

  got_pkt_buf:
    do {
      sinf.fill_list_bytes +=
        ci_tcp_sendmsg_fill_pkt(ni, &sinf, &piov, ts->outgoing_hdrs_len,
                                eff_mss CI_KERNEL_ARG(addr_spc));
      ++sinf.n_filled;

      /* Look on MSG_MORE: do not send the last packet if it is not full */
      if (m == 1 && 
          ((flags & MSG_MORE) || (ts->s.s_aflags & CI_SOCK_AFLAG_CORK))) {
        sinf.pf.pkt->flags |= CI_PKT_FLAG_TX_MORE;
      }

      CI_USER_PTR_SET(sinf.pf.pkt->pf.tcp_tx.next, sinf.fill_list);
      sinf.fill_list = sinf.pf.pkt;
    }
    while( --m > 0 );

  filled_some_pkts:
    /* If we can grab the lock now, setup the meta-data and get sending.
     * Otherwise queue the packets for sending by the netif lock holder.
     */
    if( si_trylock(ni, &sinf) ) {
      if( ts->s.tx_errno ) {
        ci_tcp_sendmsg_handle_tx_errno(ni, ts, flags, &sinf);
        if( sinf.set_errno ) CI_SET_ERROR(sinf.rc, sinf.rc);
        return sinf.rc;
      }

      /* eff_mss may now be != ts->eff_mss */
      ci_tcp_sendmsg_enqueue(ni, ts, sinf.fill_list, sinf.fill_list_bytes);
      sinf.total_sent += sinf.fill_list_bytes;
      sinf.total_unsent -= sinf.fill_list_bytes;

      /* Now we've sent all the packets we grabbed, but not necessarily all
       * of the data -- so check to see if we're done yet.  The last
       * segment gets the PSH flag.
       */
      if( sinf.total_unsent == 0 ) {
        if( (sinf.fill_list->flags & CI_PKT_FLAG_TX_MORE) )
          TX_PKT_TCP(sinf.fill_list)->tcp_flags = CI_TCP_FLAG_ACK;
        else
          TX_PKT_TCP(sinf.fill_list)->tcp_flags = 
            CI_TCP_FLAG_PSH | CI_TCP_FLAG_ACK;
        ci_tcp_tx_advance_nagle(ni, ts);
        /* Assert that there's no need to free unused packets */
        ci_assert_equal(sinf.pf.alloc_pkt, NULL);
        if( sinf.stack_locked ) ci_netif_unlock(ni);
        return sinf.total_sent;
      }

      /* Stuff left to do -- push out what we've got first. */
      ci_netif_poll(ni);
      sinf.fill_list = 0;
      if( ts->s.tx_errno ) {
        ci_tcp_sendmsg_handle_tx_errno(ni, ts, flags, &sinf);
        if( sinf.set_errno ) CI_SET_ERROR(sinf.rc, sinf.rc);
        return sinf.rc;
      }
      if(CI_LIKELY( ! ci_ip_queue_is_empty(sendq) ))
        ci_tcp_tx_advance(ts, ni);
    }
    else {
      if( ts->s.tx_errno ) {
        ci_tcp_sendmsg_handle_tx_errno(ni, ts, flags, &sinf);
        if( sinf.set_errno ) CI_SET_ERROR(sinf.rc, sinf.rc);
        return sinf.rc;
      }

      if( sinf.total_unsent == sinf.fill_list_bytes )
        /* The last segment needs to have the PSH flag set. */
        if ( ! (sinf.fill_list->flags & CI_PKT_FLAG_TX_MORE) )
          sinf.fill_list->flags |= CI_PKT_FLAG_TX_PSH;

      /* Couldn't get the netif lock, so enqueue packets on the prequeue. */
      ci_tcp_tx_prequeue(ni, ts, sinf.fill_list);
      sinf.total_sent += sinf.fill_list_bytes;
      sinf.total_unsent -= sinf.fill_list_bytes;
      ci_assert_equal(sinf.stack_locked, 0);
      if( ci_netif_lock_or_defer_work(ni, &ts->s.b) ) {
        sinf.stack_locked = 1;
	sinf.fill_list = 0;
	if( ts->s.tx_errno ) {
          ci_tcp_sendmsg_handle_tx_errno(ni, ts, flags, &sinf);
          if( sinf.set_errno ) CI_SET_ERROR(sinf.rc, sinf.rc);
          return sinf.rc;
        }
	ci_tcp_sendmsg_enqueue_prequeue(ni, ts);
	if(CI_LIKELY( ! ci_ip_queue_is_empty(sendq) )) {
	  if( sinf.total_unsent == 0 )  ci_tcp_tx_advance_nagle(ni, ts);
	  else  ci_tcp_tx_advance(ts, ni);
	}
      }
      if( sinf.total_unsent == 0 ) {
        /* Assert that there's no need to free unused packets */
        ci_assert_equal(sinf.pf.alloc_pkt, NULL);
        if( sinf.stack_locked ) ci_netif_unlock(ni);
        return sinf.total_sent;
      }
      /* We've more to send, so keep filling buffers. */
    }
  }

 send_q_full:
  /* We jump into here when the send queue (including prequeue) is full. */
  ci_assert(sinf.total_unsent > 0);
  sinf.fill_list = 0;

  if( ci_netif_may_poll(ni) && ci_netif_need_poll(ni) &&
      si_trylock(ni, &sinf) ) {
    ci_netif_poll(ni);
    if( ts->s.tx_errno ) {
      ci_tcp_sendmsg_handle_tx_errno(ni, ts, flags, &sinf);
      if( sinf.set_errno ) CI_SET_ERROR(sinf.rc, sinf.rc);
      return sinf.rc;
    }
    sinf.n_needed = ts->send_max - ci_tcp_sendq_n_pkts(ts);
    if( sinf.n_needed > 0 )  goto try_again;
  }

  /* The send queue is full, the prequeue is empty, and the netif has been
  ** polled recently (or is contended, in which case it will be polled
  ** soon).  We either want to block or return.
  */
  if( flags & MSG_DONTWAIT ) {
    /* We don't need to check tx_errno here.  We are here because the send
    ** queue is (was) full.  Therefore tx_errno was not set when we did
    ** that check.  ie. We got in before tx_errno was set (so we don't care
    ** if it got set subsequently).
    */
    sinf.rc = -EAGAIN;
    ci_tcp_sendmsg_handle_sent_or_rc(ni, ts, flags, &sinf);
    if( sinf.set_errno ) CI_SET_ERROR(sinf.rc, sinf.rc);
    return sinf.rc;
  }

  if( sinf.tcp_send_spin ) {
    int rc;
    rc = ci_tcp_sendmsg_spin(ni, ts, flags, &sinf);
    if( rc == 0 )
      goto try_again;
    else if( rc == -1 ) {
      if( sinf.set_errno ) CI_SET_ERROR(sinf.rc, sinf.rc);
      return sinf.rc;
    }
    sinf.tcp_send_spin = 0;
  }

  if( ci_tcp_sendmsg_block(ni, ts, flags, &sinf) == 0 )
    goto try_again;
  else {
    if( sinf.set_errno ) CI_SET_ERROR(sinf.rc, sinf.rc);
    return sinf.rc;
  }

 no_pkt_buf:
  {
    int rc;
    rc = ci_tcp_sendmsg_no_pkt_buf(ni, ts, flags, &sinf);
    if( rc == 0 )
      goto got_pkt_buf;
    else if( rc == 1 )
      goto filled_some_pkts;
    else {
      ci_assert(rc == -1);
      if( sinf.set_errno ) CI_SET_ERROR(sinf.rc, sinf.rc);
      return sinf.rc;
    }
  }

 not_synchronised:
  if( ci_tcp_sendmsg_notsynchronised(ni, ts, flags, &sinf) == -1 ) {
    ci_tcp_sendmsg_handle_rc_or_tx_errno(ni, ts, flags, &sinf);
    if( sinf.set_errno ) CI_SET_ERROR(sinf.rc, sinf.rc);
    return sinf.rc;
  }
  goto is_sync;

 slow_path:
  if( ci_tcp_sendmsg_slowpath(ni, ts, msg, flags, &sinf 
                              CI_KERNEL_ARG(addr_spc)) == -1 ) {
    ci_tcp_sendmsg_handle_rc_or_tx_errno(ni, ts, flags, &sinf);
    if( sinf.set_errno ) CI_SET_ERROR(sinf.rc, sinf.rc);
    return sinf.rc;
  }
  return sinf.rc;
}


#ifndef __KERNEL__
/* 
 * TODO:
 *  - handle case where iov_len > mss;
 *  - improve TCP send path (in general) to handle fragmented buffers, then:
 *   o append a small buffer to the existing send queue (via frag
 *     next) if there's space;
 *   o coalesce small buffers together (via * frag next) into a single
 *     packet;
 */

int ci_tcp_zc_send(ci_netif* ni, ci_tcp_state* ts, struct onload_zc_mmsg* msg,
                   int flags)
{
  struct tcp_send_info sinf;
  ci_ip_pkt_fmt* pkt;
  int j, sendq_space;
  unsigned eff_mss;

  ci_assert(msg != NULL);
  ci_assert(ts);
  ci_assert(ts->s.b.state != CI_TCP_LISTEN);

  if( !(ts->s.b.state & CI_TCP_STATE_SYNCHRONISED) ) {
    msg->rc = ts->s.tx_errno ? -ts->s.tx_errno : -EPIPE;
    return 1;
  }

  sinf.rc = 0;
  sinf.stack_locked = 0;
  sinf.fill_list = 0;
  sinf.fill_list_bytes = 0;
  sinf.n_filled = 0;
  sinf.total_sent = 0; /*< not used */
  sinf.pf.alloc_pkt = NULL;
  sinf.timeout = ts->s.so.sndtimeo_msec;
#ifndef __KERNEL__
  sinf.tcp_send_spin = 
    oo_per_thread_get()->spinstate & (1 << ONLOAD_SPIN_TCP_SEND);
  if( sinf.tcp_send_spin )
    ci_frc64(&sinf.start_frc);
#else
  sinf.tcp_send_spin = 0;
#endif

  eff_mss = tcp_eff_mss(ts);
  ci_assert_le(eff_mss,
               CI_MAX_ETH_DATA_LEN - sizeof(ci_tcp_hdr) - sizeof(ci_ip4_hdr));

  j = 0;

 try_again:
  sendq_space = ts->send_max - ci_tcp_sendq_n_pkts(ts);
  if( sendq_space <= 0 )
    goto send_q_full;
  
 send_q_not_full:
  while( j < msg->msg.msghdr.msg_iovlen ) {
    pkt = (ci_ip_pkt_fmt*)msg->msg.iov[j].buf;

    ci_assert(msg->msg.iov[j].iov_base != NULL);
    ci_assert_gt(msg->msg.iov[j].iov_len, 0);
    ci_assert_le(msg->msg.iov[j].iov_len, eff_mss);
    ci_assert_gt((char*)msg->msg.iov[j].iov_base,
                 PKT_START(pkt) + ts->outgoing_hdrs_len);
    ci_assert_lt((char*)msg->msg.iov[j].iov_base + 
                 msg->msg.iov[j].iov_len, 
                 ((char*)pkt) + CI_CFG_PKT_BUF_SIZE);
      
    if( msg->msg.iov[j].iov_len <= 0 || 
        msg->msg.iov[j].iov_len > eff_mss || 
        (char*)msg->msg.iov[j].iov_base < 
        PKT_START(pkt) + ts->outgoing_hdrs_len ||
        (char*)msg->msg.iov[j].iov_base + msg->msg.iov[j].iov_len > 
        ((char*)pkt) + CI_CFG_PKT_BUF_SIZE )
      goto bad_buffer;

    ci_tcp_tx_pkt_init(pkt, (char*)msg->msg.iov[j].iov_base - 
                       oo_tx_ether_data(pkt), eff_mss);
    pkt->n_buffers = 1;
    pkt->buf_len += msg->msg.iov[j].iov_len;
    pkt->tx_pkt_len += msg->msg.iov[j].iov_len;
    oo_offbuf_advance(&pkt->buf, msg->msg.iov[j].iov_len);
    pkt->pf.tcp_tx.end_seq = msg->msg.iov[j].iov_len;

    ci_assert_equal(TX_PKT_LEN(pkt), oo_offbuf_ptr(&pkt->buf) - PKT_START(pkt));

    CI_USER_PTR_SET(pkt->pf.tcp_tx.next, sinf.fill_list);
    sinf.fill_list = pkt;
    sinf.fill_list_bytes += msg->msg.iov[j].iov_len;

    /* Accumulate bytes sent for return */
    if( j == 0 )
      msg->rc = msg->msg.iov[j].iov_len;
    else
      msg->rc += msg->msg.iov[j].iov_len;

    ++sinf.n_filled;
    ++j;
  }

  if( ((flags & MSG_MORE) || (ts->s.s_aflags & CI_SOCK_AFLAG_CORK)) )
    sinf.fill_list->flags |= CI_PKT_FLAG_TX_MORE;

  /* If we can grab the lock now, setup the meta-data and get sending.
   * Otherwise queue the packets for sending by the netif lock holder.
   */
  if( si_trylock(ni, &sinf) ) {
    if( ts->s.tx_errno )
      goto tx_errno;
    ci_tcp_sendmsg_enqueue(ni, ts, sinf.fill_list, sinf.fill_list_bytes);

    if( (sinf.fill_list->flags & CI_PKT_FLAG_TX_MORE) )
      TX_PKT_TCP(sinf.fill_list)->tcp_flags = CI_TCP_FLAG_ACK;
    else
      TX_PKT_TCP(sinf.fill_list)->tcp_flags = CI_TCP_FLAG_PSH|CI_TCP_FLAG_ACK;
    ci_tcp_tx_advance_nagle(ni, ts);
    ci_netif_unlock(ni);

    return 1;
  }
  else {
    if( ts->s.tx_errno )
      goto tx_errno;

    if( !(sinf.fill_list->flags & CI_PKT_FLAG_TX_MORE) )
      sinf.fill_list->flags |= CI_PKT_FLAG_TX_PSH;

    ci_tcp_tx_prequeue(ni, ts, sinf.fill_list);
    sinf.fill_list = 0;

    ci_assert_equal(sinf.stack_locked, 0);
    if( ci_netif_lock_or_defer_work(ni, &ts->s.b) ) {
      sinf.stack_locked = 1;
      if( ts->s.tx_errno )
        goto tx_errno;
      ci_tcp_sendmsg_enqueue_prequeue(ni, ts);
      if(CI_LIKELY( ! ci_ip_queue_is_empty(&ts->send) ))
        ci_tcp_tx_advance_nagle(ni, ts);
    }
    if( sinf.stack_locked ) 
      ci_netif_unlock(ni);
    return 1;
  }

 send_q_full:
  if( ci_netif_may_poll(ni) && ci_netif_need_poll(ni) &&
      si_trylock(ni, &sinf) ) {
    ci_netif_poll(ni);
    if( ts->s.tx_errno )
      goto tx_errno;
    sendq_space = ts->send_max - ci_tcp_sendq_n_pkts(ts);
    if( sendq_space > 0 )
      goto send_q_not_full;
  }

  if( flags & MSG_DONTWAIT ) {
    if( j == 0 )
      msg->rc = -EAGAIN;
    if( sinf.stack_locked )
      ci_netif_unlock(ni);
    return 1;
  }

  if( sinf.tcp_send_spin ) {
    int rc;
    rc = ci_tcp_sendmsg_spin(ni, ts, flags, &sinf);
    if( rc == 0 )
      goto try_again;
    else if( rc == -1 ) {
      if( sinf.stack_locked ) 
        ci_netif_unlock(ni);
      if( j == 0 )
        /* Must invert error sign as functions shared with sendmsg store
         * error as positive 
         */
        msg->rc = -sinf.rc;
      return 1;
    }
  }

  if( ci_tcp_sendmsg_block(ni, ts, flags, &sinf) == 0 )
    goto try_again;
  else {
    if( sinf.stack_locked ) 
      ci_netif_unlock(ni);
    if( j == 0 )
      /* Must invert error sign as functions shared with sendmsg store
       * error as positive 
       */
      msg->rc = -sinf.rc;
    return 1;
  }


 bad_buffer:
  /* First make sure we've got rid of the fill list */
  if( sinf.fill_list ) {
    if( si_trylock(ni, &sinf) ) {
      if( ts->s.tx_errno )
        goto tx_errno;
      ci_tcp_sendmsg_enqueue(ni, ts, sinf.fill_list, sinf.fill_list_bytes);
      sinf.fill_list = 0;
    } 
    else {
      if( ts->s.tx_errno )
        goto tx_errno;

      ci_tcp_tx_prequeue(ni, ts, sinf.fill_list);
      sinf.fill_list = 0;
      ci_assert_equal(sinf.stack_locked, 0);
      if( ci_netif_lock_or_defer_work(ni, &ts->s.b) ) {
        sinf.stack_locked = 1;
        if( ts->s.tx_errno )
          goto tx_errno;
        ci_tcp_sendmsg_enqueue_prequeue(ni, ts);
      } 
      if(CI_LIKELY( ! ci_ip_queue_is_empty(&ts->send) ))
        ci_tcp_tx_advance(ts, ni);
    }
  }

  if( j == 0 )
    msg->rc = -EINVAL;
  if( sinf.stack_locked )
    ci_netif_unlock(ni);
  return 1;

 tx_errno:
  /* We've potentially got stuff in the fill_list, so need to work
   * back and undo that
   */
  ci_tcp_sendmsg_free_fill_list(ni, ts, flags, &sinf);
  ci_tcp_tx_free_prequeue(ni, ts, sinf.stack_locked);
  msg->rc = -ts->s.tx_errno;
  if( sinf.stack_locked )
    ci_netif_unlock(ni);
  return 1;
}
#endif

/*! \cidoxg_end */