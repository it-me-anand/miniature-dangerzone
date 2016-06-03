#include "sched.h"
#include "msg.h"
#include "xtimer.h"
#include "net/gnrc/netreg.h"
#include "net/gnrc/netapi.h"
#include "net/gnrc/pktbuf.h"
#include "ccnl-pkt-ndntlv.h"

#include "cluster.h"
#include "ccnlriot.h"
#include "log.h"

/* public variables */
struct ccnl_prefix_s *ccnl_helper_all_pfx;

/* buffers for interests and content */
static unsigned char _int_buf[CCNLRIOT_BUF_SIZE];
static unsigned char _cont_buf[CCNLRIOT_BUF_SIZE];
static unsigned char _out[CCNL_MAX_PACKET_SIZE];
static char _prefix_str[CCNLRIOT_PFX_LEN];

/* prototypes from CCN-lite */
void free_packet(struct ccnl_pkt_s *pkt);
struct ccnl_prefix_s *ccnl_prefix_dup(struct ccnl_prefix_s *prefix);

/* internal variables */
static xtimer_t _sleep_timer = { .target = 0, .long_target = 0 };
static msg_t _sleep_msg = { .type = CLUSTER_MSG_BACKTOSLEEP };

void ccnl_helper_init(void)
{
    unsigned cn = 0;
    char all_pfx[] = CCNLRIOT_ALL_PREFIX;
    ccnl_helper_all_pfx = ccnl_URItoPrefix(all_pfx, CCNL_SUITE_NDNTLV, NULL, &cn);
}

/**
 * @brief create a content struct
 *
 * @param[in] prefix    name of the content
 * @param[in] value     content itself (will get filled into a
 *                      @ref cluster_content_t struct
 * @param[in] cache     if true, sends data via loopback to self for caching
 *
 * @returns pointer to new content chunk
 * */
struct ccnl_content_s *ccnl_helper_create_cont(struct ccnl_prefix_s *prefix,
                                               unsigned char *value, ssize_t
                                               len, bool cache, bool send)
{
    if (len > (CLUSTER_CONT_LEN + 1)) {
        LOG_ERROR("ccnl_helper: Too long content. This is not acceptable!!!\n");
        return NULL;
    }
    int offs = CCNL_MAX_PACKET_SIZE;

    cluster_content_t my_cont;
    memset(&my_cont.value, 0, CLUSTER_CONT_LEN + 1);
    memcpy(my_cont.value, value, len);
    my_cont.num = -1;
    len = sizeof(cluster_content_t);

    ssize_t arg_len = ccnl_ndntlv_prependContent(prefix, (unsigned char*) &my_cont, len, NULL, NULL, &offs, _out);

    unsigned char *olddata;
    unsigned char *data = olddata = _out + offs;
    unsigned typ;

    if (ccnl_ndntlv_dehead(&data, &arg_len, (int*) &typ, &len) ||
        typ != NDN_TLV_Data) {
        return NULL;
    }

    struct ccnl_content_s *c = 0;
    struct ccnl_pkt_s *pk = ccnl_ndntlv_bytes2pkt(typ, olddata, &data, &arg_len);
    if (pk == NULL) {
        LOG_ERROR("ccnl_helper: something went terribly wrong!\n");
        return NULL;
    }
    c = ccnl_content_new(&ccnl_relay, &pk);
    if (send) {
        ccnl_broadcast(&ccnl_relay, c->pkt);
    }
    if (cache) {
        /* XXX: always use first (and only IF) */
        uint8_t hwaddr[CCNLRIOT_ADDRLEN];
#if (CCNLRIOT_ADDRLEN == 8)
        gnrc_netapi_get(CCNLRIOT_NETIF, NETOPT_ADDRESS_LONG, 0, hwaddr, sizeof(hwaddr));
#else
        gnrc_netapi_get(CCNLRIOT_NETIF, NETOPT_ADDRESS, 0, hwaddr, sizeof(hwaddr));
#endif
        sockunion dest;
        dest.sa.sa_family = AF_PACKET;
        memcpy(dest.linklayer.sll_addr, hwaddr, CCNLRIOT_ADDRLEN);
        dest.linklayer.sll_halen = CCNLRIOT_ADDRLEN;
        dest.linklayer.sll_protocol = htons(ETHERTYPE_NDN);
        extern void ccnl_ll_TX(struct ccnl_relay_s *ccnl, struct ccnl_if_s *ifc, sockunion *dest, struct ccnl_buf_s *buf);
        ccnl_ll_TX(&ccnl_relay, &ccnl_relay.ifs[0], &dest, c->pkt->buf);
        free_packet(c->pkt);
        ccnl_free(c);
    }

    return c;
}

