/*
 * ngtcp2
 *
 * Copyright (c) 2017 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "ngtcp2_conn.h"

#include <string.h>
#include <assert.h>
#include <math.h>

#include "ngtcp2_ppe.h"
#include "ngtcp2_macro.h"
#include "ngtcp2_log.h"
#include "ngtcp2_cid.h"
#include "ngtcp2_conv.h"

/*
 * conn_local_stream returns nonzero if |stream_id| indicates that it
 * is the stream initiated by local endpoint.
 */
static int conn_local_stream(ngtcp2_conn *conn, uint64_t stream_id) {
  return (uint8_t)(stream_id & 1) == conn->server;
}

/*
 * bidi_stream returns nonzero if |stream_id| is a bidirectional
 * stream ID.
 */
static int bidi_stream(uint64_t stream_id) { return (stream_id & 0x2) == 0; }

static int conn_call_recv_client_initial(ngtcp2_conn *conn) {
  int rv;

  assert(conn->callbacks.recv_client_initial);

  rv = conn->callbacks.recv_client_initial(conn, &conn->rcid, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_handshake_completed(ngtcp2_conn *conn) {
  int rv;

  if (!conn->callbacks.handshake_completed) {
    return 0;
  }

  rv = conn->callbacks.handshake_completed(conn, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_recv_stream_data(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                      uint8_t fin, uint64_t offset,
                                      const uint8_t *data, size_t datalen) {
  int rv;

  if (!conn->callbacks.recv_stream_data) {
    return 0;
  }

  rv = conn->callbacks.recv_stream_data(conn, strm->stream_id, fin, offset,
                                        data, datalen, conn->user_data,
                                        strm->stream_user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_recv_crypto_data(ngtcp2_conn *conn, uint64_t offset,
                                      const uint8_t *data, size_t datalen) {
  int rv;

  rv = conn->callbacks.recv_crypto_data(conn, offset, data, datalen,
                                        conn->user_data);
  switch (rv) {
  case 0:
  case NGTCP2_ERR_CRYPTO:
  case NGTCP2_ERR_PROTO:
  case NGTCP2_ERR_INTERNAL:
  case NGTCP2_ERR_CALLBACK_FAILURE:
    return rv;
  default:
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
}

static int conn_call_stream_close(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                  uint16_t app_error_code) {
  int rv;

  if (!conn->callbacks.stream_close) {
    return 0;
  }

  rv = conn->callbacks.stream_close(conn, strm->stream_id, app_error_code,
                                    conn->user_data, strm->stream_user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_extend_max_stream_id(ngtcp2_conn *conn,
                                          uint64_t max_stream_id) {
  int rv;

  if (!conn->callbacks.extend_max_stream_id) {
    return 0;
  }

  rv = conn->callbacks.extend_max_stream_id(conn, max_stream_id,
                                            conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int pktns_init(ngtcp2_pktns *pktns, int delayed_ack, ngtcp2_cc_stat *ccs,
                      ngtcp2_log *log, ngtcp2_mem *mem) {
  int rv;

  pktns->last_tx_pkt_num = (uint64_t)-1;

  rv = ngtcp2_acktr_init(&pktns->acktr, delayed_ack, log, mem);
  if (rv != 0) {
    return rv;
  }

  ngtcp2_rtb_init(&pktns->rtb, ccs, log, mem);

  return 0;
}

static void pktns_free(ngtcp2_pktns *pktns, ngtcp2_mem *mem) {
  ngtcp2_crypto_km_del(pktns->rx_ckm, mem);
  ngtcp2_crypto_km_del(pktns->tx_ckm, mem);

  ngtcp2_rtb_free(&pktns->rtb);
  ngtcp2_acktr_free(&pktns->acktr);
}

static int conn_new(ngtcp2_conn **pconn, const ngtcp2_cid *dcid,
                    const ngtcp2_cid *scid, uint32_t version,
                    const ngtcp2_conn_callbacks *callbacks,
                    const ngtcp2_settings *settings, void *user_data,
                    int server) {
  int rv;
  ngtcp2_mem *mem = ngtcp2_mem_default();

  *pconn = ngtcp2_mem_calloc(mem, 1, sizeof(ngtcp2_conn));
  if (*pconn == NULL) {
    rv = NGTCP2_ERR_NOMEM;
    goto fail_conn;
  }

  rv = ngtcp2_strm_init(&(*pconn)->crypto, 0, NGTCP2_STRM_FLAG_NONE, 0, 0, NULL,
                        mem);
  if (rv != 0) {
    goto fail_crypto_init;
  }

  rv = ngtcp2_map_init(&(*pconn)->strms, mem);
  if (rv != 0) {
    goto fail_strms_init;
  }

  rv = ngtcp2_idtr_init(&(*pconn)->remote_bidi_idtr, !server, mem);
  if (rv != 0) {
    goto fail_remote_bidi_idtr_init;
  }

  rv = ngtcp2_idtr_init(&(*pconn)->remote_uni_idtr, !server, mem);
  if (rv != 0) {
    goto fail_remote_uni_idtr_init;
  }

  rv = ngtcp2_ringbuf_init(&(*pconn)->tx_path_challenge, 4,
                           sizeof(ngtcp2_path_challenge_entry), mem);
  if (rv != 0) {
    goto fail_tx_path_challenge_init;
  }

  rv = ngtcp2_ringbuf_init(&(*pconn)->rx_path_challenge, 4,
                           sizeof(ngtcp2_path_challenge_entry), mem);
  if (rv != 0) {
    goto fail_rx_path_challenge_init;
  }

  // TODO Setting upper bound 64 is not ideal.
  rv = ngtcp2_ringbuf_init(&(*pconn)->tx_crypto_data, 64,
                           sizeof(ngtcp2_crypto_data), mem);
  if (rv != 0) {
    goto fail_tx_crypto_data_init;
  }

  (*pconn)->scid = *scid;
  (*pconn)->dcid = *dcid;

  ngtcp2_log_init(&(*pconn)->log, &(*pconn)->scid, settings->log_printf,
                  settings->initial_ts, user_data);

  rv = pktns_init(&(*pconn)->in_pktns, 0 /* delayed_ack */, &(*pconn)->ccs,
                  &(*pconn)->log, mem);
  if (rv != 0) {
    goto fail_in_pktns_init;
  }

  rv = pktns_init(&(*pconn)->hs_pktns, 0 /* delayed_ack */, &(*pconn)->ccs,
                  &(*pconn)->log, mem);
  if (rv != 0) {
    goto fail_hs_pktns_init;
  }

  rv = pktns_init(&(*pconn)->pktns, 1 /* delayed_ack */, &(*pconn)->ccs,
                  &(*pconn)->log, mem);
  if (rv != 0) {
    goto fail_pktns_init;
  }

  (*pconn)->callbacks = *callbacks;
  (*pconn)->version = version;
  (*pconn)->mem = mem;
  (*pconn)->user_data = user_data;
  (*pconn)->largest_ack = -1;
  (*pconn)->local_settings = *settings;
  (*pconn)->unsent_max_rx_offset = (*pconn)->max_rx_offset = settings->max_data;
  (*pconn)->rcs.min_rtt = UINT64_MAX;
  (*pconn)->rcs.reordering_threshold = NGTCP2_REORDERING_THRESHOLD;
  (*pconn)->ccs.cwnd = ngtcp2_min(10 * NGTCP2_MAX_DGRAM_SIZE,
                                  ngtcp2_max(2 * NGTCP2_MAX_DGRAM_SIZE, 14600));
  (*pconn)->ccs.eor_pkt_num = 0;
  (*pconn)->ccs.ssthresh = UINT64_MAX;

  return 0;

fail_pktns_init:
  pktns_free(&(*pconn)->hs_pktns, mem);
fail_hs_pktns_init:
  pktns_free(&(*pconn)->in_pktns, mem);
fail_in_pktns_init:
  ngtcp2_ringbuf_free(&(*pconn)->tx_crypto_data);
fail_tx_crypto_data_init:
  ngtcp2_ringbuf_free(&(*pconn)->rx_path_challenge);
fail_rx_path_challenge_init:
  ngtcp2_ringbuf_free(&(*pconn)->tx_path_challenge);
fail_tx_path_challenge_init:
  ngtcp2_idtr_free(&(*pconn)->remote_uni_idtr);
fail_remote_uni_idtr_init:
  ngtcp2_idtr_free(&(*pconn)->remote_bidi_idtr);
fail_remote_bidi_idtr_init:
  ngtcp2_map_free(&(*pconn)->strms);
fail_strms_init:
  ngtcp2_strm_free(&(*pconn)->crypto);
fail_crypto_init:
  ngtcp2_mem_free(mem, *pconn);
fail_conn:
  return rv;
}

int ngtcp2_conn_client_new(ngtcp2_conn **pconn, const ngtcp2_cid *dcid,
                           const ngtcp2_cid *scid, uint32_t version,
                           const ngtcp2_conn_callbacks *callbacks,
                           const ngtcp2_settings *settings, void *user_data) {
  int rv;
  rv = conn_new(pconn, dcid, scid, version, callbacks, settings, user_data, 0);
  if (rv != 0) {
    return rv;
  }
  (*pconn)->unsent_max_remote_stream_id_bidi =
      (*pconn)->max_remote_stream_id_bidi =
          ngtcp2_nth_server_bidi_id(settings->max_bidi_streams);

  (*pconn)->unsent_max_remote_stream_id_uni =
      (*pconn)->max_remote_stream_id_uni =
          ngtcp2_nth_server_uni_id(settings->max_uni_streams);

  (*pconn)->state = NGTCP2_CS_CLIENT_INITIAL;
  (*pconn)->next_local_stream_id_bidi = 0;
  (*pconn)->next_local_stream_id_uni = 2;
  return 0;
}

int ngtcp2_conn_server_new(ngtcp2_conn **pconn, const ngtcp2_cid *dcid,
                           const ngtcp2_cid *scid, uint32_t version,
                           const ngtcp2_conn_callbacks *callbacks,
                           const ngtcp2_settings *settings, void *user_data) {
  int rv;
  rv = conn_new(pconn, dcid, scid, version, callbacks, settings, user_data, 1);
  if (rv != 0) {
    return rv;
  }
  (*pconn)->server = 1;
  (*pconn)->unsent_max_remote_stream_id_bidi =
      (*pconn)->max_remote_stream_id_bidi =
          ngtcp2_nth_client_bidi_id(settings->max_bidi_streams);

  (*pconn)->unsent_max_remote_stream_id_uni =
      (*pconn)->max_remote_stream_id_uni =
          ngtcp2_nth_client_uni_id(settings->max_uni_streams);

  (*pconn)->state = NGTCP2_CS_SERVER_INITIAL;
  (*pconn)->next_local_stream_id_bidi = 1;
  (*pconn)->next_local_stream_id_uni = 3;
  return 0;
}

static void delete_buffed_pkts(ngtcp2_pkt_chain *pc, ngtcp2_mem *mem) {
  ngtcp2_pkt_chain *next;

  for (; pc;) {
    next = pc->next;
    ngtcp2_pkt_chain_del(pc, mem);
    pc = next;
  }
}

static void delete_frq(ngtcp2_frame_chain *frc, ngtcp2_mem *mem) {
  ngtcp2_frame_chain *next;
  for (; frc;) {
    next = frc->next;
    ngtcp2_frame_chain_del(frc, mem);
    frc = next;
  }
}

static int delete_strms_each(ngtcp2_map_entry *ent, void *ptr) {
  ngtcp2_mem *mem = ptr;
  ngtcp2_strm *s = ngtcp2_struct_of(ent, ngtcp2_strm, me);

  ngtcp2_strm_free(s);
  ngtcp2_mem_free(mem, s);

  return 0;
}

static void delete_early_rtb(ngtcp2_rtb_entry *ent, ngtcp2_mem *mem) {
  ngtcp2_rtb_entry *next;

  while (ent) {
    next = ent->next;
    ngtcp2_rtb_entry_del(ent, mem);
    ent = next;
  }
}

void ngtcp2_conn_del(ngtcp2_conn *conn) {
  if (conn == NULL) {
    return;
  }

  ngtcp2_mem_free(conn->mem, conn->token.begin);
  ngtcp2_mem_free(conn->mem, conn->decrypt_buf.base);

  delete_buffed_pkts(conn->buffed_rx_ppkts, conn->mem);
  delete_buffed_pkts(conn->buffed_rx_hs_pkts, conn->mem);

  ngtcp2_crypto_km_del(conn->early_ckm, conn->mem);

  delete_frq(conn->frq, conn->mem);

  pktns_free(&conn->pktns, conn->mem);
  pktns_free(&conn->hs_pktns, conn->mem);
  pktns_free(&conn->in_pktns, conn->mem);

  delete_early_rtb(conn->retry_early_rtb, conn->mem);

  ngtcp2_ringbuf_free(&conn->tx_crypto_data);

  ngtcp2_ringbuf_free(&conn->rx_path_challenge);
  ngtcp2_ringbuf_free(&conn->tx_path_challenge);

  ngtcp2_idtr_free(&conn->remote_uni_idtr);
  ngtcp2_idtr_free(&conn->remote_bidi_idtr);
  ngtcp2_map_each_free(&conn->strms, delete_strms_each, conn->mem);
  ngtcp2_map_free(&conn->strms);

  ngtcp2_strm_free(&conn->crypto);

  ngtcp2_mem_free(conn->mem, conn);
}

/*
 * conn_ensure_ack_blks makes sure that |(*pfr)->ack.blks| can contain
 * at least |n| ngtcp2_ack_blk.  |*pfr| points to the ngtcp2_frame
 * object.  |*pnum_blks_max| is the number of ngtpc2_ack_blk which
 * |*pfr| can contain.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_ensure_ack_blks(ngtcp2_conn *conn, ngtcp2_frame **pfr,
                                size_t *pnum_blks_max, size_t n) {
  ngtcp2_frame *fr;

  if (n <= *pnum_blks_max) {
    return 0;
  }

  *pnum_blks_max *= 2;
  fr = ngtcp2_mem_realloc(conn->mem, *pfr,
                          sizeof(ngtcp2_ack) +
                              sizeof(ngtcp2_ack_blk) * (*pnum_blks_max - 1));
  if (fr == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  *pfr = fr;

  return 0;
}

/*
 * conn_compute_ack_delay computes ACK delay for outgoing protected
 * ACK.
 */
static uint64_t conn_compute_ack_delay(ngtcp2_conn *conn) {
  uint64_t ack_delay;

  if (conn->rcs.min_rtt == UINT64_MAX) {
    return NGTCP2_DEFAULT_ACK_DELAY;
  }

  ack_delay = (uint64_t)(conn->rcs.smoothed_rtt / 4);

  return ngtcp2_min(NGTCP2_DEFAULT_ACK_DELAY, ack_delay);
}

/*
 * conn_create_ack_frame creates ACK frame, and assigns its pointer to
 * |*pfr| if there are any received packets to acknowledge.  If there
 * are no packets to acknowledge, this function returns 0, and |*pfr|
 * is untouched.  The caller is advised to set |*pfr| to NULL before
 * calling this function, and check it after this function returns.
 * If |nodelay| is nonzero, delayed ACK timer is ignored.
 *
 * The memory for ACK frame is dynamically allocated by this function.
 * A caller is responsible to free it.
 *
 * Call ngtcp2_acktr_commit_ack after a created ACK frame is
 * successfully serialized into a packet.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_create_ack_frame(ngtcp2_conn *conn, ngtcp2_frame **pfr,
                                 ngtcp2_acktr *acktr, ngtcp2_tstamp ts,
                                 int nodelay, uint8_t ack_delay_exponent) {
  uint64_t first_pkt_num;
  uint64_t last_pkt_num;
  ngtcp2_ack_blk *blk;
  int initial = 1;
  uint64_t gap;
  ngtcp2_ksl_it it;
  ngtcp2_acktr_entry *rpkt;
  ngtcp2_frame *fr;
  ngtcp2_ack *ack;
  /* TODO Measure an actual size of ACK bloks to find the best default
     value. */
  size_t num_blks_max = 8;
  size_t blk_idx;
  int rv;
  uint64_t max_ack_delay = (nodelay || !ngtcp2_acktr_delayed_ack(acktr))
                               ? 0
                               : conn_compute_ack_delay(conn);

  if (!ngtcp2_acktr_require_active_ack(acktr, max_ack_delay, ts)) {
    return 0;
  }

  it = ngtcp2_acktr_get(acktr);
  if (ngtcp2_ksl_it_end(&it)) {
    ngtcp2_acktr_commit_ack(acktr);
    return 0;
  }

  fr = ngtcp2_mem_malloc(conn->mem, sizeof(ngtcp2_ack) +
                                        sizeof(ngtcp2_ack_blk) * num_blks_max);
  if (fr == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  ack = &fr->ack;

  rpkt = ngtcp2_ksl_it_get(&it);
  first_pkt_num = last_pkt_num = rpkt->pkt_num;

  ack->type = NGTCP2_FRAME_ACK;
  ack->largest_ack = first_pkt_num;
  ack->ack_delay_unscaled = ts - rpkt->tstamp;
  ack->ack_delay = (ack->ack_delay_unscaled / 1000) >> ack_delay_exponent;
  ack->num_blks = 0;

  ngtcp2_ksl_it_next(&it);

  for (; !ngtcp2_ksl_it_end(&it); ngtcp2_ksl_it_next(&it)) {
    rpkt = ngtcp2_ksl_it_get(&it);
    if (rpkt->pkt_num + 1 == last_pkt_num) {
      last_pkt_num = rpkt->pkt_num;
      continue;
    }

    if (initial) {
      initial = 0;
      ack->first_ack_blklen = first_pkt_num - last_pkt_num;
    } else {
      blk_idx = ack->num_blks++;
      rv = conn_ensure_ack_blks(conn, &fr, &num_blks_max, ack->num_blks);
      if (rv != 0) {
        ngtcp2_mem_free(conn->mem, fr);
        return rv;
      }
      ack = &fr->ack;
      blk = &ack->blks[blk_idx];
      blk->gap = gap;
      blk->blklen = first_pkt_num - last_pkt_num;
    }

    gap = last_pkt_num - rpkt->pkt_num - 2;
    first_pkt_num = last_pkt_num = rpkt->pkt_num;

    if (ack->num_blks == NGTCP2_MAX_ACK_BLKS - 1) {
      break;
    }
  }

  if (initial) {
    ack->first_ack_blklen = first_pkt_num - last_pkt_num;
  } else {
    blk_idx = ack->num_blks++;
    rv = conn_ensure_ack_blks(conn, &fr, &num_blks_max, ack->num_blks);
    if (rv != 0) {
      ngtcp2_mem_free(conn->mem, fr);
      return rv;
    }
    ack = &fr->ack;
    blk = &ack->blks[blk_idx];
    blk->gap = gap;
    blk->blklen = first_pkt_num - last_pkt_num;
  }

  /* TODO Just remove entries which cannot fit into a single ACK frame
     for now. */
  if (!ngtcp2_ksl_it_end(&it)) {
    rv = ngtcp2_acktr_forget(acktr, ngtcp2_ksl_it_get(&it));
    if (rv != 0) {
      return rv;
    }
  }

  *pfr = fr;

  return 0;
}

/*
 * conn_ppe_write_frame writes |fr| to |ppe|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOBUF
 *     Buffer is too small.
 */
static int conn_ppe_write_frame(ngtcp2_conn *conn, ngtcp2_ppe *ppe,
                                const ngtcp2_pkt_hd *hd, ngtcp2_frame *fr) {
  int rv;

  rv = ngtcp2_ppe_encode_frame(ppe, fr);
  if (rv != 0) {
    return rv;
  }

  ngtcp2_log_tx_fr(&conn->log, hd, fr);

  return 0;
}

static void conn_on_pkt_sent(ngtcp2_conn *conn, ngtcp2_rtb *rtb,
                             ngtcp2_rtb_entry *ent) {
  /* This function implements OnPacketSent, but it handles only
     retransmittable packet (non-ACK only packet). */
  ngtcp2_rtb_add(rtb, ent);

  if (ngtcp2_pkt_handshake_pkt(&ent->hd) &&
      (ent->hd.type != NGTCP2_PKT_0RTT_PROTECTED ||
       (ent->flags & NGTCP2_RTB_FLAG_0RTT))) {
    conn->rcs.last_hs_tx_pkt_ts = ent->ts;
  } else {
    conn->rcs.last_tx_pkt_ts = ent->ts;
  }
  ngtcp2_conn_set_loss_detection_timer(conn);
}

/*
 * conn_select_pkt_numlen selects shortest packet number encoding
 * based on the next packet number |pkt_num| and the largest
 * acknowledged packet number.  It returns the number of bytes to
 * encode the packet number.
 */
static size_t rtb_select_pkt_numlen(ngtcp2_rtb *rtb, uint64_t pkt_num) {
  uint64_t n = (uint64_t)((int64_t)pkt_num - rtb->largest_acked_tx_pkt_num);
  if (UINT64_MAX / 2 <= pkt_num) {
    return 4;
  }

  n = n * 2 + 1;

  if (n > 0x3fff) {
    return 4;
  }
  if (n > 0x7f) {
    return 2;
  }
  return 1;
}

/*
 * conn_retransmit_pkt writes QUIC packet in the buffer pointed by
 * |dest| whose length is |destlen| to retransmit lost packet.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOBUF
 *     Buffer is too small.
 */
static ssize_t conn_retransmit_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                   size_t destlen, ngtcp2_pktns *pktns,
                                   ngtcp2_rtb_entry *ent, int require_padding,
                                   ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_ppe ppe;
  ngtcp2_pkt_hd hd = ent->hd;
  ngtcp2_frame_chain **pfrc, *frc;
  ngtcp2_frame *ackfr;
  ngtcp2_frame localfr;
  int pkt_empty = 1;
  ssize_t nwrite;
  ngtcp2_crypto_ctx ctx;
  ngtcp2_strm *strm;

  /* This is required because ent->hd may have old client version. */
  hd.version = conn->version;
  if (hd.flags & NGTCP2_PKT_FLAG_LONG_FORM) {
    switch (hd.type) {
    case NGTCP2_PKT_INITIAL:
      ctx.aead_overhead = NGTCP2_INITIAL_AEAD_OVERHEAD;
      ctx.encrypt = conn->callbacks.in_encrypt;
      ctx.encrypt_pn = conn->callbacks.in_encrypt_pn;
      ctx.ckm = pktns->tx_ckm;
      /* TODO On client side, we might have client chosen random DCID
         here.  Not sure we have to change it to server chosen DCID
         after client learned it.  It might be better to cancel
         retransmission if we know it. */
      break;
    case NGTCP2_PKT_HANDSHAKE:
      ctx.aead_overhead = conn->aead_overhead;
      ctx.encrypt = conn->callbacks.encrypt;
      ctx.encrypt_pn = conn->callbacks.encrypt_pn;
      ctx.ckm = pktns->tx_ckm;
      break;
    case NGTCP2_PKT_0RTT_PROTECTED:
      ctx.aead_overhead = conn->aead_overhead;
      ctx.encrypt = conn->callbacks.encrypt;
      ctx.encrypt_pn = conn->callbacks.encrypt_pn;
      ctx.ckm = conn->early_ckm;
      /* We might have client chosen random DCID. */
      hd.dcid = conn->dcid;
      break;
    default:
      assert(0);
    }
  } else {
    assert(hd.type == NGTCP2_PKT_SHORT);

    ctx.aead_overhead = conn->aead_overhead;
    ctx.encrypt = conn->callbacks.encrypt;
    ctx.encrypt_pn = conn->callbacks.encrypt_pn;
    ctx.ckm = pktns->tx_ckm;
  }

  ctx.user_data = conn;

  hd.pkt_num = pktns->last_tx_pkt_num + 1;
  hd.pkt_numlen = rtb_select_pkt_numlen(&pktns->rtb, hd.pkt_num);

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx);

  ngtcp2_log_tx_pkt_hd(&conn->log, &hd);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    return rv;
  }

  for (pfrc = &ent->frc; *pfrc;) {
    switch ((*pfrc)->fr.type) {
    case NGTCP2_FRAME_STREAM:
      strm = ngtcp2_conn_find_stream(conn, (*pfrc)->fr.stream.stream_id);
      if (strm == NULL || (strm->flags & NGTCP2_STRM_FLAG_SENT_RST)) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    case NGTCP2_FRAME_STOP_SENDING:
      strm = ngtcp2_conn_find_stream(conn, (*pfrc)->fr.stop_sending.stream_id);
      if (strm == NULL || (strm->flags & NGTCP2_STRM_FLAG_SHUT_RD)) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
      }
      break;
    case NGTCP2_FRAME_MAX_STREAM_ID: {
      int cancel;
      if (bidi_stream((*pfrc)->fr.max_stream_id.max_stream_id)) {
        cancel = (*pfrc)->fr.max_stream_id.max_stream_id <
                 conn->max_remote_stream_id_bidi;
      } else {
        cancel = (*pfrc)->fr.max_stream_id.max_stream_id <
                 conn->max_remote_stream_id_uni;
      }
      if (cancel) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    }
    case NGTCP2_FRAME_MAX_STREAM_DATA:
      strm =
          ngtcp2_conn_find_stream(conn, (*pfrc)->fr.max_stream_data.stream_id);
      if (strm == NULL ||
          (*pfrc)->fr.max_stream_data.max_stream_data < strm->max_rx_offset) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    case NGTCP2_FRAME_MAX_DATA:
      if ((*pfrc)->fr.max_data.max_data < conn->max_rx_offset) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    }
    rv = conn_ppe_write_frame(conn, &ppe, &hd, &(*pfrc)->fr);
    if (rv != 0) {
      return rv;
    }

    pkt_empty = 0;
    pfrc = &(*pfrc)->next;
  }

  if (pkt_empty) {
    ngtcp2_log_tx_cancel(&conn->log, &hd);
    return rv;
  }

  /* ACK is added last so that we don't send ACK only frame here. */
  ackfr = NULL;
  /* TODO Is it better to check the remaining space in packet? */
  rv = conn_create_ack_frame(conn, &ackfr, &pktns->acktr, ts, 1 /* nodelay */,
                             conn->local_settings.ack_delay_exponent);
  if (rv != 0) {
    return rv;
  }
  if (ackfr) {
    rv = conn_ppe_write_frame(conn, &ppe, &hd, ackfr);
    if (rv != 0) {
      ngtcp2_mem_free(conn->mem, ackfr);
      if (rv != NGTCP2_ERR_NOBUF) {
        return rv;
      }
    } else {
      ngtcp2_acktr_commit_ack(&pktns->acktr);
      ngtcp2_acktr_add_ack(&pktns->acktr, hd.pkt_num, &ackfr->ack, ts,
                           0 /* ack_only */);
    }
  }

  assert(!*pfrc);

  ent->hd = hd;

  if (require_padding || (ent->flags & NGTCP2_RTB_FLAG_CLIENT_INITIAL)) {
    localfr.type = NGTCP2_FRAME_PADDING;
    localfr.padding.len = ngtcp2_ppe_padding(&ppe);

    ngtcp2_log_tx_fr(&conn->log, &hd, &localfr);
  }

  nwrite = ngtcp2_ppe_final(&ppe, NULL);
  if (nwrite < 0) {
    return nwrite;
  }

  ++pktns->last_tx_pkt_num;

  return nwrite;
}

/*
 * conn_retransmit_pktns writes QUIC packet in the buffer pointed by
 * |dest| whose length is |destlen| to retransmit lost packet.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOBUF
 *     Buffer is too small.
 * NGTCP2_ERR_PKT_TIMEOUT
 *     Give up the retransmission of lost packet because of timeout.
 * NGTCP2_ERR_INVALID_ARGUMENT
 *     Packet type is unexpected.  TODO: This will be removed in the
 *     future.
 */
static ssize_t conn_retransmit_pktns(ngtcp2_conn *conn, uint8_t *dest,
                                     size_t destlen, ngtcp2_pktns *pktns,
                                     ngtcp2_tstamp ts) {
  ngtcp2_rtb_entry *ent;
  ssize_t nwrite;

  /* TODO We should not retransmit packets after we are in closing, or
     draining state */
  for (;;) {
    ent = ngtcp2_rtb_lost_head(&pktns->rtb);
    if (ent == NULL) {
      return 0;
    }

    ngtcp2_rtb_lost_pop(&pktns->rtb);

    if (ent->hd.flags & NGTCP2_PKT_FLAG_LONG_FORM) {
      switch (ent->hd.type) {
      case NGTCP2_PKT_INITIAL:
      case NGTCP2_PKT_0RTT_PROTECTED:
      case NGTCP2_PKT_HANDSHAKE:
        /* TODO find a way to stop transmitting handshake packet */
        nwrite = conn_retransmit_pkt(conn, dest, destlen, pktns, ent,
                                     0 /* require_padding */, ts);
        break;
      default:
        assert(0);
      }
    } else {
      nwrite = conn_retransmit_pkt(conn, dest, destlen, pktns, ent,
                                   0 /* require_padding */, ts);
    }

    if (nwrite <= 0) {
      if (nwrite == 0) {
        ngtcp2_rtb_entry_del(ent, conn->mem);
        continue;
      }
      if (nwrite == NGTCP2_ERR_NOBUF) {
        /* TODO we might stack here if the same insufficiently small
           buffer size is specified repeatedly. */
        ngtcp2_rtb_lost_insert(&pktns->rtb, ent);
        return nwrite;
      }

      ngtcp2_rtb_entry_del(ent, conn->mem);
      return nwrite;
    }

    /* No retransmittable frame was written, and now ent is empty. */
    if (ent->frc == NULL) {
      ngtcp2_rtb_entry_del(ent, conn->mem);
      return nwrite;
    }

    ent->pktlen = (size_t)nwrite;
    ent->ts = ts;
    conn_on_pkt_sent(conn, &pktns->rtb, ent);

    return nwrite;
  }
}

/*
 * conn_cwnd_left returns the number of bytes the local endpoint can
 * sent at this time.
 */
static uint64_t conn_cwnd_left(ngtcp2_conn *conn) {
  uint64_t bytes_in_flight = ngtcp2_conn_get_bytes_in_flight(conn);

  /* We might send more than bytes_in_flight if TLP/RTO packets are
     involved. */
  if (bytes_in_flight >= conn->ccs.cwnd) {
    return 0;
  }
  return conn->ccs.cwnd - bytes_in_flight;
}

/*
 * conn_retransmit_retry_early retransmits 0RTT Protected packet after
 * Retry is received from server.
 */
static ssize_t conn_retransmit_retry_early(ngtcp2_conn *conn, uint8_t *dest,
                                           size_t destlen, int require_padding,
                                           ngtcp2_tstamp ts) {
  ngtcp2_rtb_entry *ent;
  ssize_t nwrite;

  if (!conn->early_ckm) {
    return 0;
  }

  for (;;) {
    ent = conn->retry_early_rtb;
    if (!ent) {
      return 0;
    }
    conn->retry_early_rtb = ent->next;

    /* DCID might be changed on Retry */
    ent->hd.dcid = conn->dcid;

    nwrite = conn_retransmit_pkt(conn, dest, destlen, &conn->pktns, ent,
                                 require_padding, ts);
    if (nwrite != 0) {
      if (nwrite < 0) {
        if (ngtcp2_err_is_fatal((int)nwrite)) {
          ngtcp2_rtb_entry_del(ent, conn->mem);

          return nwrite;
        }
      } else {
        ent->pktlen = (size_t)nwrite;
        ent->ts = ts;
      }

      conn_on_pkt_sent(conn, &conn->pktns.rtb, ent);

      return nwrite;
    }
  }
}

/*
 * conn_retransmit performs retransmission.  It covers both handshake
 * and regular packets.
 */
static ssize_t conn_retransmit(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                               ngtcp2_tstamp ts) {
  ssize_t nwrite;

  nwrite = conn_retransmit_pktns(conn, dest, destlen, &conn->in_pktns, ts);
  if (nwrite) {
    return nwrite;
  }

  nwrite = conn_retransmit_pktns(conn, dest, destlen, &conn->hs_pktns, ts);
  if (nwrite) {
    return nwrite;
  }

  return conn_retransmit_pktns(conn, dest, destlen, &conn->pktns, ts);
}

/*
 * conn_create_crypto_frame creates CRYPTO frame.  CRYPTO frame is
 * allocated along with ngtcp2_frame_chain with possibly extra memory
 * to hold the scattered data.  |pkt| is the type of packet.  If |pkt|
 * is 0, Short packet is used.  Otherwise it indicates the type of
 * long packet.  |left| is the number of bytes left to write data.
 * The sum of CRYPTO stream data and any overhead of CRYPTO stream
 * must be less than or equal to |left|.
 *
 * This function returns the number of CRYPTO stream data consumed by
 * the created CRYPTO frame, or one of the following negative error
 * codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static ssize_t conn_create_crypto_frame(ngtcp2_conn *conn,
                                        ngtcp2_frame_chain **pfrc,
                                        ngtcp2_pktns *pktns, uint8_t type,
                                        size_t left) {
  size_t i;
  size_t datacnt = 0;
  ngtcp2_crypto_data *cdata;
  int rv;
  ngtcp2_crypto *fr;
  ngtcp2_ringbuf *rb = &conn->tx_crypto_data;
  size_t origleft = left;
  size_t nwrite = 0;
  size_t datalen;

  for (i = 0; left && i < ngtcp2_ringbuf_len(rb) &&
              datacnt < NGTCP2_MAX_CRYPTO_DATACNT;
       ++i, ++datacnt) {
    cdata = ngtcp2_ringbuf_get(rb, i);
    if (cdata->pkt_type != type) {
      break;
    }
    datalen = ngtcp2_buf_len(&cdata->buf);
    if (datalen < left) {
      nwrite += datalen;
      left -= datalen;
    } else {
      nwrite += left;
      left = 0;
    }
  }

  if (nwrite == 0) {
    return 0;
  }

  rv = ngtcp2_frame_chain_extralen_new(pfrc, sizeof(ngtcp2_vec) * (datacnt - 1),
                                       conn->mem);
  if (rv != 0) {
    return (ssize_t)rv;
  }

  fr = &(*pfrc)->fr.crypto;

  fr->type = NGTCP2_FRAME_CRYPTO;
  fr->ordered_offset = conn->crypto.tx_offset;
  fr->offset = pktns->crypto_tx_offset;
  fr->datacnt = datacnt;

  left = origleft;

  for (i = 0; i < datacnt; ++i) {
    cdata = ngtcp2_ringbuf_get(rb, 0);
    assert(cdata->pkt_type == type);

    datalen = ngtcp2_buf_len(&cdata->buf);
    fr->data[i].base = cdata->buf.pos;

    if (datalen <= left) {
      fr->data[i].len = datalen;
      left -= datalen;

      ngtcp2_ringbuf_pop_front(rb);
    } else {
      fr->data[i].len = left;
      cdata->buf.pos += left;

      assert(i == datacnt - 1);
      assert(ngtcp2_buf_len(&cdata->buf));
    }
  }

  return (ssize_t)nwrite;
}

/*
 * conn_retry_early_payloadlen returns the estimated wire length of
 * the first 0-RTT packet which should be retransmitted due to Retry
 * frame
 */
static size_t conn_retry_early_payloadlen(ngtcp2_conn *conn) {
  ngtcp2_rtb_entry *ent = conn->retry_early_rtb;
  ngtcp2_frame_chain *frc;
  size_t len = 0;

  if (!ent) {
    return 0;
  }

  for (frc = ent->frc; frc; frc = frc->next) {
    assert(frc->fr.type == NGTCP2_FRAME_STREAM);
    len += 1 /* Type */ + frc->fr.stream.datalen +
           8 * 3 /* Stream ID, Offset, and Length */;
  }

  return len;
}

/*
 * conn_write_handshake_pkt writes handshake packet in the buffer
 * pointed by |dest| whose length is |destlen|.  |type| specifies long
 * packet type.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOBUF
 *     Buffer is too small.
 */
static ssize_t conn_write_handshake_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                        size_t destlen, uint8_t type,
                                        int require_padding, ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_ppe ppe;
  ngtcp2_pkt_hd hd;
  ngtcp2_frame_chain *frc = NULL, **pfrc, *frc_head = NULL, *frc_next;
  ngtcp2_frame *ackfr, lfr;
  ssize_t nwrite;
  ssize_t spktlen;
  ngtcp2_crypto_ctx ctx;
  ngtcp2_rtb_entry *rtbent;
  ngtcp2_acktr_ack_entry *ack_ent = NULL;
  size_t pclen;
  int pr_encoded = 0;
  ngtcp2_path_challenge_entry *pc;
  ngtcp2_pktns *pktns;
  size_t left;
  ngtcp2_ringbuf *rb = &conn->tx_crypto_data;
  size_t min_payloadlen;
  uint8_t flags = NGTCP2_RTB_FLAG_NONE;

  if (ngtcp2_ringbuf_len(rb) == 0) {
    return 0;
  }

  pfrc = &frc_head;

  switch (type) {
  case NGTCP2_PKT_INITIAL:
    assert(conn->in_pktns.tx_ckm);
    pktns = &conn->in_pktns;
    ctx.ckm = pktns->tx_ckm;
    ctx.aead_overhead = NGTCP2_INITIAL_AEAD_OVERHEAD;
    ctx.encrypt = conn->callbacks.in_encrypt;
    ctx.encrypt_pn = conn->callbacks.in_encrypt_pn;
    break;
  case NGTCP2_PKT_HANDSHAKE:
    assert(conn->hs_pktns.tx_ckm);
    pktns = &conn->hs_pktns;
    ctx.ckm = pktns->tx_ckm;
    ctx.aead_overhead = conn->aead_overhead;
    ctx.encrypt = conn->callbacks.encrypt;
    ctx.encrypt_pn = conn->callbacks.encrypt_pn;
    ctx.user_data = conn;
    break;
  case NGTCP2_PKT_0RTT_PROTECTED:
    assert(conn->early_ckm);
    pktns = &conn->pktns;
    ctx.ckm = conn->early_ckm;
    ctx.aead_overhead = conn->aead_overhead;
    ctx.encrypt = conn->callbacks.encrypt;
    ctx.encrypt_pn = conn->callbacks.encrypt_pn;
    ctx.user_data = conn;
    flags = NGTCP2_RTB_FLAG_0RTT;
    break;
  default:
    assert(0);
  }

  ngtcp2_pkt_hd_init(
      &hd, NGTCP2_PKT_FLAG_LONG_FORM, type, &conn->dcid, &conn->scid,
      pktns->last_tx_pkt_num + 1,
      rtb_select_pkt_numlen(&pktns->rtb, pktns->last_tx_pkt_num + 1),
      conn->version, 0);

  if (type == NGTCP2_PKT_INITIAL && conn->state == NGTCP2_CS_CLIENT_INITIAL) {
    flags |= NGTCP2_RTB_FLAG_CLIENT_INITIAL;

    if (ngtcp2_buf_len(&conn->token)) {
      hd.token = conn->token.pos;
      hd.tokenlen = ngtcp2_buf_len(&conn->token);
    }
  }

  ctx.user_data = conn;

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx);

  ngtcp2_log_tx_pkt_hd(&conn->log, &hd);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    return rv;
  }

  if (type != NGTCP2_PKT_0RTT_PROTECTED &&
      conn->state != NGTCP2_CS_CLIENT_INITIAL) {
    ackfr = NULL;
    rv = conn_create_ack_frame(conn, &ackfr, &pktns->acktr, ts, 0 /* nodelay */,
                               NGTCP2_DEFAULT_ACK_DELAY_EXPONENT);
    if (rv != 0) {
      return rv;
    }
    if (ackfr) {
      rv = ngtcp2_ppe_encode_frame(&ppe, ackfr);
      if (rv != 0) {
        ngtcp2_mem_free(conn->mem, ackfr);
        return rv;
      }

      ngtcp2_log_tx_fr(&conn->log, &hd, ackfr);

      ngtcp2_acktr_commit_ack(&pktns->acktr);

      ack_ent = ngtcp2_acktr_add_ack(&pktns->acktr, hd.pkt_num, &ackfr->ack, ts,
                                     0 /* ack_only */);
    }
  }

  if (type != NGTCP2_PKT_INITIAL && type != NGTCP2_PKT_0RTT_PROTECTED) {
    if (!conn->server) {
      pclen = ngtcp2_ringbuf_len(&conn->rx_path_challenge);
      while (ngtcp2_ringbuf_len(&conn->rx_path_challenge)) {
        pc = ngtcp2_ringbuf_get(&conn->rx_path_challenge, 0);

        lfr.type = NGTCP2_FRAME_PATH_RESPONSE;
        ngtcp2_cpymem(lfr.path_challenge.data, pc->data,
                      sizeof(lfr.path_challenge.data));

        rv = ngtcp2_ppe_encode_frame(&ppe, &lfr);
        if (rv != 0) {
          if (rv == NGTCP2_ERR_NOBUF) {
            break;
          }
          return rv;
        }

        ngtcp2_log_tx_fr(&conn->log, &hd, &lfr);

        ngtcp2_ringbuf_pop_front(&conn->rx_path_challenge);
      }

      pr_encoded = (pclen != ngtcp2_ringbuf_len(&conn->rx_path_challenge));
    }

    if (ngtcp2_ppe_left(&ppe) <
        NGTCP2_CRYPTO_OVERHEAD + NGTCP2_MIN_FRAME_PAYLOADLEN) {
      spktlen = ngtcp2_ppe_final(&ppe, NULL);
      if (spktlen < 0) {
        return (int)spktlen;
      }

      if (pr_encoded) {
        rv = ngtcp2_rtb_entry_new(&rtbent, &hd, NULL, ts, (size_t)spktlen,
                                  flags, conn->mem);
        if (rv != 0) {
          return rv;
        }

        conn_on_pkt_sent(conn, &pktns->rtb, rtbent);
      }

      ++pktns->last_tx_pkt_num;

      return spktlen;
    }
  }

  if (ngtcp2_ppe_left(&ppe) <
      NGTCP2_CRYPTO_OVERHEAD + NGTCP2_MIN_FRAME_PAYLOADLEN) {
    left = 0;
  } else {
    left = ngtcp2_ppe_left(&ppe) - NGTCP2_CRYPTO_OVERHEAD;

    nwrite = conn_create_crypto_frame(conn, &frc, pktns, type, left);
    if (nwrite < 0) {
      goto fail;
    }

    if (nwrite) {
      conn->crypto.tx_offset += (size_t)nwrite;
      pktns->crypto_tx_offset += (size_t)nwrite;

      *pfrc = frc;
      pfrc = &frc->next;

      rv = ngtcp2_ppe_encode_frame(&ppe, &frc->fr);
      if (rv != 0) {
        assert(rv == NGTCP2_ERR_NOBUF);
      }

      ngtcp2_log_tx_fr(&conn->log, &hd, &frc->fr);
    }
  }

  /* If we cannot write another packet, then we need to add padding to
     Initial here. */
  if (conn->state == NGTCP2_CS_CLIENT_INITIAL) {
    min_payloadlen = ngtcp2_max(conn_retry_early_payloadlen(conn), 128);

    if (!conn->early_ckm || require_padding ||
        ngtcp2_ppe_left(&ppe) <
            /* TODO Assuming that pkt_num is encoded in 1 byte. */
            NGTCP2_MIN_LONG_HEADERLEN + conn->dcid.datalen +
                conn->scid.datalen + 1 /* payloadlen bytes - 1 */ +
                min_payloadlen + NGTCP2_MAX_AEAD_OVERHEAD) {
      lfr.type = NGTCP2_FRAME_PADDING;
      lfr.padding.len = ngtcp2_ppe_padding(&ppe);
      if (lfr.padding.len > 0) {
        ngtcp2_log_tx_fr(&conn->log, &hd, &lfr);
      }
    }
  }

  if (!frc_head && !pr_encoded && !ack_ent) {
    return 0;
  }

  spktlen = ngtcp2_ppe_final(&ppe, NULL);
  if (spktlen < 0) {
    rv = (int)spktlen;
    goto fail;
  }

  if (frc_head || pr_encoded) {
    rv = ngtcp2_rtb_entry_new(&rtbent, &hd, frc_head, ts, (size_t)spktlen,
                              flags, conn->mem);
    if (rv != 0) {
      goto fail;
    }

    conn_on_pkt_sent(conn, &pktns->rtb, rtbent);
  } else if (ack_ent) {
    ack_ent->ack_only = 1;
  }

  ++pktns->last_tx_pkt_num;

  return spktlen;

fail:
  for (frc = frc_head; frc;) {
    frc_next = frc->next;
    ngtcp2_frame_chain_del(frc, conn->mem);
    frc = frc_next;
  }
  return rv;
}

/*
 * conn_write_handshake_ack_pkt writes unprotected QUIC packet in the
 * buffer pointed by |dest| whose length is |destlen|.  The packet
 * only includes ACK frame if any ack is required.
 *
 * If there is no ACK frame to send, this function returns 0.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOBUF
 *     Buffer is too small.
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static ssize_t conn_write_handshake_ack_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                            size_t destlen, ngtcp2_pktns *pktns,
                                            ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_ppe ppe;
  ngtcp2_pkt_hd hd;
  ngtcp2_frame *ackfr;
  ngtcp2_crypto_ctx ctx;
  uint8_t type;

  if (!pktns->tx_ckm) {
    return 0;
  }

  ackfr = NULL;
  rv = conn_create_ack_frame(conn, &ackfr, &pktns->acktr, ts, 0 /* nodelay */,
                             NGTCP2_DEFAULT_ACK_DELAY_EXPONENT);
  if (rv != 0) {
    return rv;
  }
  if (!ackfr) {
    return 0;
  }

  if (pktns == &conn->in_pktns) {
    ctx.aead_overhead = NGTCP2_INITIAL_AEAD_OVERHEAD;
    ctx.encrypt = conn->callbacks.in_encrypt;
    ctx.encrypt_pn = conn->callbacks.in_encrypt_pn;
    type = NGTCP2_PKT_INITIAL;
  } else {
    assert(pktns == &conn->hs_pktns);
    ctx.aead_overhead = conn->aead_overhead;
    ctx.encrypt = conn->callbacks.encrypt;
    ctx.encrypt_pn = conn->callbacks.encrypt_pn;
    type = NGTCP2_PKT_HANDSHAKE;
  }

  ngtcp2_pkt_hd_init(
      &hd, NGTCP2_PKT_FLAG_LONG_FORM, type, &conn->dcid, &conn->scid,
      pktns->last_tx_pkt_num + 1,
      rtb_select_pkt_numlen(&pktns->rtb, pktns->last_tx_pkt_num + 1),
      conn->version, 0);

  ctx.ckm = pktns->tx_ckm;
  ctx.user_data = conn;

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx);

  ngtcp2_log_tx_pkt_hd(&conn->log, &hd);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    goto fail;
  }

  rv = ngtcp2_ppe_encode_frame(&ppe, ackfr);
  if (rv != 0) {
    goto fail;
  }

  ngtcp2_log_tx_fr(&conn->log, &hd, ackfr);

  ngtcp2_acktr_commit_ack(&pktns->acktr);

  ngtcp2_acktr_add_ack(&pktns->acktr, hd.pkt_num, &ackfr->ack, ts,
                       1 /* ack_only*/);

  ++pktns->last_tx_pkt_num;

  return ngtcp2_ppe_final(&ppe, NULL);

fail:
  ngtcp2_mem_free(conn->mem, ackfr);
  return rv;
}