/**
 * @brief creates an interestest for a given name
 */
struct ccnl_interest_s *ccnl_helper_create_int(struct ccnl_prefix_s *prefix)
{
    int nonce = random_uint32();
    LOG_DEBUG("ccnl_helper: nonce: %X\n", nonce);

    extern int ndntlv_mkInterest(struct ccnl_prefix_s *name, int *nonce, unsigned char *out, int outlen);
    int len = ndntlv_mkInterest(prefix, &nonce, _int_buf, CCNLRIOT_BUF_SIZE);

    unsigned char *start = _int_buf;
    unsigned char *data = _int_buf;
    struct ccnl_pkt_s *pkt;

    int typ;
    int int_len;

    if (ccnl_ndntlv_dehead(&data, &len, (int*) &typ, &int_len) || (int) int_len > len) {
        LOG_WARNING("ccnl_helper: invalid packet format\n");
        return NULL;
    }
    pkt = ccnl_ndntlv_bytes2pkt(NDN_TLV_Interest, start, &data, &len);

    struct ccnl_face_s *loopback_face = ccnl_get_face_or_create(&ccnl_relay, -1, NULL, 0);
    return ccnl_interest_new(&ccnl_relay, loopback_face, &pkt);
}

#if CLUSTER_DEPUTY
/**
 * @brief send an acknowledgement
 **/
static void _send_ack(struct ccnl_relay_s *relay, struct ccnl_face_s *from,
                      struct ccnl_prefix_s *pfx, int num)
{
    struct ccnl_content_s *c =
        ccnl_helper_create_cont(pfx, (unsigned char*)
                                CCNLRIOT_CONT_ACK,
                                strlen(CCNLRIOT_CONT_ACK) + 1, false, false);
    if (c == NULL) {
        return;
    }
    cluster_content_t *cc = (cluster_content_t*) c->pkt->content;
    cc->num = num;
    ccnl_face_enqueue(relay, from, ccnl_buf_new(c->pkt->buf->data,
                                                c->pkt->buf->datalen));

    free_packet(c->pkt);
    ccnl_free(c);
}
#endif

static bool _cont_is_dup(struct ccnl_pkt_s *pkt)
{
    assert(pkt->content != NULL);
    /* save old value */
    cluster_content_t *cc = (cluster_content_t*) pkt->content;
    int old = cc->num;
    /* set to -1 for comparison */
    cc->num = -1;

    for (struct ccnl_content_s *c = ccnl_relay.contents; c; c = c->next) {
        cluster_content_t *ccc = (cluster_content_t*) c->pkt->content;
        int cold = ccc->num;
        ccc->num = -1;
        if ((c->pkt->buf) && (pkt->buf) &&
            (c->pkt->buf->datalen==pkt->buf->datalen) &&
            !memcmp(c->pkt->buf->data,pkt->buf->data,c->pkt->buf->datalen)) {
            cc->num = old;
            ccc->num = cold;
            return true; /* content is dup, do nothing */
        }
        ccc->num = cold;
    }
    cc->num = old;
    return false;
}

#if CLUSTER_DEPUTY
void ccnl_helper_clear_pit_for_own(void)
{
    /* check if we have a PIT entry for our own content and remove it */
    LOG_DEBUG("ccnl_helper: clear PIT entries for own content\n");
    gnrc_netapi_set(ccnl_pid, NETOPT_CCN, CCNL_CTX_CLEAR_PIT_OWN, &ccnl_relay, sizeof(ccnl_relay));
}

/**
 * @brief remove the PIT entry for the given chunk number for *
 */
static void _remove_pit(struct ccnl_relay_s *relay, int num)
{
    struct ccnl_interest_s *i = relay->pit;
    while (i) {
        if ((ccnl_prefix_cmp(ccnl_helper_all_pfx, NULL, i->pkt->pfx, CMP_MATCH) >= 1) &&
            (*(i->pkt->pfx->chunknum) == num)) {
            LOG_INFO("ccnl_helper: remove matching PIT entry\n");
            ccnl_interest_remove(relay, i);
            return;
        }
        i = i->next;
    }
}
#endif

/**
 * @brief local callback to handle incoming content chunks
 *
 * @note  Gets called from CCNL thread context
 *
 * @returns 1   if chunk is handled and no further processing should happen
 * @returns 0   otherwise
 **/
int ccnlriot_consumer(struct ccnl_relay_s *relay, struct ccnl_face_s *from,
                      struct ccnl_pkt_s *pkt)
{
    (void) from;
    (void) relay;
    LOG_DEBUG("%" PRIu32 " ccnl_helper: local consumer for prefix: %s\n", xtimer_now(),
              ccnl_prefix_to_path_detailed(_prefix_str, pkt->pfx, 1, 0, 0));
    memset(_prefix_str, 0, CCNLRIOT_PFX_LEN);

#if CLUSTER_DEPUTY
    /* XXX: might be unnecessary du to mutex now */
    /* if we're currently transferring our cache to the new deputy, we do not touch the content store */
    if (cluster_state == CLUSTER_STATE_HANDOVER) {
        LOG_DEBUG("ccnl_helper: we're in handover state, cannot touch content store right now\n");
        free_packet(pkt);
        return 1;
    }

    /* check if prefix is for ALL and contains an ACK */
    if ((ccnl_prefix_cmp(ccnl_helper_all_pfx, NULL, pkt->pfx, CMP_MATCH) >= 1) &&
        (strncmp((char*) pkt->content, CCNLRIOT_CONT_ACK, strlen(CCNLRIOT_CONT_ACK)) == 0)) {
        cluster_content_t *cc = (cluster_content_t*) pkt->content;
        LOG_DEBUG("ccnl_helper: content number is %i\n", cc->num);
        if (cc->num >= 0) {
            _remove_pit(relay, cc->num);
        }

        LOG_DEBUG("ccnl_helper: received ACK, flag the content\n");
        msg_t m = { .type = CLUSTER_MSG_RECEIVED_ACK };
        msg_try_send(&m, cluster_pid);
        free_packet(pkt);
        return 1;
    }
#endif

    /* TODO: implement not being interested in all content */
    struct ccnl_interest_s *i = NULL;
    /* if we don't have this content, we check if we have a matching PIT entry */
    bool is_dup = _cont_is_dup(pkt);
    if (!is_dup) {
        i = relay->pit;
        while (i) {
            if (ccnl_prefix_cmp(i->pkt->pfx, NULL, pkt->pfx, CMP_EXACT) == 0) {
                LOG_DEBUG("ccnl_helper: found matching interest, nothing to do\n");
                if (from->faceid == i->from->faceid) {
                    LOG_DEBUG("ccnl_helper: not bouncing chunk\n");
                    struct ccnl_face_s *loopback_face = ccnl_get_face_or_create(&ccnl_relay, -1, NULL, 0);
                    i->pending->face = loopback_face;
                }
                break;
            }
            i = i->next;
        }
    }