/*
 * conn_write_handshake_ack_pkts writes packets which contain ACK
 * frame only.  This function writes at most 2 packets for each
 * Initial and Handshake packet.
 */
static ssize_t conn_write_handshake_ack_pkts(ngtcp2_conn *conn, uint8_t *dest,
                                             size_t destlen, ngtcp2_tstamp ts) {
  ssize_t in_nwrite, hs_nwrite;

  in_nwrite =
      conn_write_handshake_ack_pkt(conn, dest, destlen, &conn->in_pktns, ts);
  if (in_nwrite < 0) {
    return in_nwrite;
  }

  dest += in_nwrite;
  destlen -= (size_t)in_nwrite;

  hs_nwrite =
      conn_write_handshake_ack_pkt(conn, dest, destlen, &conn->hs_pktns, ts);
  if (hs_nwrite < 0) {
    if (ngtcp2_err_is_fatal((int)hs_nwrite)) {
      return hs_nwrite;
    }
    if (in_nwrite) {
      return in_nwrite;
    }
    return hs_nwrite;
  }

  return in_nwrite + hs_nwrite;
}

/*
 * conn_write_client_initial writes Initial packet in the buffer
 * pointed by |dest| whose length is |destlen|.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOBUF
 *     Buffer is too small.
 */
static ssize_t conn_write_client_initial(ngtcp2_conn *conn, uint8_t *dest,
                                         size_t destlen, int require_padding,
                                         ngtcp2_tstamp ts) {
  ngtcp2_ringbuf *rb = &conn->tx_crypto_data;
  ngtcp2_crypto_data *cdata;
  int rv;

  rv = conn->callbacks.client_initial(conn, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  if (ngtcp2_ringbuf_len(rb) == 0) {
    return NGTCP2_ERR_INTERNAL;
  }

  cdata = ngtcp2_ringbuf_get(rb, 0);
  if (cdata->pkt_type != NGTCP2_PKT_INITIAL) {
    return NGTCP2_ERR_INTERNAL;
  }

  return conn_write_handshake_pkt(conn, dest, destlen, NGTCP2_PKT_INITIAL,
                                  require_padding, ts);
}

/*
 * conn_write_client_handshake writes Handshake packet in the buffer
 * pointed by |dest| whose length is |destlen|.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOBUF
 *     Buffer is too small.
 */
static ssize_t conn_write_client_handshake(ngtcp2_conn *conn, uint8_t *dest,
                                           size_t destlen, int require_padding,
                                           ngtcp2_tstamp ts) {
  ngtcp2_ringbuf *rb = &conn->tx_crypto_data;
  ngtcp2_crypto_data *cdata;
  ssize_t nwrite;
  ssize_t res = 0;

  for (;;) {
    if (ngtcp2_ringbuf_len(rb) == 0) {
      return res;
    }

    cdata = ngtcp2_ringbuf_get(rb, 0);

    switch (cdata->pkt_type) {
    case NGTCP2_PKT_INITIAL:
      break;
    case NGTCP2_PKT_HANDSHAKE:
    case NGTCP2_PKT_0RTT_PROTECTED:
      require_padding = 0;
      break;
    default:
      assert(0);
    }

    nwrite = conn_write_handshake_pkt(conn, dest, destlen, cdata->pkt_type,
                                      require_padding, ts);
    if (nwrite < 0) {
      if (nwrite != NGTCP2_ERR_NOBUF) {
        return nwrite;
      }
      if (res) {
        return res;
      }
      return NGTCP2_ERR_NOBUF;
    }
    if (nwrite == 0) {
      return res;
    }

    res += nwrite;
    dest += nwrite;
    destlen -= (size_t)nwrite;
  }
}

static ssize_t conn_write_protected_ack_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                            size_t destlen, ngtcp2_tstamp ts);

/*
 * conn_write_server_handshake writes Handshake packet in the buffer
 * pointed by |dest| whose length is |destlen|.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOBUF
 *     Buffer is too small.
 */