    /* we don't have a PIT entry for this */
    if (!i) {
        /* if we're not deputy (or becoming it), we assume that this is our
         * own content */
        if (cluster_state != CLUSTER_STATE_DEPUTY) {
            /* cache it */
            if (relay->max_cache_entries != 0) {
                LOG_DEBUG("ccnl_helper: adding content to cache\n");
                struct ccnl_pkt_s *tmp = pkt;
                struct ccnl_content_s *c = ccnl_content_new(&ccnl_relay, &tmp);
                if (!c) {
                    LOG_ERROR("ccnl_helper: we're doomed, WE'RE ALL DOOMED\n");
                    return 0;
                }
                if (ccnl_content_add2cache(relay, c) == NULL) {
                    LOG_WARNING("ccnl_helper:  adding to cache failed, discard packet\n");
                    ccnl_free(c);
                    free_packet(pkt);
                }
                /* inform potential waiters */
                msg_t m = { .type = CLUSTER_MSG_RECEIVED };
                msg_try_send(&m, cluster_pid);
            }
            return 1;
        }
        else {
#if CLUSTER_DEPUTY
            /* create an interest if we're waiting for *, because otherwise
             * our PIT entry won't match */
            if (pkt->contlen == sizeof(cluster_content_t)) {
                LOG_DEBUG("ccnl_helper: seems to be the right content\n");
                cluster_content_t *cc = (cluster_content_t*) pkt->content;
                LOG_INFO("ccnl_helper: content number is %i\n", cc->num);
                /* if we receive content, it's either because
                 *  - we asked for * -> num >= 0
                 *  - through loopback for content we generated ourselves
                 *  - we asked for it, because the generating node sent us an interest
                 */
                if (cc->num >= 0) {
                    _remove_pit(relay, cc->num);
                }
                if (is_dup) {
                    LOG_DEBUG("ccnl_helper: we already have this content, do nothing\n");
                }
                else {
                    ccnl_helper_create_int(pkt->pfx);
                }

                if (cc->num >= 0) {
                    /* in case we're waiting for * chunks, try to send a message */
                    LOG_DEBUG("ccnl_helper: inform waiters\n");
                    msg_t m = { .type = CLUSTER_MSG_RECEIVED };
                    msg_try_send(&m, cluster_pid);
                }

                cc->num = -1;
            }
            else {
                LOG_WARNING("ccnl_helper: content length is %i, was expecting %i\n", pkt->buf->datalen, sizeof(cluster_content_t));
            }
#else
            ccnl_helper_create_int(pkt->pfx);
#endif
        }
    }
    return 0;
}

/**
 * @brief local callback to handle incoming interests
 *
 * @note  Gets called from CCNL thread context
 *
 * @returns 1   if interest is handled and no further processing should happen
 * @returns 0   otherwise
 **/
int ccnlriot_producer(struct ccnl_relay_s *relay, struct ccnl_face_s *from,
                      struct ccnl_pkt_s *pkt)
{
    (void) from;
    int res = 0;

    LOG_DEBUG("%" PRIu32 " ccnl_helper: local producer for prefix: %s\n",
              xtimer_now(), ccnl_prefix_to_path_detailed(_prefix_str, pkt->pfx, 1, 0, 0));
    memset(_prefix_str, 0, CCNLRIOT_PFX_LEN);

    /* check if we have a PIT entry for this interest and a corresponding entry
     * in the content store */
    struct ccnl_interest_s *i = relay->pit;
    struct ccnl_content_s *c = relay->contents;
    while (i) {
        if (ccnl_prefix_cmp(i->pkt->pfx, NULL, pkt->pfx, CMP_EXACT) == 0) {
            LOG_DEBUG("ccnl_helper: found matching interest, let's check if we "
                      "have the content, too\n");

            break;
        }
        i = i->next;
    }
    if (i) {
        while (c) {
            if (ccnl_prefix_cmp(c->pkt->pfx, NULL, pkt->pfx, CMP_EXACT) == 0) {
                LOG_DEBUG("ccnl_helper: indeed, we have the content, too -> we "
                          "will serve it, remove PIT entry. (Pending interests "
                          "for own data: %u)\n", (unsigned) cluster_prevent_sleep);
                /* inform ourselves that the interest was bounced back */
                msg_t m = { .type = CLUSTER_MSG_RECEIVED_INT };
                msg_try_send(&m, cluster_pid);

                ccnl_interest_remove(relay, i);
                cluster_prevent_sleep--;
                /* go to sleep if we're not currently in handover mode */
                if (cluster_state != CLUSTER_STATE_HANDOVER) {
                    LOG_DEBUG("ccnl_helper: going back to sleep in %u microseconds (%i)\n",
                              CLUSTER_STAY_AWAKE_PERIOD, (int) cluster_pid);
                    xtimer_set_msg(&_sleep_timer, CLUSTER_STAY_AWAKE_PERIOD, &_sleep_msg, cluster_pid);
                }
                return 0;
            }
            c = c->next;
        }
        if (!c) {
            LOG_DEBUG("ccnl_helper: nope, we cannot serve this\n");
        }
    }