static ssize_t conn_write_server_handshake(ngtcp2_conn *conn, uint8_t *dest,
                                           size_t destlen, ngtcp2_tstamp ts) {
  ngtcp2_ringbuf *rb = &conn->tx_crypto_data;
  ngtcp2_crypto_data *cdata;
  ssize_t nwrite;
  ssize_t res = 0;
  ngtcp2_ksl_it it;

  for (;;) {
    if (ngtcp2_ringbuf_len(rb) == 0) {
      it = ngtcp2_acktr_get(&conn->pktns.acktr);
      if (conn->early_ckm && !ngtcp2_ksl_it_end(&it)) {
        assert(conn->pktns.tx_ckm);

        nwrite = conn_write_protected_ack_pkt(conn, dest, destlen, ts);
        if (nwrite < 0) {
          if (nwrite != NGTCP2_ERR_NOBUF) {
            return nwrite;
          }
          if (res) {
            return res;
          }
          return NGTCP2_ERR_NOBUF;
        }

        res += nwrite;
      }

      return res;
    }

    cdata = ngtcp2_ringbuf_get(rb, 0);

    switch (cdata->pkt_type) {
    case NGTCP2_PKT_INITIAL:
    case NGTCP2_PKT_HANDSHAKE:
      break;
    default:
      assert(0);
    }

    nwrite = conn_write_handshake_pkt(conn, dest, destlen, cdata->pkt_type,
                                      0 /* require_padding */, ts);
    if (nwrite < 0) {
      if (nwrite != NGTCP2_ERR_NOBUF) {
        return nwrite;
      }
      if (res) {
        return res;
      }
      return NGTCP2_ERR_NOBUF;
    }
    if (nwrite == 0) {
      return res;
    }

    res += nwrite;
    dest += nwrite;
    destlen -= (size_t)nwrite;
  }
}

/*
 * conn_initial_stream_rx_offset returns the initial maximum offset of
 * data for a stream denoted by |stream_id|.
 */
static uint64_t conn_initial_stream_rx_offset(ngtcp2_conn *conn,
                                              uint64_t stream_id) {
  int local_stream = conn_local_stream(conn, stream_id);

  if (bidi_stream(stream_id)) {
    if (local_stream) {
      return conn->local_settings.max_stream_data_bidi_local;
    }
    return conn->local_settings.max_stream_data_bidi_remote;
  }

  if (local_stream) {
    return 0;
  }
  return conn->local_settings.max_stream_data_uni;
}

/*
 * conn_should_send_max_stream_data returns nonzero if MAX_STREAM_DATA
 * frame should be send for |strm|.
 */
static int conn_should_send_max_stream_data(ngtcp2_conn *conn,
                                            ngtcp2_strm *strm) {

  return conn_initial_stream_rx_offset(conn, strm->stream_id) / 2 <
             (strm->unsent_max_rx_offset - strm->max_rx_offset) ||
         2 * conn->rx_bw * conn->rcs.smoothed_rtt >=
             strm->max_rx_offset - strm->last_rx_offset;
}

/*
 * conn_should_send_max_data returns nonzero if MAX_DATA frame should
 * be sent.
 */
static int conn_should_send_max_data(ngtcp2_conn *conn) {
  return conn->local_settings.max_data / 2 <
             conn->unsent_max_rx_offset - conn->max_rx_offset ||
         2 * conn->rx_bw * conn->rcs.smoothed_rtt >=
             conn->max_rx_offset - conn->rx_offset;
}

/*
 * conn_write_pkt writes a protected packet in the buffer pointed by
 * |dest| whose length if |destlen|.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOBUF
 *     Buffer is too small.
 */
static ssize_t conn_write_pkt(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                              ssize_t *pdatalen, ngtcp2_strm *data_strm,
                              uint8_t fin, const uint8_t *data, size_t datalen,
                              ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_ppe ppe;
  ngtcp2_pkt_hd hd;
  ngtcp2_frame *ackfr;
  ssize_t nwrite;
  ngtcp2_crypto_ctx ctx;
  ngtcp2_frame_chain **pfrc, *nfrc, *frc;
  ngtcp2_rtb_entry *ent;
  ngtcp2_strm *strm, *strm_next;
  int pkt_empty = 1;
  ngtcp2_acktr_ack_entry *ack_ent = NULL;
  size_t ndatalen = 0;
  int send_stream = 0;
  int ack_only;
  ngtcp2_pktns *pktns = &conn->pktns;
  size_t left;

  if (data_strm) {
    ndatalen =
        ngtcp2_min(datalen, data_strm->max_tx_offset - data_strm->tx_offset);
    ndatalen = ngtcp2_min(ndatalen, conn->max_tx_offset - conn->tx_offset);
    if (ndatalen || (datalen == 0 && fin)) {
      send_stream = 1;
    }
  }

  if ((conn->frq || send_stream || conn_should_send_max_data(conn) ||
       ngtcp2_ringbuf_len(&conn->tx_crypto_data)) &&
      conn->unsent_max_rx_offset > conn->max_rx_offset) {
    rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
    if (rv != 0) {
      return rv;
    }
    nfrc->fr.type = NGTCP2_FRAME_MAX_DATA;
    nfrc->fr.max_data.max_data = conn->unsent_max_rx_offset;
    nfrc->next = conn->frq;
    conn->frq = nfrc;

    conn->max_rx_offset = conn->unsent_max_rx_offset;
  }

  while (conn->fc_strms) {
    strm = conn->fc_strms;
    rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
    if (rv != 0) {
      return rv;
    }
    nfrc->fr.type = NGTCP2_FRAME_MAX_STREAM_DATA;
    nfrc->fr.max_stream_data.stream_id = strm->stream_id;
    nfrc->fr.max_stream_data.max_stream_data = strm->unsent_max_rx_offset;
    nfrc->next = conn->frq;
    conn->frq = nfrc;

    strm->max_rx_offset = strm->unsent_max_rx_offset;

    strm_next = strm->fc_next;
    conn->fc_strms = strm_next;
    if (strm_next) {
      strm_next->fc_pprev = &conn->fc_strms;
    }
    strm->fc_next = NULL;
    strm->fc_pprev = NULL;
  }

  ack_only =
      !send_stream && ngtcp2_ringbuf_len(&conn->tx_crypto_data) == 0 &&
      conn->unsent_max_remote_stream_id_bidi ==
          conn->max_remote_stream_id_bidi &&
      conn->unsent_max_remote_stream_id_uni == conn->max_remote_stream_id_uni &&
      conn->frq == NULL;

  if (ack_only && conn->rcs.probe_pkt_left) {
    /* Sending ACK only packet does not elicit ACK, therefore it is
       not suitable for probe packet. */
    return 0;
  }

  ackfr = NULL;
  rv = conn_create_ack_frame(conn, &ackfr, &pktns->acktr, ts,
                             !ack_only /* nodelay */,
                             conn->local_settings.ack_delay_exponent);
  if (rv != 0) {
    return rv;
  }

  if (ackfr == NULL && ack_only) {
    return 0;
  }

  ngtcp2_pkt_hd_init(
      &hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_SHORT, &conn->dcid, &conn->scid,
      pktns->last_tx_pkt_num + 1,
      rtb_select_pkt_numlen(&pktns->rtb, pktns->last_tx_pkt_num + 1),
      conn->version, 0);

  ctx.ckm = pktns->tx_ckm;
  ctx.aead_overhead = conn->aead_overhead;
  ctx.encrypt = conn->callbacks.encrypt;
  ctx.encrypt_pn = conn->callbacks.encrypt_pn;
  ctx.user_data = conn;

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx);

  ngtcp2_log_tx_pkt_hd(&conn->log, &hd);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    goto fail;
  }

  if (ackfr) {
    rv = conn_ppe_write_frame(conn, &ppe, &hd, ackfr);
    if (rv != 0) {
      goto fail;
    }
    ngtcp2_acktr_commit_ack(&pktns->acktr);
    pkt_empty = 0;

    ack_ent = ngtcp2_acktr_add_ack(&pktns->acktr, hd.pkt_num, &ackfr->ack, ts,
                                   0 /*ack_only*/);
    /* Now ackfr is owned by conn->acktr. */
    ackfr = NULL;
  }

  for (pfrc = &conn->frq; *pfrc;) {
    switch ((*pfrc)->fr.type) {
    case NGTCP2_FRAME_RST_STREAM:
      strm = ngtcp2_conn_find_stream(conn, (*pfrc)->fr.rst_stream.stream_id);
      if (strm == NULL &&
          (*pfrc)->fr.rst_stream.app_error_code != NGTCP2_STOPPING) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    case NGTCP2_FRAME_STOP_SENDING:
      strm = ngtcp2_conn_find_stream(conn, (*pfrc)->fr.stop_sending.stream_id);
      if (strm == NULL || (strm->flags & NGTCP2_STRM_FLAG_SHUT_RD)) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    }

    rv = conn_ppe_write_frame(conn, &ppe, &hd, &(*pfrc)->fr);
    if (rv != 0) {
      assert(NGTCP2_ERR_NOBUF == rv);
      break;
    }

    pkt_empty = 0;
    pfrc = &(*pfrc)->next;
  }

  /* Write MAX_STREAM_ID after RST_STREAM so that we can extend stream
     ID space in one packet. */
  if (rv != NGTCP2_ERR_NOBUF && *pfrc == NULL &&
      conn->unsent_max_remote_stream_id_bidi >
          conn->max_remote_stream_id_bidi) {
    rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
    if (rv != 0) {
      return rv;
    }
    nfrc->fr.type = NGTCP2_FRAME_MAX_STREAM_ID;
    nfrc->fr.max_stream_id.max_stream_id =
        conn->unsent_max_remote_stream_id_bidi;
    *pfrc = nfrc;

    conn->max_remote_stream_id_bidi = conn->unsent_max_remote_stream_id_bidi;

    rv = conn_ppe_write_frame(conn, &ppe, &hd, &(*pfrc)->fr);
    if (rv != 0) {
      assert(NGTCP2_ERR_NOBUF == rv);
    } else {
      pkt_empty = 0;
      pfrc = &(*pfrc)->next;
    }
  }

  if (rv != NGTCP2_ERR_NOBUF && *pfrc == NULL &&
      conn->unsent_max_remote_stream_id_uni > conn->max_remote_stream_id_uni) {
    rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
    if (rv != 0) {
      return rv;
    }
    nfrc->fr.type = NGTCP2_FRAME_MAX_STREAM_ID;
    nfrc->fr.max_stream_id.max_stream_id =
        conn->unsent_max_remote_stream_id_uni;
    *pfrc = nfrc;

    conn->max_remote_stream_id_uni = conn->unsent_max_remote_stream_id_uni;

    rv = conn_ppe_write_frame(conn, &ppe, &hd, &(*pfrc)->fr);
    if (rv != 0) {
      assert(NGTCP2_ERR_NOBUF == rv);
    } else {
      pkt_empty = 0;
      pfrc = &(*pfrc)->next;
    }
  }

  left = ngtcp2_ppe_left(&ppe);

  if (rv != NGTCP2_ERR_NOBUF && *pfrc == NULL &&
      left >= NGTCP2_CRYPTO_OVERHEAD + NGTCP2_MIN_FRAME_PAYLOADLEN) {
    left -= NGTCP2_CRYPTO_OVERHEAD;

    nwrite = conn_create_crypto_frame(conn, &nfrc, pktns, 0 /* Short packet */
                                      ,
                                      left);
    if (nwrite < 0) {
      return nwrite;
    }

    if (nwrite) {
      conn->crypto.tx_offset += (size_t)nwrite;
      pktns->crypto_tx_offset += (size_t)nwrite;

      *pfrc = nfrc;
      pfrc = &(*pfrc)->next;

      rv = conn_ppe_write_frame(conn, &ppe, &hd, &nfrc->fr);
      if (rv != 0) {
        assert(rv == NGTCP2_ERR_NOBUF);
      }

      pkt_empty = 0;

      ngtcp2_log_tx_fr(&conn->log, &hd, &nfrc->fr);
    }
  }

  left = ngtcp2_ppe_left(&ppe);

  if (rv != NGTCP2_ERR_NOBUF && *pfrc == NULL && send_stream &&
      left >= NGTCP2_STREAM_OVERHEAD + NGTCP2_MIN_FRAME_PAYLOADLEN) {
    left -= NGTCP2_STREAM_OVERHEAD;

    ndatalen = ngtcp2_min(ndatalen, left);

    fin = fin && ndatalen == datalen;

    rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
    if (rv != 0) {
      return rv;
    }

    nfrc->fr.type = NGTCP2_FRAME_STREAM;
    nfrc->fr.stream.flags = 0;
    nfrc->fr.stream.fin = fin;
    nfrc->fr.stream.stream_id = data_strm->stream_id;
    nfrc->fr.stream.offset = data_strm->tx_offset;
    nfrc->fr.stream.datalen = ndatalen;
    nfrc->fr.stream.data = data;

    rv = conn_ppe_write_frame(conn, &ppe, &hd, &nfrc->fr);
    if (rv != 0) {
      assert(NGTCP2_ERR_NOBUF == rv);
      ngtcp2_frame_chain_del(nfrc, conn->mem);
      send_stream = 0;
    } else {
      *pfrc = nfrc;
      pfrc = &(*pfrc)->next;

      pkt_empty = 0;
    }
  } else {
    send_stream = 0;
  }

  if (pkt_empty) {
    ngtcp2_log_tx_cancel(&conn->log, &hd);
    return rv;
  }

  nwrite = ngtcp2_ppe_final(&ppe, NULL);
  if (nwrite < 0) {
    return nwrite;
  }

  if (*pfrc != conn->frq) {
    rv = ngtcp2_rtb_entry_new(&ent, &hd, NULL, ts, (size_t)nwrite,
                              NGTCP2_RTB_FLAG_NONE, conn->mem);
    if (rv != 0) {
      return rv;
    }

    ent->frc = conn->frq;
    conn->frq = *pfrc;
    *pfrc = NULL;

    conn_on_pkt_sent(conn, &pktns->rtb, ent);

    if (send_stream) {
      data_strm->tx_offset += ndatalen;
      conn->tx_offset += ndatalen;

      if (fin) {
        ngtcp2_strm_shutdown(data_strm, NGTCP2_STRM_FLAG_SHUT_WR);
      }
    }
  } else if (ack_ent) {
    ack_ent->ack_only = 1;
  }

  if (pdatalen) {
    *pdatalen = (ssize_t)ndatalen;
  }

  ++pktns->last_tx_pkt_num;

  return nwrite;

fail:
  ngtcp2_mem_free(conn->mem, ackfr);
  return rv;
}

/*
 * conn_write_single_frame_pkt writes a packet which contains |fr|
 * frame only in the buffer pointed by |dest| whose length if
 * |destlen|.  |type| is a long packet type to send.  If |type| is 0,
 * Short packet is used.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOBUF
 *     Buffer is too small.
 */
static ssize_t conn_write_single_frame_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                           size_t destlen, uint8_t type,
                                           ngtcp2_frame *fr, ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_ppe ppe;
  ngtcp2_pkt_hd hd;
  ssize_t nwrite;
  ngtcp2_crypto_ctx ctx;
  ngtcp2_pktns *pktns;
  uint8_t flags;

  switch (type) {
  case NGTCP2_PKT_INITIAL:
    pktns = &conn->in_pktns;
    ctx.aead_overhead = NGTCP2_INITIAL_AEAD_OVERHEAD;
    ctx.encrypt = conn->callbacks.in_encrypt;
    ctx.encrypt_pn = conn->callbacks.in_encrypt_pn;
    flags = NGTCP2_PKT_FLAG_LONG_FORM;
    break;
  case NGTCP2_PKT_HANDSHAKE:
    pktns = &conn->hs_pktns;
    ctx.aead_overhead = conn->aead_overhead;
    ctx.encrypt = conn->callbacks.encrypt;
    ctx.encrypt_pn = conn->callbacks.encrypt_pn;
    flags = NGTCP2_PKT_FLAG_LONG_FORM;
    break;
  case 0:
    /* 0 means Short packet. */
    pktns = &conn->pktns;
    ctx.aead_overhead = conn->aead_overhead;
    ctx.encrypt = conn->callbacks.encrypt;
    ctx.encrypt_pn = conn->callbacks.encrypt_pn;
    flags = NGTCP2_PKT_FLAG_NONE;
    break;
  default:
    /* We don't support 0-RTT Protected packet in this function. */
    assert(0);
  }

  ctx.ckm = pktns->tx_ckm;
  ctx.user_data = conn;

  ngtcp2_pkt_hd_init(
      &hd, flags, type, &conn->dcid, &conn->scid, pktns->last_tx_pkt_num + 1,
      rtb_select_pkt_numlen(&pktns->rtb, pktns->last_tx_pkt_num + 1),
      conn->version, 0);

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx);

  ngtcp2_log_tx_pkt_hd(&conn->log, &hd);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    return rv;
  }

  rv = ngtcp2_ppe_encode_frame(&ppe, fr);
  if (rv != 0) {
    return rv;
  }

  ngtcp2_log_tx_fr(&conn->log, &hd, fr);

  nwrite = ngtcp2_ppe_final(&ppe, NULL);
  if (nwrite < 0) {
    return nwrite;
  }

  /* Do this when we are sure that there is no error. */
  if (fr->type == NGTCP2_FRAME_ACK) {
    ngtcp2_acktr_commit_ack(&pktns->acktr);
    ngtcp2_acktr_add_ack(&pktns->acktr, hd.pkt_num, &fr->ack, ts,
                         1 /* ack_only */);
  }

  ++pktns->last_tx_pkt_num;

  return nwrite;
}

/*
 * conn_write_protected_ack_pkt writes a protected QUIC packet which
 * only includes ACK frame in the buffer pointed by |dest| whose
 * length is |destlen|.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOBUF
 *     Buffer is too small.
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static ssize_t conn_write_protected_ack_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                            size_t destlen, ngtcp2_tstamp ts) {
  int rv;
  ssize_t spktlen;
  ngtcp2_frame *ackfr;
  ngtcp2_acktr *acktr = &conn->pktns.acktr;

  ackfr = NULL;
  rv = conn_create_ack_frame(conn, &ackfr, acktr, ts, 0 /* nodelay */,
                             conn->local_settings.ack_delay_exponent);
  if (rv != 0) {
    return rv;
  }

  if (!ackfr) {
    return 0;
  }

  spktlen = conn_write_single_frame_pkt(conn, dest, destlen, 0 /* Short */,
                                        ackfr, ts);
  if (spktlen < 0) {
    ngtcp2_mem_free(conn->mem, ackfr);
    return spktlen;
  }

  return spktlen;
}

/*
 * conn_process_early_rtb adds ngtcp2_rtb_entry pointed by
 * conn->early_rtb, which are 0-RTT packets, to conn->rtb.  If things
 * go wrong, this function deletes ngtcp2_rtb_entry pointed by
 * conn->early_rtb excluding the ones which are already added to
 * conn->rtb.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_process_early_rtb(ngtcp2_conn *conn) {
  ngtcp2_rtb_entry *ent, *next;
  ngtcp2_frame_chain *frc;
  ngtcp2_rtb *rtb = &conn->pktns.rtb;
  ngtcp2_ksl_it it;

  for (ent = conn->retry_early_rtb; ent;) {
    next = ent->next;
    frc = ent->frc;
    ent->frc = NULL;
    ngtcp2_rtb_entry_del(ent, conn->mem);

    assert(frc->next == NULL);
    frc->next = conn->frq;
    conn->frq = frc;

    ent = next;
  }

  ngtcp2_rtb_insert_range(rtb, conn->retry_early_rtb);

  for (it = ngtcp2_rtb_head(rtb); !ngtcp2_ksl_it_end(&it);
       ngtcp2_ksl_it_next(&it)) {
    ent = ngtcp2_ksl_it_get(&it);

    if ((ent->hd.flags & NGTCP2_PKT_FLAG_LONG_FORM) == 0 ||
        ent->hd.type != NGTCP2_PKT_0RTT_PROTECTED ||
        (ent->flags & NGTCP2_RTB_FLAG_0RTT)) {
      continue;
    }

    ent->hd.dcid = conn->dcid;

    /*  0-RTT packet is retransmitted as a Short packet. */
    ent->hd.flags &= (uint8_t)~NGTCP2_PKT_FLAG_LONG_FORM;
    ent->hd.type = NGTCP2_PKT_SHORT;
  }

  return 0;
}

/*
 * conn_write_probe_ping writes probe packet containing PING frame
 * (and optionally ACK frame).
 */
static ssize_t conn_write_probe_ping(ngtcp2_conn *conn, uint8_t *dest,
                                     size_t destlen, ngtcp2_tstamp ts) {
  ngtcp2_ppe ppe;
  ngtcp2_pkt_hd hd;
  ngtcp2_pktns *pktns = &conn->pktns;
  ngtcp2_crypto_ctx ctx;
  ngtcp2_frame_chain *frc = NULL;
  ngtcp2_rtb_entry *ent;
  ngtcp2_frame *ackfr = NULL;
  int rv;
  ssize_t nwrite;

  assert(pktns->tx_ckm);

  ctx.aead_overhead = conn->aead_overhead;
  ctx.encrypt = conn->callbacks.encrypt;
  ctx.encrypt_pn = conn->callbacks.encrypt_pn;
  ctx.ckm = pktns->tx_ckm;
  ctx.user_data = conn;

  ngtcp2_pkt_hd_init(
      &hd, NGTCP2_PKT_FLAG_NONE, NGTCP2_PKT_SHORT, &conn->dcid, &conn->scid,
      pktns->last_tx_pkt_num + 1,
      rtb_select_pkt_numlen(&pktns->rtb, pktns->last_tx_pkt_num + 1),
      conn->version, 0);

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx);

  ngtcp2_log_tx_pkt_hd(&conn->log, &hd);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    return rv;
  }

  rv = ngtcp2_frame_chain_new(&frc, conn->mem);
  if (rv != 0) {
    return rv;
  }

  frc->fr.type = NGTCP2_FRAME_PING;

  rv = ngtcp2_ppe_encode_frame(&ppe, &frc->fr);
  if (rv != 0) {
    goto fail;
  }

  ngtcp2_log_tx_fr(&conn->log, &hd, &frc->fr);

  rv = conn_create_ack_frame(conn, &ackfr, &pktns->acktr, ts, 1 /* nodelay */,
                             conn->local_settings.ack_delay_exponent);
  if (rv != 0) {
    goto fail;
  }

  if (ackfr) {
    rv = ngtcp2_ppe_encode_frame(&ppe, ackfr);
    if (rv != 0) {
      ngtcp2_mem_free(conn->mem, ackfr);
      if (rv != NGTCP2_ERR_NOMEM) {
        return rv;
      }
    } else {
      ngtcp2_log_tx_fr(&conn->log, &hd, ackfr);
      ngtcp2_acktr_commit_ack(&pktns->acktr);
      ngtcp2_acktr_add_ack(&pktns->acktr, hd.pkt_num, &ackfr->ack, ts,
                           0 /* ack_only */);
    }
  }

  nwrite = ngtcp2_ppe_final(&ppe, NULL);
  if (nwrite < 0) {
    rv = (int)nwrite;
    goto fail;
  }

  rv = ngtcp2_rtb_entry_new(&ent, &hd, frc, ts, (size_t)nwrite,
                            NGTCP2_RTB_FLAG_PROBE, conn->mem);
  if (rv != 0) {
    goto fail;
  }

  conn_on_pkt_sent(conn, &pktns->rtb, ent);

  ++pktns->last_tx_pkt_num;

  return nwrite;

fail:
  ngtcp2_frame_chain_del(frc, conn->mem);

  return rv;
}

static ssize_t conn_write_probe_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                    size_t destlen, ssize_t *pdatalen,
                                    ngtcp2_strm *strm, uint8_t fin,
                                    const uint8_t *data, size_t datalen,
                                    ngtcp2_tstamp ts) {
  ssize_t nwrite;

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                  "transmit probe pkt left=%zu", conn->rcs.probe_pkt_left);

  /* a probe packet is not blocked by cwnd. */
  nwrite = conn_write_pkt(conn, dest, destlen, pdatalen, strm, fin, data,
                          datalen, ts);
  if (nwrite == 0) {
    nwrite = conn_write_probe_ping(conn, dest, destlen, ts);
  }
  if (nwrite < 0) {
    return nwrite;
  }

  assert(nwrite);

  --conn->rcs.probe_pkt_left;

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON, "probe pkt size=%zd",
                  nwrite);

  return nwrite;
}