    /* avoid interest flooding */
    if (cluster_state == CLUSTER_STATE_INACTIVE) {
        LOG_DEBUG("ccnl_helper: pretend to be sleeping\n");
        free_packet(pkt);
        return 1;
    }

#if CLUSTER_DEPUTY
    /* check if this is a handover request */
    if (ccnl_prefix_cmp(ccnl_helper_all_pfx, NULL, pkt->pfx, CMP_MATCH) >= 1) {
        if ((cluster_state == CLUSTER_STATE_DEPUTY) || (cluster_state == CLUSTER_STATE_HANDOVER)) {
            /* make sure interest contains a chunknumber */
            if ((pkt->pfx->chunknum == NULL) || (*(pkt->pfx->chunknum) == -1)) {
                LOG_WARNING("ccnl_helper: no chunknumber? what a fool!\n");
                free_packet(pkt);
                res = 1;
                goto out;
            }

            /* change state (will be done for each requested chunk, we don't care) */
            /* TODO: implement timeout to prevent that we stay in HANDOVER forever */
            LOG_INFO("\n\nccnl_helper: change to state HANDOVER\n\n");
            cluster_state = CLUSTER_STATE_HANDOVER;

            /* find corresponding chunk in store */
            struct ccnl_content_s *cit = relay->contents;
            LOG_DEBUG("ccnl_helper: received %s request for chunk number %i\n",
                      CCNLRIOT_ALL_PREFIX, *(pkt->pfx->chunknum));
            int i;
            for (i = 0; (i < *(pkt->pfx->chunknum)) && (i < CCNLRIOT_CACHE_SIZE) && (cit != NULL); i++) {
                cit = cit->next;
            }

            /* if we reached the end of the store, we send an ACK */
            if ((i >= CCNLRIOT_CACHE_SIZE) || (cit == NULL)) {
                LOG_INFO("ccnl_helper: reached end of content store, send ack\n");
                _send_ack(relay, from, ccnl_helper_all_pfx, i);

                /* we can go back to sleep now */
                /* TODO: delay this */
                msg_t m = { .type = CLUSTER_MSG_INACTIVE };
                msg_send(&m, cluster_pid);
                free_packet(pkt);
                res = 1;
                goto out;
            }

            /* otherwise we rewrite the name before we pass it down */

            /* save old prefix, so we can free its memory */
            struct ccnl_prefix_s *old = pkt->pfx;

            /* now create a new one */
            struct ccnl_prefix_s *new = ccnl_prefix_dup(cit->pkt->pfx);
            pkt->pfx = new;

            LOG_DEBUG("ccnl_helper: setting content num to %i for %p\n", i, (void*) cit->pkt->content);
            cluster_content_t *cc = (cluster_content_t*) cit->pkt->content;
            cc->num = i;

            LOG_DEBUG("%" PRIu32 " ccnl_helper: publish content for prefix: %s\n", xtimer_now(),
                      ccnl_prefix_to_path_detailed(_prefix_str, pkt->pfx, 1, 0, 0));
            memset(_prefix_str, 0, CCNLRIOT_PFX_LEN);

            /* set content number */
            if (cit == NULL) {
                LOG_ERROR("ccnl_helper: The world is a bad place, this should have never happened\n");
                res = 1;
                goto out;
            }

            /* free the old prefix */
            free_prefix(old);
            res = 0;
            goto out;
        }
        free_packet(pkt);
        res = 1;
        goto out;
    }

    if (cluster_state == CLUSTER_STATE_HANDOVER) {
        LOG_DEBUG("ccnl_helper: in handover we handle nothing else\n");
        free_packet(pkt);
        res = 1;
        goto out;
    }

out:
#endif /* CLUSTER_DEPUTY */
    return res;
}

/**
 * @brief Caching strategy: oldest representative
 * Always cache at least one value per name and replace always the oldest value
 * for this name if cache is full. If no older value for this name exist,
 * replace the oldest value for any name that has different version in the
 * cache. If no such entry exists in the cache, replace the oldest value for
 * any name.
 */