ssize_t ngtcp2_conn_write_pkt(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                              ngtcp2_tstamp ts) {
  ssize_t nwrite;
  uint64_t cwnd;
  ngtcp2_pktns *pktns = &conn->pktns;

  conn->log.last_ts = ts;

  if (pktns->last_tx_pkt_num == NGTCP2_MAX_PKT_NUM) {
    return NGTCP2_ERR_PKT_NUM_EXHAUSTED;
  }

  switch (conn->state) {
  case NGTCP2_CS_CLIENT_INITIAL:
  case NGTCP2_CS_CLIENT_WAIT_HANDSHAKE:
  case NGTCP2_CS_CLIENT_TLS_HANDSHAKE_FAILED:
  case NGTCP2_CS_SERVER_INITIAL:
  case NGTCP2_CS_SERVER_WAIT_HANDSHAKE:
  case NGTCP2_CS_SERVER_TLS_HANDSHAKE_FAILED:
    return NGTCP2_ERR_INVALID_STATE;
  case NGTCP2_CS_POST_HANDSHAKE:
    nwrite = conn_write_handshake_ack_pkts(conn, dest, destlen, ts);
    if (nwrite) {
      return nwrite;
    }

    cwnd = conn_cwnd_left(conn);

    if (cwnd >= NGTCP2_MIN_PKTLEN) {
      nwrite = conn_retransmit(conn, dest, ngtcp2_min(destlen, cwnd), ts);
      if (nwrite) {
        return nwrite;
      }
    }

    if (conn->rcs.probe_pkt_left) {
      return conn_write_probe_pkt(conn, dest, destlen, NULL, NULL, 0, NULL, 0,
                                  ts);
    }

    if (cwnd < NGTCP2_MIN_PKTLEN) {
      nwrite = conn_write_protected_ack_pkt(conn, dest, destlen, ts);
      if (nwrite) {
        return nwrite;
      }
      return NGTCP2_ERR_CONGESTION;
    }

    if (ngtcp2_rtb_lost_head(&pktns->rtb)) {
      /*
       * Failed to retransmit a packet because of congestion.  In this
       * case, just return NGTCP2_ERR_CONGESTION so that we don't add
       * extra bytes_in_flight by sending new packet.
       */
      return NGTCP2_ERR_CONGESTION;
    }

    return conn_write_pkt(conn, dest, ngtcp2_min(destlen, cwnd), NULL, NULL, 0,
                          NULL, 0, ts);
  case NGTCP2_CS_CLOSING:
    return NGTCP2_ERR_CLOSING;
  case NGTCP2_CS_DRAINING:
    return NGTCP2_ERR_DRAINING;
  default:
    return 0;
  }
}

/*
 * conn_on_version_negotiation is called when Version Negotiation
 * packet is received.  The function decodes the data in the buffer
 * pointed by |payload| whose length is |payloadlen| as Version
 * Negotiation packet payload.  The packet header is given in |hd|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_PROTO
 *     Packet payload is badly formatted.
 */
static int conn_on_version_negotiation(ngtcp2_conn *conn,
                                       const ngtcp2_pkt_hd *hd,
                                       const uint8_t *payload,
                                       size_t payloadlen) {
  uint32_t sv[16];
  uint32_t *p;
  int rv = 0;
  size_t nsv;

  if (payloadlen % sizeof(uint32_t)) {
    return NGTCP2_ERR_PROTO;
  }

  if (payloadlen > sizeof(sv)) {
    p = ngtcp2_mem_malloc(conn->mem, payloadlen);
    if (p == NULL) {
      return NGTCP2_ERR_NOMEM;
    }
  } else {
    p = sv;
  }

  /* TODO Just move to the terminal state for now in order not to send
     CONNECTION_CLOSE frame. */
  conn->state = NGTCP2_CS_DRAINING;

  nsv = ngtcp2_pkt_decode_version_negotiation(p, payload, payloadlen);

  ngtcp2_log_rx_vn(&conn->log, hd, sv, nsv);

  if (conn->callbacks.recv_version_negotiation) {
    rv = conn->callbacks.recv_version_negotiation(conn, hd, sv, nsv,
                                                  conn->user_data);
  }

  if (p != sv) {
    ngtcp2_mem_free(conn->mem, p);
  }

  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

/*
 * conn_on_retry is called when Retry packet is received.  The
 * function decodes the data in the buffer pointed by |payload| whose
 * length is |payloadlen| as Retry packet payload.  The packet header
 * is given in |hd|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_INVALID_ARGUMENT
 *     Packet payload is badly formatted.
 * NGTCP2_ERR_PROTO
 *     ODCID does not match; or Token is empty.
 * NGTCP2_ERR_TOO_MANY_RETRIES
 *     The endpoint cannot process another Retry packet.
 */
static int conn_on_retry(ngtcp2_conn *conn, const ngtcp2_pkt_hd *hd,
                         const uint8_t *payload, size_t payloadlen) {
  int rv;
  ngtcp2_pkt_retry retry;
  uint8_t *p;
  ngtcp2_rtb *rtb = &conn->pktns.rtb;
  ngtcp2_ksl_it it;
  ngtcp2_rtb_entry *ent;
  uint8_t cidbuf[sizeof(retry.odcid.data) * 2 + 1];

  if (conn->nretry >= NGTCP2_MAX_RETRIES) {
    return NGTCP2_ERR_TOO_MANY_RETRIES;
  }

  rv = ngtcp2_pkt_decode_retry(&retry, payload, payloadlen);
  if (rv != 0) {
    return rv;
  }

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT, "odcid=0x%s",
                  (const char *)ngtcp2_encode_hex(cidbuf, retry.odcid.data,
                                                  retry.odcid.datalen));

  if (!ngtcp2_cid_eq(&conn->dcid, &retry.odcid) || retry.tokenlen == 0) {
    return NGTCP2_ERR_PROTO;
  }

  /* DCID must be updated before invoking callback because client
     generates new initial keys there. */
  conn->dcid = hd->scid;

  ++conn->nretry;

  assert(conn->callbacks.recv_retry);

  rv = conn->callbacks.recv_retry(conn, hd, &retry, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  conn->state = NGTCP2_CS_CLIENT_INITIAL;

  /* Just freeing memory is dangerous because we might free twice. */

  ngtcp2_crypto_km_del(conn->early_ckm, conn->mem);
  conn->early_ckm = NULL;

  for (it = ngtcp2_rtb_head(rtb); !ngtcp2_ksl_it_end(&it);) {
    ent = ngtcp2_ksl_it_get(&it);
    /* TODO Check return value */
    ngtcp2_rtb_remove(rtb, &it, ent);
    ngtcp2_list_insert(ent, &conn->retry_early_rtb);
  }

  conn->pktns.last_tx_pkt_num = (uint64_t)-1;
  conn->pktns.crypto_tx_offset = 0;
  ngtcp2_rtb_clear(&conn->pktns.rtb);

  conn->in_pktns.last_tx_pkt_num = (uint64_t)-1;
  conn->in_pktns.crypto_tx_offset = 0;
  ngtcp2_rtb_clear(&conn->in_pktns.rtb);

  ngtcp2_ringbuf_resize(&conn->tx_crypto_data, 0);
  conn->crypto.tx_offset = 0;

  if (ngtcp2_buf_cap(&conn->token) < retry.tokenlen) {
    ngtcp2_mem_free(conn->mem, conn->token.begin);
    ngtcp2_buf_init(&conn->token, NULL, 0);
    p = ngtcp2_mem_malloc(conn->mem, retry.tokenlen);
    if (p == NULL) {
      return NGTCP2_ERR_NOMEM;
    }
    ngtcp2_buf_init(&conn->token, p, retry.tokenlen);
  }

  ngtcp2_cpymem(conn->token.begin, retry.token, retry.tokenlen);
  conn->token.pos = conn->token.begin;
  conn->token.last = conn->token.pos + retry.tokenlen;

  return 0;
}

/*
 * conn_recv_ack processes received ACK frame |fr|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 * NGTCP2_ERR_ACK_FRAME
 *     ACK frame is malformed.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 */
static int conn_recv_ack(ngtcp2_conn *conn, ngtcp2_pktns *pktns,
                         const ngtcp2_pkt_hd *hd, ngtcp2_ack *fr,
                         ngtcp2_tstamp ts) {
  int rv;

  rv = ngtcp2_pkt_validate_ack(fr);
  if (rv != 0) {
    return rv;
  }

  rv = ngtcp2_acktr_recv_ack(&pktns->acktr, fr, conn, ts);
  if (rv != 0) {
    return rv;
  }

  rv = ngtcp2_rtb_recv_ack(&pktns->rtb, fr, conn, ts);
  if (rv != 0) {
    return rv;
  }

  if (!ngtcp2_pkt_handshake_pkt(hd)) {
    conn->largest_ack = ngtcp2_max(conn->largest_ack, (int64_t)fr->largest_ack);

    rv = ngtcp2_rtb_detect_lost_pkt(&pktns->rtb, &conn->rcs, fr->largest_ack,
                                    pktns->last_tx_pkt_num, ts);
    if (rv != 0) {
      return rv;
    }
  }

  ngtcp2_conn_set_loss_detection_timer(conn);

  return 0;
}

/*
 * conn_assign_recved_ack_delay_unscaled assigns
 * fr->ack_delay_unscaled.
 */
static void assign_recved_ack_delay_unscaled(ngtcp2_ack *fr,
                                             uint8_t ack_delay_exponent) {
  fr->ack_delay_unscaled = (fr->ack_delay << ack_delay_exponent) * 1000;
}

/*
 * conn_recv_max_stream_data processes received MAX_STREAM_DATA frame
 * |fr|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_STREAM_STATE
 *     Stream ID indicates that it is a local stream, and the local
 *     endpoint has not initiated it.
 * NGTCP2_ERR_STREAM_ID
 *     Stream ID exceeds allowed limit.
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_recv_max_stream_data(ngtcp2_conn *conn,
                                     const ngtcp2_max_stream_data *fr) {
  ngtcp2_strm *strm;
  ngtcp2_idtr *idtr;
  int local_stream = conn_local_stream(conn, fr->stream_id);
  int bidi = bidi_stream(fr->stream_id);
  int rv;

  if (bidi) {
    if (local_stream) {
      if (conn->next_local_stream_id_bidi <= fr->stream_id) {
        return NGTCP2_ERR_STREAM_STATE;
      }
    } else if (conn->max_remote_stream_id_bidi < fr->stream_id) {
      return NGTCP2_ERR_STREAM_ID;
    }

    idtr = &conn->remote_bidi_idtr;
  } else {
    if (!local_stream) {
      return NGTCP2_ERR_PROTO;
    }
    if (conn->next_local_stream_id_uni <= fr->stream_id) {
      return NGTCP2_ERR_PROTO;
    }

    idtr = &conn->remote_uni_idtr;
  }

  strm = ngtcp2_conn_find_stream(conn, fr->stream_id);
  if (strm == NULL) {
    if (local_stream) {
      /* Stream has been closed. */
      return 0;
    }

    rv = ngtcp2_idtr_open(idtr, fr->stream_id);
    if (rv == NGTCP2_ERR_STREAM_IN_USE) {
      /* Stream has been closed. */
      return 0;
    }

    strm = ngtcp2_mem_malloc(conn->mem, sizeof(ngtcp2_strm));
    if (strm == NULL) {
      return NGTCP2_ERR_NOMEM;
    }
    rv = ngtcp2_conn_init_stream(conn, strm, fr->stream_id, NULL);
    if (rv != 0) {
      return rv;
    }
  }

  strm->max_tx_offset = ngtcp2_max(strm->max_tx_offset, fr->max_stream_data);

  return 0;
}

/*
 * conn_recv_max_data processes received MAX_DATA frame |fr|.
 */
static void conn_recv_max_data(ngtcp2_conn *conn, const ngtcp2_max_data *fr) {
  conn->max_tx_offset = ngtcp2_max(conn->max_tx_offset, fr->max_data);
}

static int conn_buffer_pkt(ngtcp2_conn *conn, ngtcp2_pkt_chain **ppc,
                           const uint8_t *pkt, size_t pktlen,
                           ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_pkt_chain *pc;
  size_t i;
  for (i = 0; *ppc && i < NGTCP2_MAX_NUM_BUFFED_RX_PKTS;
       ppc = &(*ppc)->next, ++i)
    ;

  if (i == NGTCP2_MAX_NUM_BUFFED_RX_PKTS) {
    return 0;
  }

  rv = ngtcp2_pkt_chain_new(&pc, pkt, pktlen, ts, conn->mem);
  if (rv != 0) {
    return rv;
  }

  *ppc = pc;

  return 0;
}

/*
 * conn_buffer_protected_pkt buffers a protected packet |pkt| whose
 * length is |pktlen|.  This function is called when a protected
 * packet is received, but the local endpoint has not established
 * cryptographic context (e.g., Handshake packet is lost or delayed).
 *
 * This function also buffers 0-RTT Protected packet if it arrives
 * before Initial packet.
 *
 * The processing of 0-RTT Protected and Short packets take place in
 * their own stage, and we don't buffer them at the same time.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_buffer_protected_pkt(ngtcp2_conn *conn, const uint8_t *pkt,
                                     size_t pktlen, ngtcp2_tstamp ts) {
  return conn_buffer_pkt(conn, &conn->buffed_rx_ppkts, pkt, pktlen, ts);
}

/*
 * conn_buffer_handshake_pkt buffers Handshake packet which comes
 * before Initial packet, in other words, before handshake rx key is
 * generated.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_buffer_handshake_pkt(ngtcp2_conn *conn, const uint8_t *pkt,
                                     size_t pktlen, ngtcp2_tstamp ts) {
  return conn_buffer_pkt(conn, &conn->buffed_rx_hs_pkts, pkt, pktlen, ts);
}

/*
 * conn_ensure_decrypt_buffer ensures that conn->decrypt_buf has at
 * least |n| bytes space.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_ensure_decrypt_buffer(ngtcp2_conn *conn, size_t n) {
  uint8_t *nbuf;
  size_t len;

  if (conn->decrypt_buf.len >= n) {
    return 0;
  }

  len = conn->decrypt_buf.len == 0 ? 2048 : conn->decrypt_buf.len * 2;
  for (; len < n; len *= 2)
    ;
  nbuf = ngtcp2_mem_realloc(conn->mem, conn->decrypt_buf.base, len);
  if (nbuf == NULL) {
    return NGTCP2_ERR_NOMEM;
  }
  conn->decrypt_buf.base = nbuf;
  conn->decrypt_buf.len = len;

  return 0;
}

/*
 * conn_decrypt_pkt decrypts the data pointed by |payload| whose
 * length is |payloadlen|, and writes plaintext data to the buffer
 * pointed by |dest| whose capacity is |destlen|.  The buffer pointed
 * by |ad| is the Additional Data, and its length is |adlen|.
 * |pkt_num| is used to create a nonce.  |ckm| is the cryptographic
 * key, and iv to use.  |decrypt| is a callback function which
 * actually decrypts a packet.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 * NGTCP2_ERR_TLS_DECRYPT
 *     TLS backend failed to decrypt data.
 */
static ssize_t conn_decrypt_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                size_t destlen, const uint8_t *payload,
                                size_t payloadlen, const uint8_t *ad,
                                size_t adlen, uint64_t pkt_num,
                                ngtcp2_crypto_km *ckm, ngtcp2_decrypt decrypt) {
  /* TODO nonce is limited to 64 bytes. */
  uint8_t nonce[64];
  ssize_t nwrite;

  assert(sizeof(nonce) >= ckm->ivlen);

  ngtcp2_crypto_create_nonce(nonce, ckm->iv, ckm->ivlen, pkt_num);

  nwrite = decrypt(conn, dest, destlen, payload, payloadlen, ckm->key,
                   ckm->keylen, nonce, ckm->ivlen, ad, adlen, conn->user_data);

  if (nwrite < 0) {
    if (nwrite == NGTCP2_ERR_TLS_DECRYPT) {
      return nwrite;
    }
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return nwrite;
}

/*
 * conn_decrypt_pn decryptes packet number which starts at |pkt| +
 * |pkt_num_offset|.  The entire plaintext QUIC packer header will be
 * written to the buffer pointed by |dest| whose capacity is
 * |destlen|.
 */
static ssize_t conn_decrypt_pn(ngtcp2_conn *conn, ngtcp2_pkt_hd *hd,
                               uint8_t *dest, size_t destlen,
                               const uint8_t *pkt, size_t pktlen,
                               size_t pkt_num_offset, ngtcp2_crypto_km *ckm,
                               ngtcp2_encrypt_pn enc, size_t aead_overhead) {
  ssize_t nwrite;
  size_t sample_offset;
  uint8_t *p = dest;

  assert(enc);
  assert(ckm);
  assert(aead_overhead >= NGTCP2_PN_SAMPLELEN);

  if (pkt_num_offset + 1 + aead_overhead > pktlen) {
    return NGTCP2_ERR_PROTO;
  }

  if (destlen < pkt_num_offset + 4) {
    return NGTCP2_ERR_INTERNAL;
  }

  p = ngtcp2_cpymem(p, pkt, pkt_num_offset);

  sample_offset = ngtcp2_min(pkt_num_offset + 4, pktlen - aead_overhead);

  nwrite = enc(conn, p, 4, pkt + pkt_num_offset, 4, ckm->pn, ckm->pnlen,
               pkt + sample_offset, NGTCP2_PN_SAMPLELEN, conn->user_data);
  if (nwrite != 4) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  hd->pkt_num = ngtcp2_get_pkt_num(&hd->pkt_numlen, p);

  p += hd->pkt_numlen;

  return p - dest;
}

static void conn_extend_max_stream_offset(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                          size_t datalen) {
  if (strm->unsent_max_rx_offset <= NGTCP2_MAX_VARINT - datalen) {
    strm->unsent_max_rx_offset += datalen;
  }

  if (!(strm->flags &
        (NGTCP2_STRM_FLAG_SHUT_RD | NGTCP2_STRM_FLAG_STOP_SENDING)) &&
      !strm->fc_pprev && conn_should_send_max_stream_data(conn, strm)) {
    strm->fc_pprev = &conn->fc_strms;
    if (conn->fc_strms) {
      strm->fc_next = conn->fc_strms;
      conn->fc_strms->fc_pprev = &strm->fc_next;
    }
    conn->fc_strms = strm;
  }
}

/*
 * conn_emit_pending_crypto_data delivers pending stream data to the
 * application due to packet reordering.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed
 * NGTCP2_ERR_CRYPTO
 *     TLS backend reported error
 */
static int conn_emit_pending_crypto_data(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                         uint64_t rx_offset) {
  size_t datalen;
  const uint8_t *data;
  int rv;
  uint64_t offset;

  for (;;) {
    datalen = ngtcp2_rob_data_at(&strm->rob, &data, rx_offset);
    if (datalen == 0) {
      assert(rx_offset == ngtcp2_strm_rx_offset(strm));
      return 0;
    }

    offset = rx_offset;
    rx_offset += datalen;

    rv = conn_call_recv_crypto_data(conn, offset, data, datalen);
    if (rv != 0) {
      return rv;
    }

    rv = ngtcp2_rob_pop(&strm->rob, rx_offset - datalen, datalen);
    if (rv != 0) {
      return rv;
    }
  }
}

/* conn_recv_connection_close is called when CONNECTION_CLOSE or
   APPLICATION_CLOSE frame is received. */
static void conn_recv_connection_close(ngtcp2_conn *conn) {
  conn->state = NGTCP2_CS_DRAINING;
}

static void conn_recv_path_challenge(ngtcp2_conn *conn,
                                     ngtcp2_path_challenge *fr,
                                     ngtcp2_tstamp ts) {
  ngtcp2_path_challenge_entry *ent;

  ent = ngtcp2_ringbuf_push_front(&conn->rx_path_challenge);
  ent->ts = ts;
  assert(sizeof(ent->data) == sizeof(fr->data));
  ngtcp2_cpymem(ent->data, fr->data, sizeof(ent->data));
}

static void conn_recv_path_response(ngtcp2_conn *conn,
                                    ngtcp2_path_response *fr) {
  size_t len = ngtcp2_ringbuf_len(&conn->tx_path_challenge);
  size_t i;
  ngtcp2_path_challenge_entry *ent;

  for (i = 0; i < len; ++i) {
    ent = ngtcp2_ringbuf_get(&conn->tx_path_challenge, i);
    if (memcmp(ent->data, fr->data, sizeof(ent->data)) == 0) {
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                      "source address validated");
      conn->flags |= NGTCP2_CONN_FLAG_SADDR_VERIFIED;
      ngtcp2_ringbuf_resize(&conn->tx_path_challenge, 0);
      return;
    }
  }
}

/* conn_update_rx_bw updates rx bandwidth. */
static void conn_update_rx_bw(ngtcp2_conn *conn, size_t datalen,
                              ngtcp2_tstamp ts) {
  /* Reset bandwidth measurement after 1 second idle time. */
  if (ts - conn->first_rx_bw_ts > 1000000000) {
    conn->first_rx_bw_ts = ts;
    conn->rx_bw_datalen = datalen;
    conn->rx_bw = 0.;
    return;
  }

  conn->rx_bw_datalen += datalen;

  if (ts - conn->first_rx_bw_ts >= 25000000) {
    conn->rx_bw =
        (double)conn->rx_bw_datalen / (double)(ts - conn->first_rx_bw_ts);

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON, "rx_bw=%.02fBs",
                    conn->rx_bw * 1000000000);
  }
}

static ssize_t conn_recv_pkt(ngtcp2_conn *conn, const uint8_t *pkt,
                             size_t pktlen, ngtcp2_tstamp ts);

/*
 * pkt_num_bits returns the number of bits available when packet
 * number is encoded in |pkt_numlen| bytes.
 */
static size_t pkt_num_bits(size_t pkt_numlen) {
  switch (pkt_numlen) {
  case 1:
    return 7;
  case 2:
    return 14;
  case 4:
    return 30;
  default:
    assert(0);
  }
}

static int conn_recv_crypto(ngtcp2_conn *conn, uint64_t rx_offset_base,
                            uint64_t max_rx_offset, const ngtcp2_crypto *fr);

/*
 * conn_recv_handshake_pkt processes received packet |pkt| whose
 * length if |pktlen| during handshake period.  The buffer pointed by
 * |pkt| might contain multiple packets.  This function only processes
 * one packet.
 *
 * This function returns the number of bytes it reads if it succeeds,
 * or one of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_INVALID_ARGUMENT
 *     Packet is too short; or it is not a long header.
 * NGTCP2_ERR_UNKNOWN_PKT_TYPE
 *     Packet type is unknown
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_PROTO
 *     Generic QUIC protocol error.
 * NGTCP2_ERR_ACK_FRAME
 *     ACK frame is malformed.
 * NGTCP2_ERR_TLS_HANDSHAKE
 *     TLS handshake failed, and/or TLS alert was generated.
 * NGTCP2_ERR_FRAME_ENCODING
 *     Frame is badly formatted.
 * NGTCP2_ERR_RECV_VERSION_NEGOTIATION
 *     Version Negotiation packet is received.
 * NGTCP2_ERR_TLS_DECRYPT
 *     Could not decrypt a packet.
 *
 * In addition to the above error codes, error codes returned from
 * conn_recv_pkt are also returned.
 */
static ssize_t conn_recv_handshake_pkt(ngtcp2_conn *conn, const uint8_t *pkt,
                                       size_t pktlen, ngtcp2_tstamp ts) {
  ssize_t nread;
  ngtcp2_pkt_hd hd;
  ngtcp2_max_frame mfr;
  ngtcp2_frame *fr = &mfr.fr;
  int rv;
  int require_ack = 0;
  size_t hdpktlen;
  const uint8_t *payload;
  size_t payloadlen;
  ssize_t nwrite;
  uint8_t plain_hdpkt[1500];
  ngtcp2_crypto_km *ckm;
  ngtcp2_encrypt_pn encrypt_pn;
  ngtcp2_decrypt decrypt;
  size_t aead_overhead;
  ngtcp2_pktns *pktns;
  ngtcp2_strm *crypto = &conn->crypto;
  uint64_t max_crypto_rx_offset;

  if (pktlen == 0) {
    return 0;
  }

  if (!(pkt[0] & NGTCP2_HEADER_FORM_BIT)) {
    if (conn->state == NGTCP2_CS_SERVER_INITIAL) {
      /* Ignore Short packet unless server's first Handshake packet
         has been transmitted. */
      return (ssize_t)pktlen;
    }

    if (conn->pktns.rx_ckm) {
      nread = conn_recv_pkt(conn, pkt, pktlen, ts);
      if (nread < 0) {
        return nread;
      }

      return (ssize_t)pktlen;
    }

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                    "buffering Short packet len=%zu", pktlen);

    rv = conn_buffer_protected_pkt(conn, pkt, pktlen, ts);
    if (rv != 0) {
      return rv;
    }
    return (ssize_t)pktlen;
  }

  nread = ngtcp2_pkt_decode_hd_long(&hd, pkt, pktlen);
  if (nread < 0) {
    return (int)nread;
  }

  switch (hd.type) {
  case NGTCP2_PKT_VERSION_NEGOTIATION:
    hdpktlen = (size_t)nread;

    ngtcp2_log_rx_pkt_hd(&conn->log, &hd);

    if (conn->server) {
      return NGTCP2_ERR_PROTO;
    }

    /* Receiving Version Negotiation packet after getting Handshake
       packet from server is invalid. */
    if (conn->flags & NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED) {
      return (ssize_t)pktlen;
    }
    if (!ngtcp2_cid_eq(&conn->scid, &hd.dcid) ||
        !ngtcp2_cid_eq(&conn->dcid, &hd.scid)) {
      /* Just discard invalid Version Negotiation packet */
      return (ssize_t)pktlen;
    }
    rv = conn_on_version_negotiation(conn, &hd, pkt + hdpktlen,
                                     pktlen - hdpktlen);
    if (rv != 0) {
      return rv;
    }
    return NGTCP2_ERR_RECV_VERSION_NEGOTIATION;
  case NGTCP2_PKT_RETRY:
    hdpktlen = (size_t)nread;

    ngtcp2_log_rx_pkt_hd(&conn->log, &hd);

    if (conn->server) {
      return NGTCP2_ERR_PROTO;
    }

    /* Receiving Retry packet after getting Initial packet from server
       is invalid. */
    if (conn->flags & NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED) {
      return (ssize_t)pktlen;
    }

    rv = conn_on_retry(conn, &hd, pkt + hdpktlen, pktlen - hdpktlen);
    if (rv != 0) {
      if (rv == NGTCP2_ERR_TOO_MANY_RETRIES) {
        return rv;
      }
      if (ngtcp2_err_is_fatal(rv)) {
        return rv;
      }
    }
    return (ssize_t)pktlen;
  }

  if (pktlen < (size_t)nread + hd.len) {
    return (ssize_t)pktlen;
  }

  pktlen = (size_t)nread + hd.len;

  if (conn->version != hd.version) {
    return (ssize_t)pktlen;
  }

  /* Quoted from spec: if subsequent packets of those types include a
     different Source Connection ID, they MUST be discarded. */
  if ((conn->flags & NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED) &&
      !ngtcp2_cid_eq(&conn->dcid, &hd.scid)) {
    ngtcp2_log_rx_pkt_hd(&conn->log, &hd);
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "packet was ignored because of mismatched SCID");
    return (ssize_t)pktlen;
  }

  switch (hd.type) {
  case NGTCP2_PKT_0RTT_PROTECTED:
    if (!conn->server) {
      /* TODO protocol violation? */
      return (ssize_t)pktlen;
    }
    if (conn->flags & NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED) {
      if (conn->early_ckm) {
        ssize_t nread2;
        /* TODO Avoid to parse header twice. */
        nread2 = conn_recv_pkt(conn, pkt, pktlen, ts);
        if (nread2 < 0) {
          return nread2;
        }
      }

      /* Discard 0-RTT packet if we don't have a key to decrypt it. */
      return (ssize_t)pktlen;
    }

    /* Buffer re-ordered 0-RTT Protected packet. */
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                    "buffering 0-RTT Protected packet len=%zu", pktlen);

    rv = conn_buffer_protected_pkt(conn, pkt, pktlen, ts);
    if (rv != 0) {
      return rv;
    }
    return (ssize_t)pktlen;
  case NGTCP2_PKT_INITIAL:
    if (conn->server) {
      if ((conn->flags & NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED) == 0) {
        conn->flags |= NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED;
        conn->rcid = hd.dcid;

        rv = conn_call_recv_client_initial(conn);
        if (rv != 0) {
          return rv;
        }
      } else if (!ngtcp2_cid_eq(&conn->scid, &hd.dcid) &&
                 !ngtcp2_cid_eq(&conn->rcid, &hd.dcid)) {
        ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                        "packet was ignored because of mismatched DCID");
        return (ssize_t)pktlen;
      }
    } else {
      if (!ngtcp2_cid_eq(&conn->scid, &hd.dcid)) {
        ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                        "packet was ignored because of mismatched DCID");
        return (ssize_t)pktlen;
      }
      if (hd.tokenlen != 0) {
        return (ssize_t)pktlen;
      }
    }

    pktns = &conn->in_pktns;
    encrypt_pn = conn->callbacks.in_encrypt_pn;
    decrypt = conn->callbacks.in_decrypt;
    aead_overhead = NGTCP2_INITIAL_AEAD_OVERHEAD;
    if (conn->server && conn->early_ckm) {
      max_crypto_rx_offset = conn->early_crypto_rx_offset_base;
    } else {
      max_crypto_rx_offset = conn->hs_pktns.crypto_rx_offset_base;
    }

    break;
  case NGTCP2_PKT_HANDSHAKE:
    if (!ngtcp2_cid_eq(&conn->scid, &hd.dcid)) {
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "packet was ignored because of mismatched DCID");
      return (ssize_t)pktlen;
    }

    if (!conn->hs_pktns.rx_ckm) {
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                      "buffering Handshake packet len=%zu", pktlen);

      rv = conn_buffer_handshake_pkt(conn, pkt, pktlen, ts);
      if (rv != 0) {
        return rv;
      }
      return (ssize_t)pktlen;
    }

    pktns = &conn->hs_pktns;
    encrypt_pn = conn->callbacks.encrypt_pn;
    decrypt = conn->callbacks.decrypt;
    aead_overhead = conn->aead_overhead;
    max_crypto_rx_offset = conn->pktns.crypto_rx_offset_base;

    break;
  default:
    /* unknown packet type */
    return (ssize_t)pktlen;
  }

  ckm = pktns->rx_ckm;

  assert(ckm);
  assert(encrypt_pn);
  assert(decrypt);

  nwrite =
      conn_decrypt_pn(conn, &hd, plain_hdpkt, sizeof(plain_hdpkt), pkt, pktlen,
                      (size_t)nread, ckm, encrypt_pn, aead_overhead);
  if (nwrite < 0) {
    return (ssize_t)nwrite;
  }

  hdpktlen = (size_t)nwrite;
  payload = pkt + hdpktlen;
  payloadlen = hd.len - hd.pkt_numlen;

  hd.pkt_num = ngtcp2_pkt_adjust_pkt_num(pktns->max_rx_pkt_num, hd.pkt_num,
                                         pkt_num_bits(hd.pkt_numlen));

  ngtcp2_log_rx_pkt_hd(&conn->log, &hd);

  rv = conn_ensure_decrypt_buffer(conn, payloadlen);
  if (rv != 0) {
    return rv;
  }

  nwrite = conn_decrypt_pkt(conn, conn->decrypt_buf.base, payloadlen, payload,
                            payloadlen, plain_hdpkt, hdpktlen, hd.pkt_num, ckm,
                            decrypt);
  if (nwrite < 0) {
    return (int)nwrite;
  }

  payload = conn->decrypt_buf.base;
  payloadlen = (size_t)nwrite;

  if (!conn->server && hd.type == NGTCP2_PKT_INITIAL &&
      !(conn->flags & NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED)) {
    conn->flags |= NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED;
    conn->dcid = hd.scid;
  }

  for (; payloadlen;) {
    nread = ngtcp2_pkt_decode_frame(fr, payload, payloadlen);
    if (nread < 0) {
      return (int)nread;
    }

    payload += nread;
    payloadlen -= (size_t)nread;

    if (fr->type == NGTCP2_FRAME_ACK) {
      assign_recved_ack_delay_unscaled(&fr->ack,
                                       NGTCP2_DEFAULT_ACK_DELAY_EXPONENT);
    }

    ngtcp2_log_rx_fr(&conn->log, &hd, fr);

    switch (fr->type) {
    case NGTCP2_FRAME_ACK:
      rv = conn_recv_ack(conn, pktns, &hd, &fr->ack, ts);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_PADDING:
      break;
    case NGTCP2_FRAME_CRYPTO:
      rv = conn_recv_crypto(conn, pktns->crypto_rx_offset_base,
                            max_crypto_rx_offset, &fr->crypto);
      if (rv != 0) {
        return rv;
      }
      require_ack = 1;
      break;
    case NGTCP2_FRAME_CONNECTION_CLOSE:
      conn_recv_connection_close(conn);
      break;
    case NGTCP2_FRAME_PING:
      require_ack = 1;
      break;
    default:
      return NGTCP2_ERR_PROTO;
    }
  }

  if (conn->server && hd.type == NGTCP2_PKT_INITIAL &&
      ngtcp2_rob_first_gap_offset(&crypto->rob) == 0) {
    return NGTCP2_ERR_PROTO;
  }

  pktns->max_rx_pkt_num = ngtcp2_max(pktns->max_rx_pkt_num, hd.pkt_num);

  rv = ngtcp2_conn_sched_ack(conn, &pktns->acktr, hd.pkt_num, require_ack, ts);
  if (rv != 0) {
    return rv;
  }

  return conn->state == NGTCP2_CS_DRAINING ? NGTCP2_ERR_DRAINING
                                           : (ssize_t)pktlen;
}

/*
 * conn_recv_handshake_cpkt processes compound packet during
 * handshake.  The buffer pointed by |pkt| might contain multiple
 * packets.  The Short packet must be the last one because it does not
 * have payload length field.
 */
static int conn_recv_handshake_cpkt(ngtcp2_conn *conn, const uint8_t *pkt,
                                    size_t pktlen, ngtcp2_tstamp ts) {
  ssize_t nread;

  while (pktlen) {
    nread = conn_recv_handshake_pkt(conn, pkt, pktlen, ts);
    if (nread < 0) {
      return (int)nread;
    }

    assert(pktlen >= (size_t)nread);
    pkt += nread;
    pktlen -= (size_t)nread;

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "read packet %zd left %zu", nread, pktlen);
  }

  return 0;
}

int ngtcp2_conn_init_stream(ngtcp2_conn *conn, ngtcp2_strm *strm,
                            uint64_t stream_id, void *stream_user_data) {
  int rv;
  uint64_t max_rx_offset;
  uint64_t max_tx_offset;
  int local_stream = conn_local_stream(conn, stream_id);

  if (bidi_stream(stream_id)) {
    if (local_stream) {
      max_rx_offset = conn->local_settings.max_stream_data_bidi_local;
      max_tx_offset = conn->remote_settings.max_stream_data_bidi_remote;
    } else {
      max_rx_offset = conn->local_settings.max_stream_data_bidi_remote;
      max_tx_offset = conn->remote_settings.max_stream_data_bidi_local;
    }
  } else if (local_stream) {
    max_rx_offset = 0;
    max_tx_offset = conn->remote_settings.max_stream_data_uni;
  } else {
    max_rx_offset = conn->local_settings.max_stream_data_uni;
    max_tx_offset = 0;
  }

  rv = ngtcp2_strm_init(strm, stream_id, NGTCP2_STRM_FLAG_NONE, max_rx_offset,
                        max_tx_offset, stream_user_data, conn->mem);
  if (rv != 0) {
    ngtcp2_mem_free(conn->mem, strm);
    return rv;
  }

  rv = ngtcp2_map_insert(&conn->strms, &strm->me);
  if (rv != 0) {
    assert(rv != NGTCP2_ERR_INVALID_ARGUMENT);

    ngtcp2_strm_free(strm);
    ngtcp2_mem_free(conn->mem, strm);
    return rv;
  }

  return 0;
}

/*
 * conn_emit_pending_stream_data passes buffered ordered stream data
 * to the application.  |rx_offset| is the first offset to deliver to
 * the application.  This function assumes that the data up to
 * |rx_offset| has been delivered already.  This function only passes
 * the ordered data without any gap.  If there is a gap, it stops
 * providing the data to the application, and returns.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 */
static int conn_emit_pending_stream_data(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                         uint64_t rx_offset) {
  size_t datalen;
  const uint8_t *data;
  int rv;
  uint64_t offset;

  for (;;) {
    datalen = ngtcp2_rob_data_at(&strm->rob, &data, rx_offset);
    if (datalen == 0) {
      assert(rx_offset == ngtcp2_strm_rx_offset(strm));
      return 0;
    }

    offset = rx_offset;
    rx_offset += datalen;

    rv = conn_call_recv_stream_data(conn, strm,
                                    (strm->flags & NGTCP2_STRM_FLAG_SHUT_RD) &&
                                        rx_offset == strm->last_rx_offset,
                                    offset, data, datalen);
    if (rv != 0) {
      return rv;
    }

    rv = ngtcp2_rob_pop(&strm->rob, rx_offset - datalen, datalen);
    if (rv != 0) {
      return rv;
    }
  }
}

/*
 * conn_recv_crypto is called when CRYPTO frame |fr| is received.
 * |rx_offset_base| is the offset in the entire TLS handshake stream.
 * fr->offset specifies the offset in each encryption level.
 * |max_rx_offset| is, if it is nonzero, the maximum offset in the
 * entire TLS handshake stream that |fr| can carry.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * TBD
 */
static int conn_recv_crypto(ngtcp2_conn *conn, uint64_t rx_offset_base,
                            uint64_t max_rx_offset, const ngtcp2_crypto *fr) {
  ngtcp2_strm *crypto = &conn->crypto;
  uint64_t fr_end_offset;
  uint64_t rx_offset;
  int rv;

  if (fr->datacnt == 0) {
    return 0;
  }

  fr_end_offset = rx_offset_base + fr->offset + fr->data[0].len;

  if (max_rx_offset && max_rx_offset < fr_end_offset) {
    return NGTCP2_ERR_PROTO;
  }

  if (crypto->max_rx_offset && crypto->max_rx_offset < fr_end_offset) {
    return NGTCP2_ERR_INTERNAL;
  }

  rx_offset = ngtcp2_strm_rx_offset(crypto);

  if (fr_end_offset <= rx_offset) {
    return 0;
  }

  crypto->last_rx_offset = ngtcp2_max(crypto->last_rx_offset, fr_end_offset);

  /* TODO Before dispatching incoming data to TLS stack, make sure
     that previous data in previous encryption level has been
     completely sent to TLS stack.  Usually, if data is left, it is an
     error because key is generated after consuming all data in the
     previous encryption level. */
  if (rx_offset_base + fr->offset <= rx_offset) {
    size_t ncut = rx_offset - fr->offset - rx_offset_base;
    const uint8_t *data = fr->data[0].base + ncut;
    size_t datalen = fr->data[0].len - ncut;
    uint64_t offset = rx_offset;

    rx_offset += datalen;
    rv = ngtcp2_rob_remove_prefix(&crypto->rob, rx_offset);
    if (rv != 0) {
      return rv;
    }

    rv = conn_call_recv_crypto_data(conn, offset, data, datalen);
    if (rv != 0) {
      return rv;
    }

    rv = conn_emit_pending_crypto_data(conn, crypto, rx_offset);
    if (rv != 0) {
      return rv;
    }
  } else {
    rv = ngtcp2_strm_recv_reordering(crypto, fr->data[0].base, fr->data[0].len,
                                     rx_offset_base + fr->offset);
    if (rv != 0) {
      return rv;
    }
  }

  return 0;
}

/*
 * conn_max_data_violated returns nonzero if receiving |datalen|
 * violates connection flow control on local endpoint.
 */
static int conn_max_data_violated(ngtcp2_conn *conn, size_t datalen) {
  return conn->max_rx_offset - conn->rx_offset < datalen;
}

static int conn_recv_stream(ngtcp2_conn *conn, const ngtcp2_stream *fr) {
  int rv;
  ngtcp2_strm *strm;
  ngtcp2_idtr *idtr;
  uint64_t rx_offset, fr_end_offset;
  int local_stream;
  int bidi;

  local_stream = conn_local_stream(conn, fr->stream_id);
  bidi = bidi_stream(fr->stream_id);

  if (bidi) {
    if (local_stream) {
      if (conn->next_local_stream_id_bidi <= fr->stream_id) {
        return NGTCP2_ERR_STREAM_STATE;
      }
    } else if (conn->max_remote_stream_id_bidi < fr->stream_id) {
      return NGTCP2_ERR_STREAM_ID;
    }

    idtr = &conn->remote_bidi_idtr;
  } else {
    if (local_stream) {
      return NGTCP2_ERR_PROTO;
    }
    if (conn->max_remote_stream_id_uni < fr->stream_id) {
      return NGTCP2_ERR_STREAM_ID;
    }

    idtr = &conn->remote_uni_idtr;
  }

  if (NGTCP2_MAX_VARINT - fr->datalen < fr->offset) {
    return NGTCP2_ERR_PROTO;
  }

  strm = ngtcp2_conn_find_stream(conn, fr->stream_id);
  if (strm == NULL) {
    if (local_stream) {
      /* TODO The stream has been closed.  This should be responded
         with RST_STREAM, or simply ignored. */
      return 0;
    }

    rv = ngtcp2_idtr_open(idtr, fr->stream_id);
    if (rv == NGTCP2_ERR_STREAM_IN_USE) {
      /* TODO The stream has been closed.  This should be responded
         with RST_STREAM, or simply ignored. */
      return 0;
    }
    assert(0 == rv);

    strm = ngtcp2_mem_malloc(conn->mem, sizeof(ngtcp2_strm));
    if (strm == NULL) {
      return NGTCP2_ERR_NOMEM;
    }
    /* TODO Perhaps, call new_stream callback? */
    rv = ngtcp2_conn_init_stream(conn, strm, fr->stream_id, NULL);
    if (rv != 0) {
      return rv;
    }
    if (!bidi) {
      ngtcp2_strm_shutdown(strm, NGTCP2_STRM_FLAG_SHUT_WR);
    }
  }

  fr_end_offset = fr->offset + fr->datalen;

  if (strm->max_rx_offset < fr_end_offset) {
    return NGTCP2_ERR_FLOW_CONTROL;
  }

  if (strm->last_rx_offset < fr_end_offset) {
    size_t datalen = fr_end_offset - strm->last_rx_offset;

    if (conn_max_data_violated(conn, datalen)) {
      return NGTCP2_ERR_FLOW_CONTROL;
    }

    conn->rx_offset += datalen;
  }

  rx_offset = ngtcp2_strm_rx_offset(strm);

  if (fr->fin) {
    if (strm->flags & NGTCP2_STRM_FLAG_SHUT_RD) {
      if (strm->last_rx_offset != fr_end_offset) {
        return NGTCP2_ERR_FINAL_OFFSET;
      }
    } else if (strm->last_rx_offset > fr_end_offset) {
      return NGTCP2_ERR_FINAL_OFFSET;
    } else {
      strm->last_rx_offset = fr_end_offset;

      ngtcp2_strm_shutdown(strm, NGTCP2_STRM_FLAG_SHUT_RD);

      if (strm->flags & NGTCP2_STRM_FLAG_STOP_SENDING) {
        return ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm,
                                                     strm->app_error_code);
      }

      /* Since strm is now in closed (remote), we don't have to send
         MAX_STREAM_DATA anymore. */
      if (strm->fc_pprev) {
        *strm->fc_pprev = strm->fc_next;
        if (strm->fc_next) {
          strm->fc_next->fc_pprev = strm->fc_pprev;
        }
        strm->fc_pprev = NULL;
        strm->fc_next = NULL;
      }

      if (fr_end_offset == rx_offset) {
        rv = conn_call_recv_stream_data(conn, strm, 1, rx_offset, NULL, 0);
        if (rv != 0) {
          return rv;
        }
        return ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm,
                                                     NGTCP2_NO_ERROR);
      }
    }
  } else {
    if ((strm->flags & NGTCP2_STRM_FLAG_SHUT_RD) &&
        strm->last_rx_offset < fr_end_offset) {
      return NGTCP2_ERR_FINAL_OFFSET;
    }

    strm->last_rx_offset = ngtcp2_max(strm->last_rx_offset, fr_end_offset);

    if (fr_end_offset <= rx_offset) {
      return 0;
    }

    if (strm->flags & NGTCP2_STRM_FLAG_STOP_SENDING) {
      return 0;
    }
  }

  if (fr->offset <= rx_offset) {
    size_t ncut = rx_offset - fr->offset;
    const uint8_t *data = fr->data + ncut;
    size_t datalen = fr->datalen - ncut;
    uint64_t offset = rx_offset;

    rx_offset += datalen;
    rv = ngtcp2_rob_remove_prefix(&strm->rob, rx_offset);
    if (rv != 0) {
      return rv;
    }

    rv = conn_call_recv_stream_data(conn, strm,
                                    (strm->flags & NGTCP2_STRM_FLAG_SHUT_RD) &&
                                        rx_offset == strm->last_rx_offset,
                                    offset, data, datalen);
    if (rv != 0) {
      return rv;
    }

    rv = conn_emit_pending_stream_data(conn, strm, rx_offset);
    if (rv != 0) {
      return rv;
    }
  } else {
    rv = ngtcp2_strm_recv_reordering(strm, fr->data, fr->datalen, fr->offset);
    if (rv != 0) {
      return rv;
    }
  }
  return ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm, NGTCP2_NO_ERROR);
}

/*
 * conn_rst_stream adds RST_STREAM frame to the transmission queue.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_rst_stream(ngtcp2_conn *conn, ngtcp2_strm *strm,
                           uint16_t app_error_code) {
  int rv;
  ngtcp2_frame_chain *frc;

  rv = ngtcp2_frame_chain_new(&frc, conn->mem);
  if (rv != 0) {
    return rv;
  }

  frc->fr.type = NGTCP2_FRAME_RST_STREAM;
  frc->fr.rst_stream.stream_id = strm->stream_id;
  frc->fr.rst_stream.app_error_code = app_error_code;
  frc->fr.rst_stream.final_offset = strm->tx_offset;

  /* TODO This prepends RST_STREAM to conn->frq. */
  frc->next = conn->frq;
  conn->frq = frc;

  return 0;
}

static int conn_stop_sending(ngtcp2_conn *conn, ngtcp2_strm *strm,
                             uint16_t app_error_code) {
  int rv;
  ngtcp2_frame_chain *frc;

  rv = ngtcp2_frame_chain_new(&frc, conn->mem);
  if (rv != 0) {
    return rv;
  }

  frc->fr.type = NGTCP2_FRAME_STOP_SENDING;
  frc->fr.stop_sending.stream_id = strm->stream_id;
  frc->fr.stop_sending.app_error_code = app_error_code;

  /* TODO This prepends STOP_SENDING to conn->frq. */
  frc->next = conn->frq;
  conn->frq = frc;

  /* Since STREAM is being reset, we don't have to send
     MAX_STREAM_DATA anymore */
  if (strm->fc_pprev) {
    *strm->fc_pprev = strm->fc_next;
    if (strm->fc_next) {
      strm->fc_next->fc_pprev = strm->fc_pprev;
    }
    strm->fc_pprev = NULL;
    strm->fc_next = NULL;
  }

  return 0;
}

static int conn_recv_rst_stream(ngtcp2_conn *conn,
                                const ngtcp2_rst_stream *fr) {
  ngtcp2_strm *strm;
  int local_stream = conn_local_stream(conn, fr->stream_id);
  int bidi = bidi_stream(fr->stream_id);
  uint64_t datalen;
  ngtcp2_idtr *idtr;

  if (bidi) {
    if (local_stream) {
      if (conn->next_local_stream_id_bidi <= fr->stream_id) {
        return NGTCP2_ERR_STREAM_STATE;
      }
    } else if (fr->stream_id > conn->max_remote_stream_id_bidi) {
      return NGTCP2_ERR_STREAM_ID;
    }

    idtr = &conn->remote_bidi_idtr;
  } else {
    if (local_stream) {
      return NGTCP2_ERR_PROTO;
    }
    if (fr->stream_id > conn->max_remote_stream_id_uni) {
      return NGTCP2_ERR_STREAM_ID;
    }

    idtr = &conn->remote_uni_idtr;
  }

  strm = ngtcp2_conn_find_stream(conn, fr->stream_id);
  if (strm == NULL) {
    if (!local_stream && !ngtcp2_idtr_is_open(idtr, fr->stream_id)) {
      /* Stream is reset before we create ngtcp2_strm object. */
      if (conn_initial_stream_rx_offset(conn, fr->stream_id) <
              fr->final_offset ||
          conn_max_data_violated(conn, fr->final_offset)) {
        return NGTCP2_ERR_FLOW_CONTROL;
      }
      ngtcp2_idtr_open(idtr, fr->stream_id);
      conn->rx_offset += fr->final_offset;
    }
    return 0;
  }

  if ((strm->flags & NGTCP2_STRM_FLAG_SHUT_RD)) {
    if (strm->last_rx_offset != fr->final_offset) {
      return NGTCP2_ERR_FINAL_OFFSET;
    }
  } else if (strm->last_rx_offset > fr->final_offset) {
    return NGTCP2_ERR_FINAL_OFFSET;
  }

  datalen = fr->final_offset - strm->last_rx_offset;

  if (strm->max_rx_offset < fr->final_offset ||
      conn_max_data_violated(conn, datalen)) {
    return NGTCP2_ERR_FLOW_CONTROL;
  }

  conn->rx_offset += datalen;

  strm->flags |= NGTCP2_STRM_FLAG_SHUT_RD | NGTCP2_STRM_FLAG_RECV_RST;

  return ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm, fr->app_error_code);
}

static int conn_recv_stop_sending(ngtcp2_conn *conn,
                                  const ngtcp2_stop_sending *fr) {
  int rv;
  ngtcp2_strm *strm;
  ngtcp2_idtr *idtr;
  int local_stream = conn_local_stream(conn, fr->stream_id);
  int bidi = bidi_stream(fr->stream_id);

  if (bidi) {
    if (local_stream) {
      if (conn->next_local_stream_id_bidi <= fr->stream_id) {
        return NGTCP2_ERR_STREAM_STATE;
      }
    } else if (fr->stream_id > conn->max_remote_stream_id_bidi) {
      return NGTCP2_ERR_STREAM_ID;
    }

    idtr = &conn->remote_bidi_idtr;
  } else {
    if (!local_stream) {
      return NGTCP2_ERR_PROTO;
    }
    if (conn->next_local_stream_id_uni <= fr->stream_id) {
      return NGTCP2_ERR_STREAM_STATE;
    }

    idtr = &conn->remote_uni_idtr;
  }

  strm = ngtcp2_conn_find_stream(conn, fr->stream_id);
  if (strm == NULL) {
    if (!local_stream && !ngtcp2_idtr_is_open(idtr, fr->stream_id)) {
      /* Frame is received reset before we create ngtcp2_strm
         object. */
      ngtcp2_idtr_open(idtr, fr->stream_id);
    }
    return 0;
  }

  rv = conn_rst_stream(conn, strm, NGTCP2_STOPPING);
  if (rv != 0) {
    return rv;
  }

  strm->flags |= NGTCP2_STRM_FLAG_SHUT_WR | NGTCP2_STRM_FLAG_SENT_RST;

  return ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm, fr->app_error_code);
}

/*
 * conn_on_stateless_reset decodes Stateless Reset from the buffer
 * pointed by |payload| whose length is |payloadlen|.  |payload|
 * should start after first byte of packet.
 *
 * If Stateless Reset is decoded, and the Stateless Reset Token is
 * validated, the connection is closed.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_INVALID_ARGUMENT
 *     Could not decode Stateless Reset; or Stateless Reset Token does
 *     not match.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 */