int cs_oldest_representative(struct ccnl_relay_s *relay, struct ccnl_content_s *c)
{
    struct ccnl_content_s *c2, *oldest = NULL;
    long oldest_ts = 0;
    for (c2 = relay->contents; c2; c2 = c2->next) {
        if (!(c2->flags & CCNL_CONTENT_FLAGS_STATIC)) {
            if (ccnl_prefix_cmp(c->pkt->pfx, NULL, c->pkt->pfx, CMP_MATCH) >= 3) {
                if (c->pkt->pfx->compcnt < 4) {
                    LOG_WARNING("ccnl_helper: invalid prefix found in cache, skipping\n");
                    continue;
                }
                long c2_ts = strtol((char*) c2->pkt->pfx->comp[3], NULL, 16);
                if ((oldest_ts == 0) || (c2_ts < oldest_ts)) {
                    oldest_ts = c2_ts;
                    oldest = c2;
                }
            }
        }
    }
    long c_ts = strtol((char*) c->pkt->pfx->comp[3], NULL, 16);
    if (oldest_ts > c_ts) {
        LOG_INFO("New value is older than oldest value for this ID, skipping\n");
        return 1;
    }
    if (oldest) {
#ifdef CCNL_RIOT
        mutex_unlock(&(relay->cache_write_lock));
#endif
        LOG_INFO("ccnl_helper: remove oldest entry for this prefix from cache\n");
        ccnl_content_remove(relay, oldest);
#ifdef CCNL_RIOT
        mutex_lock(&(relay->cache_write_lock));
#endif
        return 1;
    }
    return 0;
}


static xtimer_t _wait_timer = { .target = 0, .long_target = 0 };
static msg_t _timeout_msg;
static int _wait_for_chunk(void *buf, size_t buf_len, bool wait_for_int)
{
    int res = (-1);

    int32_t remaining = (CCNL_MAX_INTEREST_RETRANSMIT ? CCNL_MAX_INTEREST_RETRANSMIT : 1) * CCNLRIOT_INT_TIMEOUT;
    uint32_t now = xtimer_now();

    while (1) { /* wait for a content pkt (ignore interests) */
        LOG_DEBUG("ccnl_helper:  waiting for packet\n");

        /* check if there's time left to wait for the content */
        _timeout_msg.type = CCNL_MSG_TIMEOUT;
        remaining -= (xtimer_now() - now);
        if (remaining < 0) {
            LOG_WARNING("ccnl_helper: timeout waiting for valid message\n");
            res = -ETIMEDOUT;
            break;
        }
        LOG_DEBUG("remaining time: %u\n", (unsigned) remaining);
        xtimer_set_msg(&_wait_timer, remaining, &_timeout_msg, sched_active_pid);

        msg_t m;
        msg_receive(&m);

        /* we received something from local_consumer */
        if (m.type == CLUSTER_MSG_RECEIVED) {
            if (wait_for_int) {
                LOG_DEBUG("ccnl_helper: that was an chunk - we're currently not interested in\n");
                continue;
            }
            LOG_DEBUG("ccnl_helper: received something, that's good enough for me\n");
            res = 1;
            xtimer_remove(&_wait_timer);
            break;
        }
        else if (m.type == CLUSTER_MSG_RECEIVED_ACK) {
            if (wait_for_int) {
                LOG_DEBUG("ccnl_helper: that was an chunk - we're currently not interested in\n");
                continue;
            }
            LOG_INFO("ccnl_helper: received ack\n");
            memcpy(buf, CCNLRIOT_CONT_ACK, sizeof(CCNLRIOT_CONT_ACK));
            res = sizeof(CCNLRIOT_CONT_ACK);
            xtimer_remove(&_wait_timer);
            break;
        }
        else if (m.type == CLUSTER_MSG_RECEIVED_INT) {
            if (wait_for_int) {
                LOG_DEBUG("ccnl_helper: received an interest - we're waiting for that\n");
                res = 1;
                xtimer_remove(&_wait_timer);
                break;
            }
            else {
                LOG_DEBUG("ccnl_helper: we were not waiting for an interest\n");
                continue;
            }
        }
        /* we received a chunk from CCN-Lite */
        else if (m.type == GNRC_NETAPI_MSG_TYPE_RCV) {
            gnrc_pktsnip_t *pkt = (gnrc_pktsnip_t *)m.content.ptr;
            LOG_DEBUG("ccnl_helper: Type is: %i\n", pkt->type);
            if (pkt->type == GNRC_NETTYPE_CCN_CHUNK) {
                char *c = (char*) pkt->data;
                LOG_INFO("ccnl_helper: Content is: %s\n", c);
                size_t len = (pkt->size > buf_len) ? buf_len : pkt->size;
                memcpy(buf, pkt->data, len);
                res = (int) len;
                gnrc_pktbuf_release(pkt);
            }
            else {
                LOG_WARNING("ccnl_helper: Unkown content\n");
                gnrc_pktbuf_release(pkt);
                continue;
            }
            xtimer_remove(&_wait_timer);
            break;
        }
        else if (m.type == CCNL_MSG_TIMEOUT) {
            res = -ETIMEDOUT;
            break;
        }
        else if (m.type == CLUSTER_MSG_NEWDATA) {
            LOG_DEBUG("ccnl_helper: received newdata msg while waiting for content, postpone it\n");
            xtimer_set_msg(&cluster_data_timer, SEC_IN_USEC, &cluster_data_msg, cluster_pid);
        }
        else if (m.type == CLUSTER_MSG_SECOND) {
            //LOG_DEBUG("ccnl_helper: SECOND: %u\n", (unsigned) cluster_period_counter);
            xtimer_remove(&cluster_timer);
            if (cluster_period_counter == 1) {
                LOG_WARNING("ccnl_helper: we're late!\n");
            }
            else {
                cluster_period_counter--;
            }
            xtimer_set_msg(&cluster_timer, SEC_IN_USEC, &cluster_wakeup_msg, cluster_pid);
        }
        else {
            LOG_DEBUG("ccnl_helper: Unknown message received, ignore it\n");
        }
    }

    return res;
}