static int conn_on_stateless_reset(ngtcp2_conn *conn, const ngtcp2_pkt_hd *hd,
                                   const uint8_t *payload, size_t payloadlen) {
  int rv;
  ngtcp2_pkt_stateless_reset sr;
  const uint8_t *token;
  size_t i;

  assert(!conn->server);

  rv = ngtcp2_pkt_decode_stateless_reset(&sr, payload, payloadlen);
  if (rv != 0) {
    return rv;
  }

  if (!conn->remote_settings.stateless_reset_token_present) {
    return NGTCP2_ERR_PROTO;
  }

  token = conn->remote_settings.stateless_reset_token;

  for (i = 0; i < NGTCP2_STATELESS_RESET_TOKENLEN; ++i) {
    rv |= token[i] ^ sr.stateless_reset_token[i];
  }

  if (rv != 0) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  conn->state = NGTCP2_CS_DRAINING;

  ngtcp2_log_rx_sr(&conn->log, hd, &sr);

  if (!conn->callbacks.recv_stateless_reset) {
    return 0;
  }

  rv = conn->callbacks.recv_stateless_reset(conn, hd, &sr, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

/*
 * conn_recv_delayed_handshake_pkt processes the received handshake
 * packet which is received after handshake completed.  This function
 * does the minimal job, and its purpose is send acknowledgement of
 * this packet to the peer.  We assume that hd->type is one of
 * Initial, or Handshake.  |ad| and |adlen| is an additional data and
 * its length to decrypt a packet.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_PROTO
 *     Packet type is unexpected; or same packet number has already
 *     been added.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 * NGTCP2_ERR_FRAME_ENCODING
 *     Frame is badly formatted; or frame type is unknown.
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 * NGTCP2_ERR_TLS_DECRYPT
 *     Could not decrypt a packet.
 */
static int conn_recv_delayed_handshake_pkt(ngtcp2_conn *conn,
                                           const ngtcp2_pkt_hd *hd,
                                           const uint8_t *payload,
                                           size_t payloadlen, const uint8_t *ad,
                                           size_t adlen, ngtcp2_tstamp ts) {
  ssize_t nread;
  ngtcp2_max_frame mfr;
  ngtcp2_frame *fr = &mfr.fr;
  int rv;
  int require_ack = 0;
  ssize_t nwrite;
  ngtcp2_pktns *pktns;
  ngtcp2_decrypt decrypt;

  switch (hd->type) {
  case NGTCP2_PKT_INITIAL:
    pktns = &conn->in_pktns;
    decrypt = conn->callbacks.in_decrypt;
    break;
  case NGTCP2_PKT_HANDSHAKE:
    pktns = &conn->hs_pktns;
    decrypt = conn->callbacks.decrypt;
    break;
  default:
    assert(0);
  }

  rv = conn_ensure_decrypt_buffer(conn, payloadlen);
  if (rv != 0) {
    return rv;
  }

  nwrite = conn_decrypt_pkt(conn, conn->decrypt_buf.base, payloadlen, payload,
                            payloadlen, ad, adlen, hd->pkt_num, pktns->rx_ckm,
                            decrypt);
  if (nwrite < 0) {
    return (int)nwrite;
  }

  payload = conn->decrypt_buf.base;
  payloadlen = (size_t)nwrite;

  for (; payloadlen;) {
    nread = ngtcp2_pkt_decode_frame(fr, payload, payloadlen);
    if (nread < 0) {
      return (int)nread;
    }

    payload += nread;
    payloadlen -= (size_t)nread;

    if (fr->type == NGTCP2_FRAME_ACK) {
      assign_recved_ack_delay_unscaled(&fr->ack,
                                       NGTCP2_DEFAULT_ACK_DELAY_EXPONENT);
    }

    ngtcp2_log_rx_fr(&conn->log, hd, fr);

    switch (fr->type) {
    case NGTCP2_FRAME_ACK:
      rv = conn_recv_ack(conn, pktns, hd, &fr->ack, ts);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_PADDING:
      break;
    case NGTCP2_FRAME_CONNECTION_CLOSE:
      /* TODO What should I do if we get this in delayed
         Initial/Handshake? */
      break;
    case NGTCP2_FRAME_CRYPTO:
    case NGTCP2_FRAME_PING:
      require_ack = 1;
      break;
    default:
      return NGTCP2_ERR_PROTO;
    }
  }

  pktns->max_rx_pkt_num = ngtcp2_max(pktns->max_rx_pkt_num, hd->pkt_num);

  return ngtcp2_conn_sched_ack(conn, &pktns->acktr, hd->pkt_num, require_ack,
                               ts);
}

/*
 * conn_recv_max_stream_id processes the incoming MAX_STREAM_ID frame
 * |fr|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 */
static int conn_recv_max_stream_id(ngtcp2_conn *conn,
                                   const ngtcp2_max_stream_id *fr) {
  if (bidi_stream(fr->max_stream_id)) {
    conn->max_local_stream_id_bidi =
        ngtcp2_max(conn->max_local_stream_id_bidi, fr->max_stream_id);
  } else {
    conn->max_local_stream_id_uni =
        ngtcp2_max(conn->max_local_stream_id_uni, fr->max_stream_id);
  }

  return conn_call_extend_max_stream_id(conn, fr->max_stream_id);
}

static ssize_t conn_recv_pkt(ngtcp2_conn *conn, const uint8_t *pkt,
                             size_t pktlen, ngtcp2_tstamp ts) {
  ngtcp2_pkt_hd hd;
  int rv = 0;
  size_t hdpktlen;
  const uint8_t *payload;
  size_t payloadlen;
  ssize_t nread, nwrite;
  ngtcp2_max_frame mfr;
  ngtcp2_frame *fr = &mfr.fr;
  int require_ack = 0;
  ngtcp2_crypto_km *ckm;
  uint8_t plain_hdpkt[1500];
  ngtcp2_encrypt_pn encrypt_pn;
  size_t aead_overhead;
  ngtcp2_pktns *pktns;
  uint64_t crypto_rx_offset_base;
  uint64_t max_crypto_rx_offset;
  /* maybeSR becomes nonzero if an incoming packet has mismatched DCID
     and may be Stateless Reset packet. */
  int maybeSR = 0;

  if (pkt[0] & NGTCP2_HEADER_FORM_BIT) {
    nread = ngtcp2_pkt_decode_hd_long(&hd, pkt, pktlen);
    if (nread < 0) {
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "could not decode long header");
      return (ssize_t)pktlen;
    }

    switch (hd.type) {
    case NGTCP2_PKT_INITIAL:
    case NGTCP2_PKT_HANDSHAKE:
    case NGTCP2_PKT_0RTT_PROTECTED:
      break;
    default:
      ngtcp2_log_rx_pkt_hd(&conn->log, &hd);
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "packet type 0x%02x was ignored", hd.type);
      return (ssize_t)pktlen;
    }

    if (pktlen < (size_t)nread + hd.len) {
      return (ssize_t)pktlen;
    }

    pktlen = (size_t)nread + hd.len;

    if (conn->version != hd.version) {
      return (ssize_t)pktlen;
    }

    /* Quoted from spec: if subsequent packets of those types include
       a different Source Connection ID, they MUST be discarded. */
    if (!ngtcp2_cid_eq(&conn->dcid, &hd.scid)) {
      ngtcp2_log_rx_pkt_hd(&conn->log, &hd);
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "packet was ignored because of mismatched SCID");
      return (ssize_t)pktlen;
    }

    switch (hd.type) {
    case NGTCP2_PKT_INITIAL:
      if (!ngtcp2_cid_eq(&conn->scid, &hd.dcid) &&
          (!conn->server || !ngtcp2_cid_eq(&conn->rcid, &hd.dcid))) {
        ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                        "packet was ignored because of mismatched DCID");
        return (ssize_t)pktlen;
      }

      pktns = &conn->in_pktns;
      ckm = pktns->rx_ckm;
      encrypt_pn = conn->callbacks.in_encrypt_pn;
      aead_overhead = NGTCP2_INITIAL_AEAD_OVERHEAD;
      crypto_rx_offset_base = 0;
      if (conn->server && conn->early_ckm) {
        max_crypto_rx_offset = conn->early_crypto_rx_offset_base;
      } else {
        max_crypto_rx_offset = conn->hs_pktns.crypto_rx_offset_base;
      }
      break;
    case NGTCP2_PKT_HANDSHAKE:
      if (!ngtcp2_cid_eq(&conn->scid, &hd.dcid)) {
        ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                        "packet was ignored because of mismatched DCID");
        return (ssize_t)pktlen;
      }

      pktns = &conn->hs_pktns;
      ckm = pktns->rx_ckm;
      encrypt_pn = conn->callbacks.encrypt_pn;
      aead_overhead = conn->aead_overhead;
      crypto_rx_offset_base = pktns->crypto_rx_offset_base;
      max_crypto_rx_offset = conn->pktns.crypto_rx_offset_base;
      break;
    case NGTCP2_PKT_0RTT_PROTECTED:
      if (!conn->server) {
        return (ssize_t)pktlen;
      }
      if (!ngtcp2_cid_eq(&conn->rcid, &hd.dcid) &&
          !ngtcp2_cid_eq(&conn->scid, &hd.dcid)) {
        ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                        "packet was ignored because of mismatched DCID");
        return (ssize_t)pktlen;
      }

      pktns = &conn->pktns;
      if (!conn->early_ckm) {
        return (ssize_t)pktlen;
      }
      ckm = conn->early_ckm;
      encrypt_pn = conn->callbacks.encrypt_pn;
      aead_overhead = conn->aead_overhead;
      crypto_rx_offset_base = conn->early_crypto_rx_offset_base;
      max_crypto_rx_offset = conn->hs_pktns.crypto_rx_offset_base;
      break;
    default:
      assert(0);
    }
  } else {
    nread = ngtcp2_pkt_decode_hd_short(&hd, pkt, pktlen, conn->scid.datalen);
    if (nread < 0) {
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "could not decode short header");
      return (ssize_t)pktlen;
    }

    /* TODO If we check DCID here, we drop Stateless Reset packet. */
    if (!ngtcp2_cid_eq(&conn->scid, &hd.dcid)) {
      maybeSR = 1;
    }

    pktns = &conn->pktns;
    ckm = pktns->rx_ckm;
    encrypt_pn = conn->callbacks.encrypt_pn;
    aead_overhead = conn->aead_overhead;
    crypto_rx_offset_base = pktns->crypto_rx_offset_base;
    max_crypto_rx_offset = 0;
  }

  nwrite =
      conn_decrypt_pn(conn, &hd, plain_hdpkt, sizeof(plain_hdpkt), pkt, pktlen,
                      (size_t)nread, ckm, encrypt_pn, aead_overhead);
  if (nwrite < 0) {
    return (ssize_t)nwrite;
  }

  hdpktlen = (size_t)nwrite;
  payload = pkt + hdpktlen;
  payloadlen = pktlen - hdpktlen;

  hd.pkt_num = ngtcp2_pkt_adjust_pkt_num(pktns->max_rx_pkt_num, hd.pkt_num,
                                         pkt_num_bits(hd.pkt_numlen));

  ngtcp2_log_rx_pkt_hd(&conn->log, &hd);

  if (hd.flags & NGTCP2_PKT_FLAG_LONG_FORM) {
    switch (hd.type) {
    case NGTCP2_PKT_INITIAL:
    case NGTCP2_PKT_HANDSHAKE:
      /* TODO find a way when to ignore incoming handshake packet */
      rv = conn_recv_delayed_handshake_pkt(conn, &hd, payload, payloadlen,
                                           plain_hdpkt, hdpktlen, ts);
      if (ngtcp2_err_is_fatal(rv)) {
        return rv;
      }
      return (ssize_t)pktlen;
    case NGTCP2_PKT_0RTT_PROTECTED:
      break;
    default:
      /* unreachable */
      assert(0);
    }
  }

  rv = conn_ensure_decrypt_buffer(conn, payloadlen);
  if (rv != 0) {
    return rv;
  }

  nwrite = conn_decrypt_pkt(conn, conn->decrypt_buf.base, payloadlen, payload,
                            payloadlen, plain_hdpkt, hdpktlen, hd.pkt_num, ckm,
                            conn->callbacks.decrypt);
  if (nwrite < 0) {
    if (nwrite != NGTCP2_ERR_TLS_DECRYPT ||
        (hd.flags & NGTCP2_PKT_FLAG_LONG_FORM)) {
      return (int)nwrite;
    }

    if (!conn->server) {
      rv = conn_on_stateless_reset(conn, &hd, pkt + 1, pktlen - 1);
      if (rv == 0) {
        return (ssize_t)pktlen;
      }
    }
    return (int)nwrite;
  }

  if (maybeSR) {
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "packet was ignored because of mismatched DCID");
    return (ssize_t)pktlen;
  }

  payload = conn->decrypt_buf.base;
  payloadlen = (size_t)nwrite;

  if (!(hd.flags & NGTCP2_PKT_FLAG_LONG_FORM)) {
    if (!ngtcp2_cid_eq(&conn->scid, &hd.dcid)) {
      return (ssize_t)pktlen;
    }
    conn->flags |= NGTCP2_CONN_FLAG_RECV_PROTECTED_PKT;
  }

  for (; payloadlen;) {
    nread = ngtcp2_pkt_decode_frame(fr, payload, payloadlen);
    if (nread < 0) {
      return (int)nread;
    }

    payload += nread;
    payloadlen -= (size_t)nread;

    if (fr->type == NGTCP2_FRAME_ACK) {
      /* It probably illegal to send ACK in 0RTT protected packet. */
      if ((hd.flags & NGTCP2_PKT_FLAG_LONG_FORM) &&
          hd.type == NGTCP2_PKT_0RTT_PROTECTED) {
        return (ssize_t)pktlen;
      }
      assign_recved_ack_delay_unscaled(
          &fr->ack, conn->remote_settings.ack_delay_exponent);
    }

    ngtcp2_log_rx_fr(&conn->log, &hd, fr);

    if (hd.type == NGTCP2_PKT_0RTT_PROTECTED) {
      switch (fr->type) {
      case NGTCP2_FRAME_CRYPTO:
      case NGTCP2_FRAME_PADDING:
      case NGTCP2_FRAME_PING:
      case NGTCP2_FRAME_STREAM:
        break;
      default:
        return NGTCP2_ERR_PROTO;
      }
    }

    switch (fr->type) {
    case NGTCP2_FRAME_ACK:
    case NGTCP2_FRAME_PADDING:
    case NGTCP2_FRAME_CONNECTION_CLOSE:
      break;
    default:
      require_ack = 1;
    }

    switch (fr->type) {
    case NGTCP2_FRAME_ACK:
      rv = conn_recv_ack(conn, pktns, &hd, &fr->ack, ts);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_STREAM:
      rv = conn_recv_stream(conn, &fr->stream);
      if (rv != 0) {
        return rv;
      }
      conn_update_rx_bw(conn, fr->stream.datalen, ts);
      break;
    case NGTCP2_FRAME_CRYPTO:
      rv = conn_recv_crypto(conn, crypto_rx_offset_base, max_crypto_rx_offset,
                            &fr->crypto);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_RST_STREAM:
      rv = conn_recv_rst_stream(conn, &fr->rst_stream);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_STOP_SENDING:
      rv = conn_recv_stop_sending(conn, &fr->stop_sending);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_MAX_STREAM_DATA:
      rv = conn_recv_max_stream_data(conn, &fr->max_stream_data);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_MAX_DATA:
      conn_recv_max_data(conn, &fr->max_data);
      break;
    case NGTCP2_FRAME_MAX_STREAM_ID:
      rv = conn_recv_max_stream_id(conn, &fr->max_stream_id);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_CONNECTION_CLOSE:
    case NGTCP2_FRAME_APPLICATION_CLOSE:
      conn_recv_connection_close(conn);
      break;
    case NGTCP2_FRAME_PING:
      break;
    case NGTCP2_FRAME_PATH_CHALLENGE:
      conn_recv_path_challenge(conn, &fr->path_challenge, ts);
      break;
    case NGTCP2_FRAME_PATH_RESPONSE:
      conn_recv_path_response(conn, &fr->path_response);
      break;
    case NGTCP2_FRAME_BLOCKED:
    case NGTCP2_FRAME_STREAM_ID_BLOCKED:
    case NGTCP2_FRAME_NEW_CONNECTION_ID:
    case NGTCP2_FRAME_NEW_TOKEN:
      /* TODO Not implemented yet */
      break;
    }
  }

  pktns->max_rx_pkt_num = ngtcp2_max(pktns->max_rx_pkt_num, hd.pkt_num);

  rv = ngtcp2_conn_sched_ack(conn, &pktns->acktr, hd.pkt_num, require_ack, ts);
  if (rv != 0) {
    return rv;
  }
  return (ssize_t)pktlen;
}

static int conn_process_buffered_protected_pkt(ngtcp2_conn *conn,
                                               ngtcp2_tstamp ts) {
  ssize_t rv;
  ngtcp2_pkt_chain *pc = conn->buffed_rx_ppkts, *next;

  for (; pc; pc = pc->next) {
    rv = conn_recv_pkt(conn, pc->pkt, pc->pktlen, ts);
    if (rv < 0) {
      return (int)rv;
    }
  }

  for (pc = conn->buffed_rx_ppkts; pc;) {
    next = pc->next;
    ngtcp2_pkt_chain_del(pc, conn->mem);
    pc = next;
  }

  conn->buffed_rx_ppkts = NULL;

  return 0;
}

static int conn_process_buffered_handshake_pkt(ngtcp2_conn *conn,
                                               ngtcp2_tstamp ts) {
  ssize_t rv;
  ngtcp2_pkt_chain *pc = conn->buffed_rx_hs_pkts, *next;

  for (; pc; pc = pc->next) {
    rv = conn_recv_handshake_pkt(conn, pc->pkt, pc->pktlen, ts);
    if (rv < 0) {
      return (int)rv;
    }
  }

  for (pc = conn->buffed_rx_hs_pkts; pc;) {
    next = pc->next;
    ngtcp2_pkt_chain_del(pc, conn->mem);
    pc = next;
  }

  conn->buffed_rx_hs_pkts = NULL;

  return 0;
}

/*
 * conn_handshake_completed is called once cryptographic handshake has
 * completed.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 */
static int conn_handshake_completed(ngtcp2_conn *conn) {
  int rv;

  rv = conn_call_handshake_completed(conn);
  if (rv != 0) {
    return rv;
  }

  if (conn->max_local_stream_id_bidi > 0) {
    rv = conn_call_extend_max_stream_id(conn, conn->max_local_stream_id_bidi);
    if (rv != 0) {
      return rv;
    }
  }
  if (conn->max_local_stream_id_uni > 0) {
    rv = conn_call_extend_max_stream_id(conn, conn->max_local_stream_id_uni);
    if (rv != 0) {
      return rv;
    }
  }

  return 0;
}

/*
 * conn_recv_cpkt processes compound packet after handshake.  The
 * buffer pointed by |pkt| might contain multiple packets.  The Short
 * packet must be the last one because it does not have payload length
 * field.
 */
static int conn_recv_cpkt(ngtcp2_conn *conn, const uint8_t *pkt, size_t pktlen,
                          ngtcp2_tstamp ts) {
  ssize_t nread;

  while (pktlen) {
    nread = conn_recv_pkt(conn, pkt, pktlen, ts);
    if (nread < 0) {
      return (int)nread;
    }

    assert(pktlen >= (size_t)nread);
    pkt += nread;
    pktlen -= (size_t)nread;

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "read packet %zd left %zu", nread, pktlen);
  }

  return 0;
}

int ngtcp2_conn_recv(ngtcp2_conn *conn, const uint8_t *pkt, size_t pktlen,
                     ngtcp2_tstamp ts) {
  int rv = 0;

  conn->log.last_ts = ts;

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON, "recv packet len=%zu",
                  pktlen);

  if (pktlen == 0) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  switch (conn->state) {
  case NGTCP2_CS_CLIENT_INITIAL:
  case NGTCP2_CS_CLIENT_WAIT_HANDSHAKE:
  case NGTCP2_CS_CLIENT_TLS_HANDSHAKE_FAILED:
  case NGTCP2_CS_SERVER_INITIAL:
  case NGTCP2_CS_SERVER_WAIT_HANDSHAKE:
  case NGTCP2_CS_SERVER_TLS_HANDSHAKE_FAILED:
    return NGTCP2_ERR_INVALID_STATE;
  case NGTCP2_CS_CLOSING:
    return NGTCP2_ERR_CLOSING;
  case NGTCP2_CS_DRAINING:
    return NGTCP2_ERR_DRAINING;
  case NGTCP2_CS_POST_HANDSHAKE:
    rv = conn_recv_cpkt(conn, pkt, pktlen, ts);
    if (rv != 0) {
      break;
    }
    if (conn->state == NGTCP2_CS_DRAINING) {
      return NGTCP2_ERR_DRAINING;
    }
    break;
  }

  return rv;
}

/*
 * conn_handle_delayed_ack_expiry handles expired delayed ACK timer
 * during handshake.  The delayed ACK timer is only used for ACK frame
 * in protected packet, but it starts during handshake.  Application
 * may be awakened by this timer and attempt to send ACK, but this is
 * ACK for protected packet.  We have no key so we cannot send any
 * protected packet.  Timer keeps firing and it could become busy loop
 * until handshake finishes.  We can disable it during handshake, but
 * 0-RTT stuff complicates this.  This function checks that delayed
 * ACK timer has expired, and if so disarm timer.  We have internal
 * flag which shows that timer has expired.
 */
static void conn_handle_delayed_ack_expiry(ngtcp2_conn *conn,
                                           ngtcp2_tstamp ts) {
  if (ngtcp2_conn_ack_delay_expiry(conn) > ts) {
    return;
  }

  ngtcp2_acktr_expire_delayed_ack(&conn->pktns.acktr);
}

static int conn_check_pkt_num_exhausted(ngtcp2_conn *conn) {
  return conn->in_pktns.last_tx_pkt_num == NGTCP2_MAX_PKT_NUM ||
         conn->hs_pktns.last_tx_pkt_num == NGTCP2_MAX_PKT_NUM ||
         conn->pktns.last_tx_pkt_num == NGTCP2_MAX_PKT_NUM;
}

static ssize_t conn_handshake(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                              const uint8_t *pkt, size_t pktlen,
                              int require_padding, ngtcp2_tstamp ts) {
  int rv;
  ssize_t res = 0, nwrite, early_spktlen;
  uint64_t cwnd;
  size_t origlen = destlen;
  ngtcp2_pktns *hs_pktns = &conn->hs_pktns;

  conn->log.last_ts = ts;

  if (pktlen > 0) {
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON, "recv packet len=%zu",
                    pktlen);
  }

  conn_handle_delayed_ack_expiry(conn, ts);

  if (conn_check_pkt_num_exhausted(conn)) {
    return NGTCP2_ERR_PKT_NUM_EXHAUSTED;
  }

  cwnd = conn_cwnd_left(conn);
  destlen = ngtcp2_min(destlen, cwnd);

  switch (conn->state) {
  case NGTCP2_CS_CLIENT_INITIAL:
    if (pktlen > 0) {
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
    if (cwnd < NGTCP2_MIN_PKTLEN) {
      return NGTCP2_ERR_CONGESTION;
    }
    nwrite =
        conn_write_client_initial(conn, dest, destlen, require_padding, ts);
    if (nwrite < 0) {
      return nwrite;
    }
    conn->state = NGTCP2_CS_CLIENT_WAIT_HANDSHAKE;

    return nwrite;
  case NGTCP2_CS_CLIENT_WAIT_HANDSHAKE:
    rv = conn_recv_handshake_cpkt(conn, pkt, pktlen, ts);
    if (rv < 0) {
      return rv;
    }

    if (conn->state == NGTCP2_CS_CLIENT_INITIAL) {
      require_padding = !conn->retry_early_rtb;
      nwrite =
          conn_write_client_initial(conn, dest, destlen, require_padding, ts);
      if (nwrite < 0) {
        return nwrite;
      }
      conn->state = NGTCP2_CS_CLIENT_WAIT_HANDSHAKE;

      early_spktlen = conn_retransmit_retry_early(conn, dest + nwrite,
                                                  destlen - (size_t)nwrite,
                                                  1 /* require_padding */, ts);
      if (early_spktlen < 0) {
        if (ngtcp2_err_is_fatal((int)early_spktlen)) {
          return early_spktlen;
        }
        return nwrite;
      }

      return nwrite + early_spktlen;
    }

    if (hs_pktns->rx_ckm) {
      rv = conn_process_buffered_handshake_pkt(conn, ts);
      if (rv != 0) {
        return rv;
      }
    }

    if (cwnd < NGTCP2_MIN_PKTLEN) {
      nwrite = conn_write_handshake_ack_pkts(conn, dest, origlen, ts);
      if (nwrite == 0) {
        return NGTCP2_ERR_CONGESTION;
      }
      return nwrite;
    }

    nwrite = conn_retransmit(conn, dest, destlen, ts);
    if (nwrite) {
      return nwrite;
    }

    nwrite = conn_retransmit_retry_early(conn, dest, destlen,
                                         0 /* require_padding */, ts);
    if (nwrite) {
      return nwrite;
    }

    nwrite =
        conn_write_client_handshake(conn, dest, destlen, require_padding, ts);
    if (nwrite < 0) {
      return nwrite;
    }

    res += nwrite;
    dest += nwrite;
    destlen -= (size_t)nwrite;

    nwrite = conn_write_handshake_ack_pkts(conn, dest, destlen, ts);
    if (nwrite < 0) {
      if (nwrite != NGTCP2_ERR_NOBUF) {
        return nwrite;
      }
    } else {
      res += nwrite;
    }

    if (!(conn->flags & NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED) ||
        ngtcp2_ringbuf_len(&conn->tx_crypto_data)) {
      return res;
    }

    if (!(conn->flags & NGTCP2_CONN_FLAG_TRANSPORT_PARAM_RECVED)) {
      return NGTCP2_ERR_REQUIRED_TRANSPORT_PARAM;
    }

    rv = conn_handshake_completed(conn);
    if (rv != 0) {
      return (ssize_t)rv;
    }

    conn->state = NGTCP2_CS_POST_HANDSHAKE;

    rv = conn_process_early_rtb(conn);
    if (rv != 0) {
      return (ssize_t)rv;
    }

    rv = conn_process_buffered_protected_pkt(conn, ts);
    if (rv != 0) {
      return (ssize_t)rv;
    }

    return res;
  case NGTCP2_CS_SERVER_INITIAL:
    rv = conn_recv_handshake_cpkt(conn, pkt, pktlen, ts);
    if (rv < 0) {
      /* TODO Draft says nothing about how to notify the peer that TLS
         stack failed before getting handshake key */
      return rv;
    }

    if (ngtcp2_rob_first_gap_offset(&conn->crypto.rob) == 0) {
      return 0;
    }

    /* Process re-ordered 0-RTT Protected packets which were
       arrived before Initial packet. */
    rv = conn_process_buffered_protected_pkt(conn, ts);
    if (rv != 0) {
      return (ssize_t)rv;
    }

    if (cwnd < NGTCP2_MIN_PKTLEN) {
      /* TODO This should be conn_write_initial_ack_pkt */
      nwrite = conn_write_handshake_ack_pkts(conn, dest, origlen, ts);
      if (nwrite == 0) {
        return NGTCP2_ERR_CONGESTION;
      }
      return nwrite;
    }

    nwrite = conn_write_server_handshake(conn, dest, destlen, ts);
    if (nwrite < 0) {
      return (ssize_t)rv;
    }

    conn->state = NGTCP2_CS_SERVER_WAIT_HANDSHAKE;

    return nwrite;
  case NGTCP2_CS_SERVER_WAIT_HANDSHAKE:
    rv = conn_recv_handshake_cpkt(conn, pkt, pktlen, ts);
    if (rv < 0) {
      return rv;
    }

    if (hs_pktns->rx_ckm) {
      rv = conn_process_buffered_handshake_pkt(conn, ts);
      if (rv != 0) {
        return rv;
      }
    }

    nwrite = conn_retransmit(conn, dest, destlen, ts);
    if (nwrite) {
      return nwrite;
    }

    if (!(conn->flags & NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED)) {
      if (cwnd < NGTCP2_MIN_PKTLEN) {
        nwrite = conn_write_handshake_ack_pkts(conn, dest, origlen, ts);
        if (nwrite == 0) {
          return NGTCP2_ERR_CONGESTION;
        }
        return nwrite;
      }

      nwrite = conn_write_server_handshake(conn, dest, destlen, ts);
      if (nwrite < 0) {
        return nwrite;
      }

      res += nwrite;
      dest += nwrite;
      destlen -= (size_t)nwrite;

      nwrite = conn_write_handshake_ack_pkts(conn, dest, destlen, ts);
      if (nwrite < 0) {
        if (nwrite != NGTCP2_ERR_NOBUF) {
          return nwrite;
        }
      } else {
        res += nwrite;
      }
      return res;
    }

    nwrite = conn_write_handshake_ack_pkts(conn, dest, origlen, ts);
    if (nwrite < 0) {
      if (nwrite != NGTCP2_ERR_NOBUF) {
        return nwrite;
      }
    } else {
      res += nwrite;
    }

    if (!(conn->flags & NGTCP2_CONN_FLAG_TRANSPORT_PARAM_RECVED)) {
      return NGTCP2_ERR_REQUIRED_TRANSPORT_PARAM;
    }

    rv = conn_handshake_completed(conn);
    if (rv != 0) {
      return (ssize_t)rv;
    }
    conn->state = NGTCP2_CS_POST_HANDSHAKE;
    /* The receipt of the final cryptographic message from the client
       verifies source address. */
    conn->flags |= NGTCP2_CONN_FLAG_SADDR_VERIFIED;

    rv = conn_process_buffered_protected_pkt(conn, ts);
    if (rv != 0) {
      return (ssize_t)rv;
    }

    conn->hs_pktns.acktr.flags |= NGTCP2_ACKTR_FLAG_PENDING_FINISHED_ACK;

    return res;
  case NGTCP2_CS_CLOSING:
    return NGTCP2_ERR_CLOSING;
  case NGTCP2_CS_DRAINING:
    return NGTCP2_ERR_DRAINING;
  default:
    return 0;
  }
}

ssize_t ngtcp2_conn_handshake(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                              const uint8_t *pkt, size_t pktlen,
                              ngtcp2_tstamp ts) {
  return conn_handshake(conn, dest, destlen, pkt, pktlen, !conn->server, ts);
}

static ssize_t conn_write_stream_early(ngtcp2_conn *conn, uint8_t *dest,
                                       size_t destlen, ssize_t *pdatalen,
                                       ngtcp2_strm *strm, uint8_t fin,
                                       const uint8_t *data, size_t datalen,
                                       int require_padding, ngtcp2_tstamp ts) {
  ngtcp2_crypto_ctx ctx;
  ngtcp2_ppe ppe;
  ngtcp2_rtb_entry *ent;
  ngtcp2_frame_chain *frc;
  ngtcp2_frame localfr;
  ngtcp2_pkt_hd hd;
  int rv;
  size_t ndatalen, left;
  ssize_t nwrite;
  uint8_t pkt_flags;
  uint8_t pkt_type;
  ngtcp2_pktns *pktns = &conn->pktns;

  assert(!conn->server);
  assert(conn->early_ckm);

  pkt_flags = NGTCP2_PKT_FLAG_LONG_FORM;
  pkt_type = NGTCP2_PKT_0RTT_PROTECTED;
  ctx.ckm = conn->early_ckm;

  ngtcp2_pkt_hd_init(
      &hd, pkt_flags, pkt_type, &conn->dcid, &conn->scid,
      pktns->last_tx_pkt_num + 1,
      rtb_select_pkt_numlen(&pktns->rtb, pktns->last_tx_pkt_num + 1),
      conn->version, 0);

  ctx.aead_overhead = conn->aead_overhead;
  ctx.encrypt = conn->callbacks.encrypt;
  ctx.encrypt_pn = conn->callbacks.encrypt_pn;
  ctx.user_data = conn;

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx);

  ngtcp2_log_tx_pkt_hd(&conn->log, &hd);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    return rv;
  }

  left = ngtcp2_ppe_left(&ppe);
  if (left < NGTCP2_STREAM_OVERHEAD + NGTCP2_MIN_FRAME_PAYLOADLEN) {
    return NGTCP2_ERR_NOBUF;
  }

  left -= NGTCP2_STREAM_OVERHEAD;

  ndatalen = ngtcp2_min(datalen, left);
  ndatalen = ngtcp2_min(ndatalen, strm->max_tx_offset - strm->tx_offset);
  ndatalen = ngtcp2_min(ndatalen, conn->max_tx_offset - conn->tx_offset);

  if (datalen > 0 && ndatalen == 0) {
    return NGTCP2_ERR_STREAM_DATA_BLOCKED;
  }

  fin = fin && ndatalen == datalen;

  rv = ngtcp2_frame_chain_new(&frc, conn->mem);
  if (rv != 0) {
    return rv;
  }

  frc->fr.type = NGTCP2_FRAME_STREAM;
  frc->fr.stream.flags = 0;
  frc->fr.stream.fin = fin;
  frc->fr.stream.stream_id = strm->stream_id;
  frc->fr.stream.offset = strm->tx_offset;
  frc->fr.stream.datalen = ndatalen;
  frc->fr.stream.data = data;

  rv = ngtcp2_ppe_encode_frame(&ppe, &frc->fr);
  if (rv != 0) {
    ngtcp2_frame_chain_del(frc, conn->mem);
    return rv;
  }

  ngtcp2_log_tx_fr(&conn->log, &hd, &frc->fr);

  if (require_padding) {
    localfr.type = NGTCP2_FRAME_PADDING;
    localfr.padding.len = ngtcp2_ppe_padding(&ppe);

    ngtcp2_log_tx_fr(&conn->log, &hd, &localfr);
  }

  nwrite = ngtcp2_ppe_final(&ppe, NULL);
  if (nwrite < 0) {
    ngtcp2_frame_chain_del(frc, conn->mem);
    return nwrite;
  }

  rv = ngtcp2_rtb_entry_new(&ent, &hd, frc, ts, (size_t)nwrite,
                            NGTCP2_RTB_FLAG_NONE, conn->mem);
  if (rv != 0) {
    ngtcp2_frame_chain_del(frc, conn->mem);
    return rv;
  }

  conn_on_pkt_sent(conn, &pktns->rtb, ent);

  strm->tx_offset += ndatalen;
  conn->tx_offset += ndatalen;

  ++pktns->last_tx_pkt_num;

  if (pdatalen) {
    *pdatalen = (ssize_t)ndatalen;
  }

  if (fin) {
    ngtcp2_strm_shutdown(strm, NGTCP2_STRM_FLAG_SHUT_WR);
  }

  return nwrite;
}

ssize_t ngtcp2_conn_client_handshake(ngtcp2_conn *conn, uint8_t *dest,
                                     size_t destlen, ssize_t *pdatalen,
                                     const uint8_t *pkt, size_t pktlen,
                                     uint64_t stream_id, uint8_t fin,
                                     const uint8_t *data, size_t datalen,
                                     ngtcp2_tstamp ts) {
  ngtcp2_strm *strm = NULL;
  int send_stream = 0;
  ssize_t spktlen, early_spktlen;
  uint64_t cwnd;
  int require_padding;
  int was_client_initial;

  if (pdatalen) {
    *pdatalen = -1;
  }

  if (conn->server) {
    return NGTCP2_ERR_INVALID_STATE;
  }

  /* conn->early_ckm might be created in the first call of
     conn_handshake().  Check it later. */
  if (stream_id != (uint64_t)-1 &&
      !(conn->flags & NGTCP2_CONN_FLAG_EARLY_DATA_REJECTED)) {
    strm = ngtcp2_conn_find_stream(conn, stream_id);
    if (strm == NULL) {
      return NGTCP2_ERR_STREAM_NOT_FOUND;
    }

    if (strm->flags & NGTCP2_STRM_FLAG_SHUT_WR) {
      return NGTCP2_ERR_STREAM_SHUT_WR;
    }

    send_stream = !conn->retry_early_rtb &&
                  ((datalen == 0 && fin) ||
                   (datalen > 0 && (strm->max_tx_offset - strm->tx_offset) &&
                    (conn->max_tx_offset - conn->tx_offset)));
  }

  was_client_initial = conn->state == NGTCP2_CS_CLIENT_INITIAL;
  spktlen = conn_handshake(conn, dest, destlen, pkt, pktlen, !send_stream, ts);

  if (spktlen < 0) {
    return spktlen;
  }

  if (conn->pktns.tx_ckm || !conn->early_ckm || !send_stream) {
    return spktlen;
  }

  /* If spktlen > 0, we are making a compound packet.  If Initial
     packet is written, we have to pad bytes to 0-RTT Protected
     packet. */

  require_padding = spktlen && was_client_initial;

  cwnd = conn_cwnd_left(conn);

  dest += spktlen;
  destlen -= (size_t)spktlen;
  destlen = ngtcp2_min(destlen, cwnd);

  early_spktlen =
      conn_write_stream_early(conn, dest, destlen, pdatalen, strm, fin, data,
                              datalen, require_padding, ts);

  switch (early_spktlen) {
  case NGTCP2_ERR_NOBUF:
  case NGTCP2_ERR_STREAM_DATA_BLOCKED:
    return spktlen;
  }

  return spktlen + early_spktlen;
}

void ngtcp2_conn_handshake_completed(ngtcp2_conn *conn) {
  conn->flags |= NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED;
}

int ngtcp2_conn_get_handshake_completed(ngtcp2_conn *conn) {
  return (conn->flags & NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED) > 0;
}

int ngtcp2_conn_sched_ack(ngtcp2_conn *conn, ngtcp2_acktr *acktr,
                          uint64_t pkt_num, int active_ack, ngtcp2_tstamp ts) {
  ngtcp2_acktr_entry *rpkt;
  int rv;

  rv = ngtcp2_acktr_entry_new(&rpkt, pkt_num, ts, conn->mem);
  if (rv != 0) {
    return rv;
  }

  rv = ngtcp2_acktr_add(acktr, rpkt, active_ack, ts);
  if (rv != 0) {
    ngtcp2_acktr_entry_del(rpkt, conn->mem);
    /* NGTCP2_ERR_INVALID_ARGUMENT means duplicated packet number.
       Just ignore it for now. */
    if (rv != NGTCP2_ERR_INVALID_ARGUMENT) {
      return rv;
    }
    return 0;
  }

  return 0;
}

int ngtcp2_accept(ngtcp2_pkt_hd *dest, const uint8_t *pkt, size_t pktlen) {
  ssize_t nread;
  ngtcp2_pkt_hd hd, *p;

  if (dest) {
    p = dest;
  } else {
    p = &hd;
  }

  if (pktlen == 0 || (pkt[0] & NGTCP2_HEADER_FORM_BIT) == 0) {
    return -1;
  }

  nread = ngtcp2_pkt_decode_hd_long(p, pkt, pktlen);
  if (nread < 0) {
    return -1;
  }

  switch (p->type) {
  case NGTCP2_PKT_INITIAL:
    /* 0-RTT Protected packet may arrive before Initial packet due to
       re-ordering. */
  case NGTCP2_PKT_0RTT_PROTECTED:
    break;
  default:
    return -1;
  }

  switch (p->version) {
  case NGTCP2_PROTO_VER_D14:
    break;
  default:
    return 1;
  }

  return 0;
}

void ngtcp2_conn_set_aead_overhead(ngtcp2_conn *conn, size_t aead_overhead) {
  conn->aead_overhead = aead_overhead;
}

int ngtcp2_conn_set_initial_tx_keys(ngtcp2_conn *conn, const uint8_t *key,
                                    size_t keylen, const uint8_t *iv,
                                    size_t ivlen, const uint8_t *pn,
                                    size_t pnlen) {
  ngtcp2_pktns *pktns = &conn->in_pktns;

  if (pktns->tx_ckm) {
    ngtcp2_crypto_km_del(pktns->tx_ckm, conn->mem);
    pktns->tx_ckm = NULL;
  }

  return ngtcp2_crypto_km_new(&pktns->tx_ckm, key, keylen, iv, ivlen, pn, pnlen,
                              conn->mem);
}

int ngtcp2_conn_set_initial_rx_keys(ngtcp2_conn *conn, const uint8_t *key,
                                    size_t keylen, const uint8_t *iv,
                                    size_t ivlen, const uint8_t *pn,
                                    size_t pnlen) {
  ngtcp2_pktns *pktns = &conn->in_pktns;

  if (pktns->rx_ckm) {
    ngtcp2_crypto_km_del(pktns->rx_ckm, conn->mem);
    pktns->rx_ckm = NULL;
  }

  return ngtcp2_crypto_km_new(&pktns->rx_ckm, key, keylen, iv, ivlen, pn, pnlen,
                              conn->mem);
}

int ngtcp2_conn_set_handshake_tx_keys(ngtcp2_conn *conn, const uint8_t *key,
                                      size_t keylen, const uint8_t *iv,
                                      size_t ivlen, const uint8_t *pn,
                                      size_t pnlen) {
  ngtcp2_pktns *pktns = &conn->hs_pktns;

  if (pktns->tx_ckm) {
    ngtcp2_crypto_km_del(pktns->tx_ckm, conn->mem);
    pktns->tx_ckm = NULL;
  }

  return ngtcp2_crypto_km_new(&pktns->tx_ckm, key, keylen, iv, ivlen, pn, pnlen,
                              conn->mem);
}

int ngtcp2_conn_set_handshake_rx_keys(ngtcp2_conn *conn, const uint8_t *key,
                                      size_t keylen, const uint8_t *iv,
                                      size_t ivlen, const uint8_t *pn,
                                      size_t pnlen) {
  ngtcp2_pktns *pktns = &conn->hs_pktns;

  if (pktns->rx_ckm) {
    ngtcp2_crypto_km_del(pktns->rx_ckm, conn->mem);
    pktns->rx_ckm = NULL;
  }

  conn->hs_pktns.crypto_rx_offset_base = conn->crypto.last_rx_offset;

  return ngtcp2_crypto_km_new(&pktns->rx_ckm, key, keylen, iv, ivlen, pn, pnlen,
                              conn->mem);
}

int ngtcp2_conn_set_early_keys(ngtcp2_conn *conn, const uint8_t *key,
                               size_t keylen, const uint8_t *iv, size_t ivlen,
                               const uint8_t *pn, size_t pnlen) {
  if (conn->early_ckm) {
    return NGTCP2_ERR_INVALID_STATE;
  }

  if (conn->server) {
    conn->early_crypto_rx_offset_base = conn->crypto.last_rx_offset;
  }

  return ngtcp2_crypto_km_new(&conn->early_ckm, key, keylen, iv, ivlen, pn,
                              pnlen, conn->mem);
}

int ngtcp2_conn_update_tx_keys(ngtcp2_conn *conn, const uint8_t *key,
                               size_t keylen, const uint8_t *iv, size_t ivlen,
                               const uint8_t *pn, size_t pnlen) {
  ngtcp2_pktns *pktns = &conn->pktns;

  if (pktns->tx_ckm) {
    return NGTCP2_ERR_INVALID_STATE;
  }

  return ngtcp2_crypto_km_new(&pktns->tx_ckm, key, keylen, iv, ivlen, pn, pnlen,
                              conn->mem);
}

int ngtcp2_conn_update_rx_keys(ngtcp2_conn *conn, const uint8_t *key,
                               size_t keylen, const uint8_t *iv, size_t ivlen,
                               const uint8_t *pn, size_t pnlen) {
  ngtcp2_pktns *pktns = &conn->pktns;

  if (pktns->rx_ckm) {
    return NGTCP2_ERR_INVALID_STATE;
  }

  /* TODO This must be done once */
  if (conn->pktns.crypto_rx_offset_base == 0) {
    conn->pktns.crypto_rx_offset_base = conn->crypto.last_rx_offset;
  }

  return ngtcp2_crypto_km_new(&pktns->rx_ckm, key, keylen, iv, ivlen, pn, pnlen,
                              conn->mem);
}

ngtcp2_tstamp ngtcp2_conn_loss_detection_expiry(ngtcp2_conn *conn) {
  if (conn->rcs.loss_detection_timer) {
    return conn->rcs.loss_detection_timer;
  }
  return UINT64_MAX;
}

ngtcp2_tstamp ngtcp2_conn_ack_delay_expiry(ngtcp2_conn *conn) {
  ngtcp2_acktr *acktr = &conn->pktns.acktr;

  if (acktr->first_unacked_ts == UINT64_MAX) {
    return UINT64_MAX;
  }
  return acktr->first_unacked_ts + conn_compute_ack_delay(conn);
}