/**
 * @brief build and send an interest packet
 *
 * @param[in] wait_for_int if true, waits for interest instead of chunk
 *
 * @returns CCNLRIOT_RECEIVED_CHUNK     if a chunk was received
 * @returns CCNLRIOT_LAST_CN            if an ACK was received
 * @returns CCNLRIOT_TIMEOUT            if nothing was received within the
 *                                      given timeframe
 **/
int ccnl_helper_int(struct ccnl_prefix_s *prefix, unsigned *chunknum, bool wait_for_int)
{
    LOG_DEBUG("ccnl_helper: ccnl_helper_int\n");

    /* clear interest and content buffer */
    memset(_int_buf, '\0', CCNLRIOT_BUF_SIZE);
    memset(_cont_buf, '\0', CCNLRIOT_BUF_SIZE);

    unsigned success = CCNLRIOT_TIMEOUT;

    gnrc_netreg_entry_t _ne;

    /* actual sending of the content */
    for (int cnt = 0; cnt < CCNLRIOT_INT_RETRIES; cnt++) {
        LOG_INFO("ccnl_helper: sending interest #%u for %s (%i)\n", (unsigned) cnt,
                 ccnl_prefix_to_path_detailed(_prefix_str, prefix, 1, 0, 0), (int) *chunknum);
        /* register for content chunks */
        _ne.demux_ctx =  GNRC_NETREG_DEMUX_CTX_ALL;
        _ne.pid = sched_active_pid;
        gnrc_netreg_register(GNRC_NETTYPE_CCN_CHUNK, &_ne);

        *(prefix->chunknum) = *chunknum;
        ccnl_interest_t i = { .prefix = prefix, .buf = _int_buf, .buflen = CCNLRIOT_BUF_SIZE };
        gnrc_pktsnip_t *pkt = gnrc_pktbuf_add(NULL, &i, sizeof(i), GNRC_NETTYPE_CCN);
        gnrc_netapi_send(ccnl_pid, pkt);
        if (_wait_for_chunk(_cont_buf, CCNLRIOT_BUF_SIZE, wait_for_int) > 0) {
            gnrc_netreg_unregister(GNRC_NETTYPE_CCN_CHUNK, &_ne);
            LOG_DEBUG("ccnl_helper: Content received: %s\n", _cont_buf);
            success = CCNLRIOT_RECEIVED_CHUNK;
            break;
        }
        gnrc_netreg_unregister(GNRC_NETTYPE_CCN_CHUNK, &_ne);
    }

    if (success == CCNLRIOT_RECEIVED_CHUNK) {
        if (strncmp((char*) _cont_buf, CCNLRIOT_CONT_ACK, strlen(CCNLRIOT_CONT_ACK)) == 0) {
            LOG_DEBUG("ccnl_helper: received ACK, signaling end of takeover\n");
            success = CCNLRIOT_LAST_CN;
        }
        LOG_WARNING("\nccnl_helper: +++ SUCCESS +++\n");
    }
    else {
        LOG_WARNING("\nccnl_helper: !!! TIMEOUT while waiting for chunk number %i!!!\n",
                    (int) *chunknum);
    }

    return success;
}