int ngtcp2_pkt_chain_new(ngtcp2_pkt_chain **ppc, const uint8_t *pkt,
                         size_t pktlen, ngtcp2_tstamp ts, ngtcp2_mem *mem) {
  *ppc = ngtcp2_mem_malloc(mem, sizeof(ngtcp2_pkt_chain) + pktlen);
  if (*ppc == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  (*ppc)->next = NULL;
  (*ppc)->pkt = (uint8_t *)(*ppc) + sizeof(ngtcp2_pkt_chain);
  (*ppc)->pktlen = pktlen;
  (*ppc)->ts = ts;

  memcpy((*ppc)->pkt, pkt, pktlen);

  return 0;
}

void ngtcp2_pkt_chain_del(ngtcp2_pkt_chain *pc, ngtcp2_mem *mem) {
  ngtcp2_mem_free(mem, pc);
}

static void
settings_copy_from_transport_params(ngtcp2_settings *dest,
                                    const ngtcp2_transport_params *src) {
  dest->max_stream_data_bidi_local = src->initial_max_stream_data_bidi_local;
  dest->max_stream_data_bidi_remote = src->initial_max_stream_data_bidi_remote;
  dest->max_stream_data_uni = src->initial_max_stream_data_uni;
  dest->max_data = src->initial_max_data;
  dest->max_bidi_streams = src->initial_max_bidi_streams;
  dest->max_uni_streams = src->initial_max_uni_streams;
  dest->idle_timeout = src->idle_timeout;
  dest->max_packet_size = src->max_packet_size;
  dest->stateless_reset_token_present = src->stateless_reset_token_present;
  if (src->stateless_reset_token_present) {
    memcpy(dest->stateless_reset_token, src->stateless_reset_token,
           sizeof(dest->stateless_reset_token));
  } else {
    memset(dest->stateless_reset_token, 0, sizeof(dest->stateless_reset_token));
  }
  dest->ack_delay_exponent = src->ack_delay_exponent;
  dest->disable_migration = src->disable_migration;
  dest->preferred_address = src->preferred_address;
}

static void transport_params_copy_from_settings(ngtcp2_transport_params *dest,
                                                const ngtcp2_settings *src) {
  dest->initial_max_stream_data_bidi_local = src->max_stream_data_bidi_local;
  dest->initial_max_stream_data_bidi_remote = src->max_stream_data_bidi_remote;
  dest->initial_max_stream_data_uni = src->max_stream_data_uni;
  dest->initial_max_data = src->max_data;
  dest->initial_max_bidi_streams = src->max_bidi_streams;
  dest->initial_max_uni_streams = src->max_uni_streams;
  dest->idle_timeout = src->idle_timeout;
  dest->max_packet_size = src->max_packet_size;
  dest->stateless_reset_token_present = src->stateless_reset_token_present;
  if (src->stateless_reset_token_present) {
    memcpy(dest->stateless_reset_token, src->stateless_reset_token,
           sizeof(dest->stateless_reset_token));
  } else {
    memset(dest->stateless_reset_token, 0, sizeof(dest->stateless_reset_token));
  }
  dest->ack_delay_exponent = src->ack_delay_exponent;
  dest->disable_migration = src->disable_migration;
  dest->preferred_address = src->preferred_address;
}

/*
 * conn_client_validate_transport_params validates |params| as client.
 * |params| must be sent with Encrypted Extensions.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_VERSION_NEGOTIATION
 *     The negotiated version is invalid.
 */
static int
conn_client_validate_transport_params(ngtcp2_conn *conn,
                                      const ngtcp2_transport_params *params) {
  size_t i;

  if (params->v.ee.negotiated_version != conn->version) {
    return NGTCP2_ERR_VERSION_NEGOTIATION;
  }

  for (i = 0; i < params->v.ee.len; ++i) {
    if (params->v.ee.supported_versions[i] == conn->version) {
      return 0;
    }
  }

  return NGTCP2_ERR_VERSION_NEGOTIATION;
}

int ngtcp2_conn_set_remote_transport_params(
    ngtcp2_conn *conn, uint8_t exttype, const ngtcp2_transport_params *params) {
  int rv;

  switch (exttype) {
  case NGTCP2_TRANSPORT_PARAMS_TYPE_CLIENT_HELLO:
    if (!conn->server) {
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
    /* TODO At the moment, we only support one version, and there is
       no validation here. */
    break;
  case NGTCP2_TRANSPORT_PARAMS_TYPE_ENCRYPTED_EXTENSIONS:
    if (conn->server) {
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
    rv = conn_client_validate_transport_params(conn, params);
    if (rv != 0) {
      return rv;
    }
    break;
  default:
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  ngtcp2_log_remote_tp(&conn->log, exttype, params);

  settings_copy_from_transport_params(&conn->remote_settings, params);

  if (conn->server) {
    conn->max_local_stream_id_bidi =
        ngtcp2_nth_server_bidi_id(conn->remote_settings.max_bidi_streams);
    conn->max_local_stream_id_uni =
        ngtcp2_nth_server_uni_id(conn->remote_settings.max_uni_streams);
  } else {
    conn->max_local_stream_id_bidi =
        ngtcp2_nth_client_bidi_id(conn->remote_settings.max_bidi_streams);
    conn->max_local_stream_id_uni =
        ngtcp2_nth_client_uni_id(conn->remote_settings.max_uni_streams);
  }

  conn->max_tx_offset = conn->remote_settings.max_data;

  conn->flags |= NGTCP2_CONN_FLAG_TRANSPORT_PARAM_RECVED;

  return 0;
}

int ngtcp2_conn_set_early_remote_transport_params(
    ngtcp2_conn *conn, const ngtcp2_transport_params *params) {
  if (conn->server) {
    return NGTCP2_ERR_INVALID_STATE;
  }

  settings_copy_from_transport_params(&conn->remote_settings, params);

  if (conn->server) {
    conn->max_local_stream_id_bidi =
        ngtcp2_nth_server_bidi_id(conn->remote_settings.max_bidi_streams);
    conn->max_local_stream_id_uni =
        ngtcp2_nth_server_uni_id(conn->remote_settings.max_uni_streams);
  } else {
    conn->max_local_stream_id_bidi =
        ngtcp2_nth_client_bidi_id(conn->remote_settings.max_bidi_streams);
    conn->max_local_stream_id_uni =
        ngtcp2_nth_client_uni_id(conn->remote_settings.max_uni_streams);
  }

  conn->max_tx_offset = conn->remote_settings.max_data;

  return 0;
}

int ngtcp2_conn_get_local_transport_params(ngtcp2_conn *conn,
                                           ngtcp2_transport_params *params,
                                           uint8_t exttype) {
  switch (exttype) {
  case NGTCP2_TRANSPORT_PARAMS_TYPE_CLIENT_HELLO:
    if (conn->server) {
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
    /* TODO Fix this; not sure how to handle them correctly */
    params->v.ch.initial_version = conn->version;
    break;
  case NGTCP2_TRANSPORT_PARAMS_TYPE_ENCRYPTED_EXTENSIONS:
    if (!conn->server) {
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
    /* TODO Fix this; not sure how to handle them correctly */
    params->v.ee.negotiated_version = conn->version;
    params->v.ee.len = 1;
    params->v.ee.supported_versions[0] = conn->version;
    break;
  default:
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }
  transport_params_copy_from_settings(params, &conn->local_settings);
  return 0;
}

int ngtcp2_conn_open_bidi_stream(ngtcp2_conn *conn, uint64_t *pstream_id,
                                 void *stream_user_data) {
  int rv;
  ngtcp2_strm *strm;

  if (conn->next_local_stream_id_bidi > conn->max_local_stream_id_bidi) {
    return NGTCP2_ERR_STREAM_ID_BLOCKED;
  }

  strm = ngtcp2_mem_malloc(conn->mem, sizeof(ngtcp2_strm));
  if (strm == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  rv = ngtcp2_conn_init_stream(conn, strm, conn->next_local_stream_id_bidi,
                               stream_user_data);
  if (rv != 0) {
    return rv;
  }

  *pstream_id = conn->next_local_stream_id_bidi;
  conn->next_local_stream_id_bidi += 4;

  return 0;
}

int ngtcp2_conn_open_uni_stream(ngtcp2_conn *conn, uint64_t *pstream_id,
                                void *stream_user_data) {
  int rv;
  ngtcp2_strm *strm;

  if (conn->next_local_stream_id_uni > conn->max_local_stream_id_uni) {
    return NGTCP2_ERR_STREAM_ID_BLOCKED;
  }

  strm = ngtcp2_mem_malloc(conn->mem, sizeof(ngtcp2_strm));
  if (strm == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  rv = ngtcp2_conn_init_stream(conn, strm, conn->next_local_stream_id_uni,
                               stream_user_data);
  if (rv != 0) {
    return rv;
  }
  ngtcp2_strm_shutdown(strm, NGTCP2_STRM_FLAG_SHUT_RD);

  *pstream_id = conn->next_local_stream_id_uni;
  conn->next_local_stream_id_uni += 4;

  return 0;
}

ngtcp2_strm *ngtcp2_conn_find_stream(ngtcp2_conn *conn, uint64_t stream_id) {
  ngtcp2_map_entry *me;

  me = ngtcp2_map_find(&conn->strms, stream_id);
  if (me == NULL) {
    return NULL;
  }

  return ngtcp2_struct_of(me, ngtcp2_strm, me);
}

ssize_t ngtcp2_conn_write_stream(ngtcp2_conn *conn, uint8_t *dest,
                                 size_t destlen, ssize_t *pdatalen,
                                 uint64_t stream_id, uint8_t fin,
                                 const uint8_t *data, size_t datalen,
                                 ngtcp2_tstamp ts) {
  ngtcp2_strm *strm;
  ssize_t nwrite;
  uint64_t cwnd;
  ngtcp2_pktns *pktns = &conn->pktns;

  conn->log.last_ts = ts;

  if (pdatalen) {
    *pdatalen = -1;
  }

  switch (conn->state) {
  case NGTCP2_CS_CLOSING:
    return NGTCP2_ERR_CLOSING;
  case NGTCP2_CS_DRAINING:
    return NGTCP2_ERR_DRAINING;
  }

  if (conn_check_pkt_num_exhausted(conn)) {
    return NGTCP2_ERR_PKT_NUM_EXHAUSTED;
  }

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  if (strm == NULL) {
    return NGTCP2_ERR_STREAM_NOT_FOUND;
  }

  if (strm->flags & NGTCP2_STRM_FLAG_SHUT_WR) {
    return NGTCP2_ERR_STREAM_SHUT_WR;
  }

  nwrite = conn_write_handshake_ack_pkts(conn, dest, destlen, ts);
  if (nwrite) {
    return nwrite;
  }

  cwnd = conn_cwnd_left(conn);

  if (cwnd >= NGTCP2_MIN_PKTLEN) {
    nwrite = conn_retransmit(conn, dest, ngtcp2_min(destlen, cwnd), ts);
    if (nwrite) {
      return nwrite;
    }
  }

  if (pktns->tx_ckm) {
    if (conn->rcs.probe_pkt_left) {
      return conn_write_probe_pkt(conn, dest, destlen, pdatalen, strm, fin,
                                  data, datalen, ts);
    }

    if (cwnd < NGTCP2_MIN_PKTLEN) {
      nwrite = conn_write_protected_ack_pkt(conn, dest, destlen, ts);
      if (nwrite) {
        return nwrite;
      }
      return NGTCP2_ERR_CONGESTION;
    }

    nwrite = conn_write_pkt(conn, dest, destlen = ngtcp2_min(destlen, cwnd),
                            pdatalen, strm, fin, data, datalen, ts);
    if (nwrite) {
      return nwrite;
    }
    if (datalen) {
      return NGTCP2_ERR_STREAM_DATA_BLOCKED;
    }
    return 0;
  }

  /* Send STREAM frame in 0-RTT packet. */
  if (conn->server || !conn->early_ckm) {
    return NGTCP2_ERR_NOKEY;
  }

  if (conn->flags & NGTCP2_CONN_FLAG_EARLY_DATA_REJECTED) {
    return NGTCP2_ERR_EARLY_DATA_REJECTED;
  }

  if (cwnd < NGTCP2_MIN_PKTLEN) {
    return NGTCP2_ERR_CONGESTION;
  }

  nwrite = conn_retransmit_retry_early(conn, dest, ngtcp2_min(destlen, cwnd),
                                       0 /* require_padding */, ts);
  if (nwrite) {
    return nwrite;
  }

  return conn_write_stream_early(conn, dest, ngtcp2_min(destlen, cwnd),
                                 pdatalen, strm, fin, data, datalen,
                                 0 /* require_padding */, ts);
}

ssize_t ngtcp2_conn_write_connection_close(ngtcp2_conn *conn, uint8_t *dest,
                                           size_t destlen, uint16_t error_code,
                                           ngtcp2_tstamp ts) {
  ssize_t nwrite;
  ngtcp2_frame fr;
  uint8_t pkt_type;

  conn->log.last_ts = ts;

  if (conn_check_pkt_num_exhausted(conn)) {
    return NGTCP2_ERR_PKT_NUM_EXHAUSTED;
  }

  switch (conn->state) {
  case NGTCP2_CS_CLOSING:
  case NGTCP2_CS_DRAINING:
    return NGTCP2_ERR_INVALID_STATE;
  }

  fr.type = NGTCP2_FRAME_CONNECTION_CLOSE;
  fr.connection_close.error_code = error_code;
  fr.connection_close.frame_type = 0;
  fr.connection_close.reasonlen = 0;
  fr.connection_close.reason = NULL;

  if (conn->state == NGTCP2_CS_POST_HANDSHAKE) {
    pkt_type = 0;
  } else if (conn->hs_pktns.tx_ckm) {
    pkt_type = NGTCP2_PKT_HANDSHAKE;
  } else {
    assert(conn->in_pktns.tx_ckm);
    pkt_type = NGTCP2_PKT_INITIAL;
  }

  nwrite = conn_write_single_frame_pkt(conn, dest, destlen, pkt_type, &fr, ts);

  if (nwrite > 0) {
    conn->state = NGTCP2_CS_CLOSING;
  }

  return nwrite;
}

ssize_t ngtcp2_conn_write_application_close(ngtcp2_conn *conn, uint8_t *dest,
                                            size_t destlen,
                                            uint16_t app_error_code,
                                            ngtcp2_tstamp ts) {
  ssize_t nwrite;
  ngtcp2_frame fr;

  conn->log.last_ts = ts;

  if (app_error_code == NGTCP2_STOPPING) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  if (conn_check_pkt_num_exhausted(conn)) {
    return NGTCP2_ERR_PKT_NUM_EXHAUSTED;
  }

  switch (conn->state) {
  case NGTCP2_CS_POST_HANDSHAKE:
    break;
  default:
    return NGTCP2_ERR_INVALID_STATE;
  }

  fr.type = NGTCP2_FRAME_APPLICATION_CLOSE;
  fr.application_close.app_error_code = app_error_code;
  fr.application_close.reasonlen = 0;
  fr.application_close.reason = NULL;

  nwrite =
      conn_write_single_frame_pkt(conn, dest, destlen, 0 /* Short */, &fr, ts);
  if (nwrite < 0) {
    return nwrite;
  }

  conn->state = NGTCP2_CS_CLOSING;

  return nwrite;
}

int ngtcp2_conn_is_in_closing_period(ngtcp2_conn *conn) {
  return conn->state == NGTCP2_CS_CLOSING;
}

int ngtcp2_conn_is_in_draining_period(ngtcp2_conn *conn) {
  return conn->state == NGTCP2_CS_DRAINING;
}

/*
 * handle_remote_stream_id_extension extends
 * |*punsent_max_remote_stream_id| if a condition allows it.
 */
static void
handle_remote_stream_id_extension(uint64_t *punsent_max_remote_stream_id,
                                  uint64_t *premote_stream_id_window_start,
                                  ngtcp2_idtr *idtr) {
  if (*punsent_max_remote_stream_id <= NGTCP2_MAX_VARINT - 4 &&
      *premote_stream_id_window_start < ngtcp2_idtr_first_gap(idtr)) {
    *punsent_max_remote_stream_id += 4;
    ++premote_stream_id_window_start;
  }
}

int ngtcp2_conn_close_stream(ngtcp2_conn *conn, ngtcp2_strm *strm,
                             uint16_t app_error_code) {
  int rv;

  if (!strm->app_error_code) {
    app_error_code = strm->app_error_code;
  }

  rv = ngtcp2_map_remove(&conn->strms, strm->me.key);
  if (rv != 0) {
    return rv;
  }

  rv = conn_call_stream_close(conn, strm, app_error_code);
  if (rv != 0) {
    return rv;
  }

  if (!conn_local_stream(conn, strm->stream_id)) {
    if (bidi_stream(strm->stream_id)) {
      handle_remote_stream_id_extension(
          &conn->unsent_max_remote_stream_id_bidi,
          &conn->remote_stream_id_bidi_window_start, &conn->remote_bidi_idtr);
    } else {
      handle_remote_stream_id_extension(
          &conn->unsent_max_remote_stream_id_uni,
          &conn->remote_stream_id_uni_window_start, &conn->remote_uni_idtr);
    }
  }

  if (strm->fc_pprev) {
    *strm->fc_pprev = strm->fc_next;
    if (strm->fc_next) {
      strm->fc_next->fc_pprev = strm->fc_pprev;
    }
  }

  ngtcp2_strm_free(strm);
  ngtcp2_mem_free(conn->mem, strm);

  return 0;
}

int ngtcp2_conn_close_stream_if_shut_rdwr(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                          uint16_t app_error_code) {
  if ((strm->flags & NGTCP2_STRM_FLAG_SHUT_RDWR) ==
          NGTCP2_STRM_FLAG_SHUT_RDWR &&
      ((strm->flags & NGTCP2_STRM_FLAG_RECV_RST) ||
       ngtcp2_rob_first_gap_offset(&strm->rob) == strm->last_rx_offset) &&
      ((strm->flags & NGTCP2_STRM_FLAG_SENT_RST) ||
       ngtcp2_gaptr_first_gap_offset(&strm->acked_tx_offset) ==
           strm->tx_offset)) {
    return ngtcp2_conn_close_stream(conn, strm, app_error_code);
  }
  return 0;
}

static int conn_shutdown_stream_write(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                      uint16_t app_error_code) {
  if (strm->flags & NGTCP2_STRM_FLAG_SENT_RST) {
    return 0;
  }

  /* Set this flag so that we don't accidentally send DATA to this
     stream. */
  strm->flags |= NGTCP2_STRM_FLAG_SHUT_WR | NGTCP2_STRM_FLAG_SENT_RST;
  strm->app_error_code = app_error_code;

  return conn_rst_stream(conn, strm, app_error_code);
}

static int conn_shutdown_stream_read(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                     uint16_t app_error_code) {
  if (strm->flags & NGTCP2_STRM_FLAG_STOP_SENDING) {
    return 0;
  }

  strm->flags |= NGTCP2_STRM_FLAG_STOP_SENDING;
  strm->app_error_code = app_error_code;

  return conn_stop_sending(conn, strm, app_error_code);
}

int ngtcp2_conn_shutdown_stream(ngtcp2_conn *conn, uint64_t stream_id,
                                uint16_t app_error_code) {
  int rv;
  ngtcp2_strm *strm;

  if (app_error_code == NGTCP2_STOPPING) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  if (strm == NULL) {
    return NGTCP2_ERR_STREAM_NOT_FOUND;
  }

  rv = conn_shutdown_stream_read(conn, strm, app_error_code);
  if (rv != 0) {
    return rv;
  }

  rv = conn_shutdown_stream_write(conn, strm, app_error_code);
  if (rv != 0) {
    return rv;
  }

  return 0;
}

int ngtcp2_conn_shutdown_stream_write(ngtcp2_conn *conn, uint64_t stream_id,
                                      uint16_t app_error_code) {
  ngtcp2_strm *strm;

  if (app_error_code == NGTCP2_STOPPING) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  if (strm == NULL) {
    return NGTCP2_ERR_STREAM_NOT_FOUND;
  }

  return conn_shutdown_stream_write(conn, strm, app_error_code);
}

int ngtcp2_conn_shutdown_stream_read(ngtcp2_conn *conn, uint64_t stream_id,
                                     uint16_t app_error_code) {
  ngtcp2_strm *strm;

  if (app_error_code == NGTCP2_STOPPING) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  if (strm == NULL) {
    return NGTCP2_ERR_STREAM_NOT_FOUND;
  }

  return conn_shutdown_stream_read(conn, strm, app_error_code);
}

int ngtcp2_conn_extend_max_stream_offset(ngtcp2_conn *conn, uint64_t stream_id,
                                         size_t datalen) {
  ngtcp2_strm *strm;

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  if (strm == NULL) {
    return NGTCP2_ERR_STREAM_NOT_FOUND;
  }

  conn_extend_max_stream_offset(conn, strm, datalen);

  return 0;
}

void ngtcp2_conn_extend_max_offset(ngtcp2_conn *conn, size_t datalen) {
  if (NGTCP2_MAX_VARINT < (uint64_t)datalen ||
      conn->unsent_max_rx_offset > NGTCP2_MAX_VARINT - datalen) {
    conn->unsent_max_rx_offset = NGTCP2_MAX_VARINT;
    return;
  }

  conn->unsent_max_rx_offset += datalen;
}

size_t ngtcp2_conn_get_bytes_in_flight(ngtcp2_conn *conn) {
  ngtcp2_pktns *in_pktns = &conn->in_pktns;
  ngtcp2_pktns *hs_pktns = &conn->hs_pktns;
  ngtcp2_pktns *pktns = &conn->pktns;

  return in_pktns->rtb.bytes_in_flight + hs_pktns->rtb.bytes_in_flight +
         pktns->rtb.bytes_in_flight;
}

const ngtcp2_cid *ngtcp2_conn_get_dcid(ngtcp2_conn *conn) {
  return &conn->dcid;
}

const ngtcp2_cid *ngtcp2_conn_get_scid(ngtcp2_conn *conn) {
  return &conn->scid;
}

uint32_t ngtcp2_conn_get_negotiated_version(ngtcp2_conn *conn) {
  return conn->version;
}

void ngtcp2_conn_early_data_rejected(ngtcp2_conn *conn) {
  ngtcp2_rtb_entry *ent, *next;
  ngtcp2_rtb *rtb = &conn->pktns.rtb;
  ngtcp2_ksl_it it;
  ngtcp2_frame_chain *frc;

  conn->flags |= NGTCP2_CONN_FLAG_EARLY_DATA_REJECTED;

  for (ent = conn->retry_early_rtb; ent;) {
    next = ent->next;
    frc = ent->frc;
    ent->frc = NULL;
    ngtcp2_rtb_entry_del(ent, conn->mem);

    assert(frc->next == NULL);
    frc->next = conn->frq;
    conn->frq = frc;

    ent = next;
  }

  for (it = ngtcp2_rtb_head(rtb); !ngtcp2_ksl_it_end(&it);) {
    ent = ngtcp2_ksl_it_get(&it);
    if ((ent->hd.flags & NGTCP2_PKT_FLAG_LONG_FORM) ||
        ent->hd.type != NGTCP2_PKT_0RTT_PROTECTED) {
      ngtcp2_ksl_it_next(&it);
      continue;
    }

    /* TODO Check return value */
    ngtcp2_rtb_remove(rtb, &it, ent);
    frc = ent->frc;
    ent->frc = NULL;
    ngtcp2_rtb_entry_del(ent, conn->mem);

    frc->next = conn->frq;
    conn->frq = frc;
  }
}

void ngtcp2_conn_update_rtt(ngtcp2_conn *conn, uint64_t rtt, uint64_t ack_delay,
                            int ack_only) {
  ngtcp2_rcvry_stat *rcs = &conn->rcs;

  rcs->min_rtt = ngtcp2_min(rcs->min_rtt, rtt);
  if (rtt - rcs->min_rtt > ack_delay) {
    rtt -= ack_delay;
    if (!ack_only) {
      rcs->max_ack_delay = ngtcp2_max(rcs->max_ack_delay, ack_delay);
    }
  }

  rcs->latest_rtt = rtt;

  if (rcs->smoothed_rtt < 1e-9) {
    rcs->smoothed_rtt = (double)rtt;
    rcs->rttvar = (double)rtt / 2;
  } else {
    double sample = fabs(rcs->smoothed_rtt - (double)rtt);
    rcs->rttvar = rcs->rttvar * 3 / 4 + sample / 4;
    rcs->smoothed_rtt = rcs->smoothed_rtt * 7 / 8 + (double)rtt / 8;
  }

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_RCV,
                  "latest_rtt=%" PRIu64 " min_rtt=%" PRIu64
                  " smoothed_rtt=%.3f rttvar=%.3f max_ack_delay=%" PRIu64,
                  rcs->latest_rtt / 1000000, rcs->min_rtt / 1000000,
                  rcs->smoothed_rtt / 1000000, rcs->rttvar / 1000000,
                  rcs->max_ack_delay / 1000000);
}

void ngtcp2_conn_get_rcvry_stat(ngtcp2_conn *conn, ngtcp2_rcvry_stat *rcs) {
  *rcs = conn->rcs;
}

void ngtcp2_conn_set_loss_detection_timer(ngtcp2_conn *conn) {
  ngtcp2_rcvry_stat *rcs = &conn->rcs;
  uint64_t timeout;
  ngtcp2_ksl_it it;
  ngtcp2_pktns *in_pktns = &conn->in_pktns;
  ngtcp2_pktns *hs_pktns = &conn->hs_pktns;
  ngtcp2_pktns *pktns = &conn->pktns;

  if (!ngtcp2_rtb_empty(&in_pktns->rtb) || !ngtcp2_rtb_empty(&hs_pktns->rtb) ||
      pktns->rtb.nearly_pkt) {
    if (rcs->smoothed_rtt < 1e-09) {
      timeout = 2 * NGTCP2_DEFAULT_INITIAL_RTT;
    } else {
      timeout = (uint64_t)(2 * rcs->smoothed_rtt);
    }

    timeout = ngtcp2_max(timeout + rcs->max_ack_delay, NGTCP2_MIN_TLP_TIMEOUT);
    timeout *= 1ull << rcs->handshake_count;

    rcs->loss_detection_timer = rcs->last_hs_tx_pkt_ts + timeout;

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_RCV,
                    "loss_detection_timer=%" PRIu64
                    " last_hs_tx_pkt_ts=%" PRIu64 " timeout=%" PRIu64,
                    rcs->loss_detection_timer, rcs->last_hs_tx_pkt_ts,
                    timeout / 1000000);
    return;
  }

  it = ngtcp2_rtb_head(&pktns->rtb);
  if (ngtcp2_ksl_it_end(&it) ||
      !(conn->flags & NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED)) {
    if (rcs->loss_detection_timer) {
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_RCV,
                      "loss detection timer canceled");
      rcs->loss_detection_timer = 0;
    }
    return;
  }

  /* We rarely gets assertion failure: assert(rcs->loss_time >=
     rcs->last_tx_pkt_ts).  So check the condition. */
  if (rcs->loss_time && rcs->loss_time < rcs->last_tx_pkt_ts) {
    ngtcp2_log_info(
        &conn->log, NGTCP2_LOG_EVENT_RCV,
        "assertion loss_time >= last_tx_pkt_ts failed: loss_time=%" PRIu64
        " last_tx_pkt_ts=%" PRIu64);
  }

  if (rcs->loss_time && rcs->loss_time >= rcs->last_tx_pkt_ts) {
    timeout = rcs->loss_time - rcs->last_tx_pkt_ts;
  } else {
    timeout = (uint64_t)(rcs->smoothed_rtt + 4 * rcs->rttvar +
                         (double)rcs->max_ack_delay);
    timeout = ngtcp2_max(timeout, NGTCP2_MIN_RTO_TIMEOUT);
    timeout *= 1ull << rcs->rto_count;

    if (rcs->tlp_count < NGTCP2_MAX_TLP_COUNT) {
      uint64_t tlp_timeout = ngtcp2_max(
          (uint64_t)(1.5 * rcs->smoothed_rtt + (double)rcs->max_ack_delay),
          NGTCP2_MIN_TLP_TIMEOUT);
      timeout = ngtcp2_min(timeout, tlp_timeout);
    }
  }

  rcs->loss_detection_timer = rcs->last_tx_pkt_ts + timeout;
}

int ngtcp2_conn_on_loss_detection_timer(ngtcp2_conn *conn, ngtcp2_tstamp ts) {
  ngtcp2_rcvry_stat *rcs = &conn->rcs;
  int rv;
  ngtcp2_pktns *in_pktns = &conn->in_pktns;
  ngtcp2_pktns *hs_pktns = &conn->hs_pktns;
  ngtcp2_pktns *pktns = &conn->pktns;

  conn->log.last_ts = ts;

  if (!rcs->loss_detection_timer) {
    return 0;
  }

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_RCV,
                  "loss detection timer fired");

  if (!ngtcp2_rtb_empty(&in_pktns->rtb) || !ngtcp2_rtb_empty(&hs_pktns->rtb) ||
      pktns->rtb.nearly_pkt) {
    rv = ngtcp2_rtb_mark_pkt_lost(&in_pktns->rtb);
    if (rv != 0) {
      return rv;
    }
    rv = ngtcp2_rtb_mark_pkt_lost(&hs_pktns->rtb);
    if (rv != 0) {
      return rv;
    }
    rv = ngtcp2_rtb_mark_0rtt_pkt_lost(&pktns->rtb);
    if (rv != 0) {
      return rv;
    }
    ++rcs->handshake_count;
  } else if (rcs->loss_time) {
    rv = ngtcp2_rtb_detect_lost_pkt(&pktns->rtb, rcs,
                                    (uint64_t)conn->largest_ack,
                                    pktns->last_tx_pkt_num, ts);
    if (rv != 0) {
      return rv;
    }
  } else if (rcs->tlp_count < NGTCP2_MAX_TLP_COUNT) {
    rcs->probe_pkt_left = 1;
    ++rcs->tlp_count;
  } else {
    rcs->probe_pkt_left = 2;
    if (rcs->rto_count == 0) {
      rcs->largest_sent_before_rto = pktns->last_tx_pkt_num;
    }
    ++rcs->rto_count;
  }

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_RCV,
                  "handshake_count=%zu tlp_count=%zu rto_count=%zu",
                  rcs->handshake_count, rcs->tlp_count, rcs->rto_count);

  ngtcp2_conn_set_loss_detection_timer(conn);

  return 0;
}

int ngtcp2_conn_submit_crypto_data(ngtcp2_conn *conn, const uint8_t *data,
                                   const size_t datalen) {
  uint8_t pkt_type;
  ngtcp2_ringbuf *rb = &conn->tx_crypto_data;
  ngtcp2_crypto_data *cdata;
  size_t len;
  ngtcp2_pktns *in_pktns = &conn->in_pktns;
  ngtcp2_pktns *hs_pktns = &conn->hs_pktns;
  ngtcp2_pktns *pktns = &conn->pktns;

  if (datalen == 0) {
    return 0;
  }

  assert(!ngtcp2_ringbuf_full(rb));

  if (pktns->tx_ckm) {
    pkt_type = 0; /* Short packet */
  } else if (hs_pktns->tx_ckm) {
    pkt_type = NGTCP2_PKT_HANDSHAKE;
  } else if (!conn->server && conn->early_ckm && datalen == 4) {
    /* TODO datalen == 4 is quite hackish.  When client sends Initial
       along with 0RTT, and server sends back HRR, without this
       condition client sends CH in 0RTT packet.  For TLSv1.3, the
       only data submitted by this function for 0RTT is EOED which is
       just 4 bytes long.  If the given data is not 4 bytes, assume
       that it is CH in response to HRR. */
    pkt_type = NGTCP2_PKT_0RTT_PROTECTED;
  } else {
    assert(in_pktns->tx_ckm);
    pkt_type = NGTCP2_PKT_INITIAL;
  }

  len = ngtcp2_ringbuf_len(rb);
  if (len) {
    cdata = ngtcp2_ringbuf_get(rb, len - 1);
    if (cdata->pkt_type == pkt_type && cdata->buf.last == data) {
      cdata->buf.last += datalen;
      return 0;
    }
  }

  cdata = ngtcp2_ringbuf_push_back(rb);
  ngtcp2_buf_init(&cdata->buf, (uint8_t *)data, datalen);
  cdata->buf.last += datalen;
  cdata->pkt_type = pkt_type;

  return 0;
}
