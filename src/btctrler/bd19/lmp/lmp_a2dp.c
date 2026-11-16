/* Classic/lmp_a2dp.c (included by lmp.c): the A2DP (sbc/aac) media path:
 * marking L2CAP packets of the media channel, fetch/free, buffer level and
 * clearing up to a sequence number (synchronised to a TWS slave). */
#undef LMP_FILE
#define LMP_FILE "lmp_a2dp.c"

int os_sem_create(OS_SEM *sem, int cnt);
int os_sem_set(OS_SEM *sem, u16 cnt);
int os_sem_pend(OS_SEM *sem, int timeout);
#define RX_BITS(rx) (*((u8 *)(rx) + 2))
#define RX_CID(rx) ((rx)->data[3] << 8 | (rx)->data[2])
#define RX_SEQ(rx) ((rx)->data[6] << 8 | (rx)->data[7])
#define PACKET_RX(p) ((struct bredr_rx_bulk *)((u8 *)(p) - offsetof(struct bredr_rx_bulk, data) - 4))

int LMP_CODE lmp_private_abandon_sbc_data(struct lmp_connection *conn, struct list_head *rx_head, u16 seqn_number)
{
    struct list_head *node, *next;
    struct bredr_rx_bulk *rx;
    int ret = -1, seq;
    u8 bits, llid, del = 0;

    if (!(config_btctler_modules & 1))
        return 0;
    if (rx_head->next == rx_head)
        return -1;
    for (node = rx_head->next; node != rx_head; node = next) {
        next = node->next;
        rx = RX_OF(node);
        bits = RX_BITS(rx);
        llid = bits & 7;
        if (llid == 2) {
            del = 0;
            if ((bits & 0xc0) != 0x80 && !(bits & 8) && conn->sbc_channel == RX_CID(rx)) {
                seq = RX_SEQ(rx);
                if (seqn_number && (s16)(seq - seqn_number) >= 0)
                    return seq;
                __list_del_entry(node);
                bredr_rx_bulk_free(rx);
                ret = seq;
                del = 1;
            }
        } else if (del && llid == 1) {
            __list_del_entry(node);
            bredr_rx_bulk_free(rx);
        }
    }
    return ret;
}

void LMP_CODE lmp_private_set_a2dp_channel(u8 *addr, u16 channel)
{
    struct lmp_connection *conn;

    local_irq_disable();
    conn = lmp_conn_for_address(addr);
    if (conn)
        conn->sbc_channel = channel;
    local_irq_enable();
}

void *LMP_CODE lmp_private_open_sbc_channel(u8 *addr, u16 channel, u8 codec_type)
{
    struct lmp_connection *conn, *p;
    struct list_head *node;
    struct bredr_rx_bulk *rx;
    u8 bits;

    if (!(config_btctler_modules & 1))
        return NULL;
    conn = lmp_conn_for_address(addr);
    prev_seqn_number = 0;
    conn->prev_a2dp_seqn = 0;       /* sic: before the NULL test below */
    local_irq_disable();
    conn = lmp_conn_for_address(addr);
    if (conn == NULL) {
        local_irq_enable();
        return NULL;
    }
    LOG_I("open_sbc_channel: %d\n", channel);
    os_sem_create(&conn->sbc_sem, 0);
    conn->sbc_channel = channel;
    conn->codec_type = codec_type;
    conn->sbc_open = 1;
    conn->rxbuf_full = 0;
    conn->prev_a2dp_seqn = 0;
    for (node = conn->rx_acl_u.next; node != &conn->rx_acl_u; node = node->next) {
        rx = RX_OF(node);
        bits = RX_BITS(rx);
        if (bits & 8)
            continue;
        if ((bits & 0xc0) == 0 && RX_CID(rx) == conn->sbc_channel) {
            RX_BITS(rx) = (bits & ~0xf0) | 0x70;
            conn->sbc_packet_size = rx->data[1] << 8 | rx->data[0];
            if (conn->prev_a2dp_seqn == 0)
                conn->prev_a2dp_seqn = RX_SEQ(rx) - 1;
        }
    }
    if (!CONFIG_BTCTLER_TWS_ENABLE) {
        if (link_conn_num_more_than_one()) {
            for (p = lmp_acl_link; p < lmp_acl_link + 1; p++) {
                if (p == conn && !(p->core.state & 8)) {
                    LOG_I("----active bd\n");
                    link_conn_set_max_rx_bulk_persent(conn->link, 70);
                } else {
                    LOG_I("----background bd\n");
                }
            }
        } else {
            LOG_I("----only one bd\n");
            link_conn_set_max_rx_bulk_persent(conn->link, 100);
        }
    }
    local_irq_enable();
    link_conn_a2dp_channel_open(conn->link);
    return conn;
}

void *LMP_CODE lmp_private_sbc_source_slot(u8 *addr, u8 sbc_src_sta)
{
    struct lmp_connection *conn;

    if (!(config_btctler_modules & 1))
        return NULL;
    conn = lmp_conn_for_address(addr);
    if (conn == NULL) {
        LOG_E("link_task_reset_slot conn=NULL why\n");
        return NULL;
    }
    if (link_conn_num_more_than_one()) {
        if (sbc_src_sta)
            link_conn_a2dp_channel_open(conn->link);
        else
            link_conn_a2dp_channel_close(conn->link);
    }
    return conn;
}

int LMP_CODE lmp_get_a2dp_data_cache_low(struct lmp_connection *conn)
{
    return conn->codec_type == 0 ? CONFIG_A2DP_DATA_CACHE_LOW_SBC : CONFIG_A2DP_DATA_CACHE_LOW_AAC;
}

int LMP_CODE lmp_get_a2dp_data_cache_hi(struct lmp_connection *conn)
{
    return conn->codec_type == 0 ? CONFIG_A2DP_DATA_CACHE_HI_SBC : CONFIG_A2DP_DATA_CACHE_HI_AAC;
}

int LMP_CODE lmp_private_get_sbc_packet(void *_conn, u8 **frame, int block)
{
    struct lmp_connection *conn = _conn;
    struct list_head *node;
    struct bredr_rx_bulk *rx;
    int t;
    u8 bits;
    u16 seq;

    if (!(config_btctler_modules & 1))
        return -22;
    if (conn == NULL || conn->core.suspend) {
        LOG_I("sbc_get_suspend\n");
        return -22;
    }
    for (;;) {
        local_irq_disable();
        if (conn->link == NULL || (conn->core.state & 8) || !conn->sbc_open) {
            local_irq_enable();
            return -22;
        }
        if (conn->clear_a2dp_packet_num) {
            local_irq_enable();
            return 0;
        }
        os_sem_set(&conn->sbc_sem, 0);
        t = lmp_private_get_sbc_remain_time(conn, 1);
        if (t > lmp_get_a2dp_data_cache_low(conn) && t < CONFIG_A2DP_DELAY_TIME + 50) {
            if (t >= lmp_get_a2dp_data_cache_hi(conn))
                link_conn_qos_enable(conn->link);
        } else {
            link_conn_qos_disable(conn->link);
        }
        for (node = conn->rx_acl_u.next; node != &conn->rx_acl_u; node = node->next) {
            rx = RX_OF(node);
            bits = RX_BITS(rx);
            if ((bits & 0xc0) != 0x40)
                continue;
            RX_BITS(rx) = (bits & ~0xc0) | 0x80;
            local_irq_enable();
            seq = RX_SEQ(rx);
            if (prev_seqn_number && prev_seqn_number + 1 != seq)
                LOG_I("sbc_packet: %d, %d\n", prev_seqn_number, seq);
            prev_seqn_number = seq;
            *frame = rx->data + 4;
            return rx->len - 4;
        }
        local_irq_enable();
        if (!block)
            return 0;
        if (os_sem_pend(&conn->sbc_sem, 3))
            return -35;
    }
}

int LMP_IRQ __lmp_get_a2dp_packet_num(struct lmp_connection *conn)
{
    struct list_head *node;
    int num = 0;

    if (!conn->sbc_open)
        return 0;
    for (node = conn->rx_acl_u.next; node != &conn->rx_acl_u; node = node->next) {
        if (conn->sbc_channel == RX_CID(RX_OF(node)))
            num += (RX_BITS(RX_OF(node)) & 0xc0) != 0x80;
    }
    return num;
}

u32 LMP_CODE lmp_private_dump_rx_time(void *_conn, void *packet)
{
    if (!(config_btctler_modules & 1))
        return 0;
    return PACKET_RX(packet)->hash;
}

void *LMP_CODE lmp_private_fetch_sbc_packet(void *_conn, int *len, void *_prev, int msec)
{
    struct lmp_connection *conn = _conn;
    struct link_connection *link;
    struct list_head *node;
    struct bredr_rx_bulk *rx;
    int found = 0, wait;
    u8 bits;

    if (!(config_btctler_modules & 1)) {
        *len = 0;
        return NULL;
    }
    for (;;) {
        local_irq_disable();
        link = conn->link;      /* sic: read before the NULL test */
        wait = 0;
        if (conn && link && !conn->clear_a2dp_packet_num) {
            for (node = conn->rx_acl_u.next; node != &conn->rx_acl_u; node = node->next) {
                rx = RX_OF(node);
                bits = RX_BITS(rx);
                if (!(bits & 0xc0) || (bits & 8))
                    continue;
                if (_prev && !found) {
                    found = rx == PACKET_RX(_prev);
                    continue;
                }
                local_irq_enable();
                *len = rx->len - 4;
                return rx->data + 4;
            }
            wait = msec;
        }
        bredr_link_event(link, 1, 5);
        local_irq_enable();
        if (conn && wait && conn->sbc_open) {
            if (os_sem_pend(&conn->sbc_sem, wait / 10) == 0)
                continue;
            return NULL;
        }
        *len = 0;
        return NULL;
    }
}

int LMP_CODE lmp_private_get_sbc_packet_num(void *_conn)
{
    struct lmp_connection *conn = _conn;
    struct list_head *node;
    int num = 0;
    u8 bits;

    if (!(config_btctler_modules & 1))
        return 0;
    local_irq_disable();
    if (conn) {
        for (node = conn->rx_acl_u.next; node != &conn->rx_acl_u; node = node->next) {
            bits = RX_BITS(RX_OF(node));
            num += (bits & 0xc0) && !(bits & 8);
        }
    }
    local_irq_enable();
    return num;
}

void LMP_CODE lmp_private_free_sbc_packet(void *_conn, void *packet)
{
    struct lmp_connection *conn = _conn;
    struct bredr_rx_bulk *rx = PACKET_RX(packet);

    if (!(config_btctler_modules & 1))
        return;
    local_irq_disable();
    __list_del_entry(&rx->entry);
    if (conn && conn->link)
        link_conn_rx_bulk_avaliable(conn->link, rx);
    else
        bredr_rx_bulk_free(rx);
    local_irq_enable();
}

int LMP_CODE lmp_private_get_sbc_data_len(void *_conn)
{
    struct lmp_connection *conn = _conn;
    struct link_connection *link;
    struct list_head *node;
    struct bredr_rx_bulk *rx;
    int tws_len = 0, len1 = 0, len2 = 0;

    if (!(config_btctler_modules & 1))
        return 0;
    local_irq_disable();
    if (conn->link == NULL || (conn->core.state & 8) || !conn->sbc_open || conn->clear_a2dp_packet_num) {
        local_irq_enable();
        return 0;
    }
    if (CONFIG_BTCTLER_TWS_ENABLE)
        tws_len = tws_phone_link_sbc_data_len(conn->sbc_channel);
    link = conn->link;
    for (node = link->rx_head.next; node != &link->rx_head; node = node->next) {
        rx = RX_OF(node);
        if ((RX_BITS(rx) & 7) == 2 && conn->sbc_channel == RX_CID(rx))
            len1 += rx->data[1] << 8 | rx->data[0];
    }
    for (node = conn->rx_acl_u.next; node != &conn->rx_acl_u; node = node->next) {
        rx = RX_OF(node);
        if ((RX_BITS(rx) & 0xc0) == 0x40)
            len2 += rx->len - 4;
    }
    local_irq_enable();
    return len1 + tws_len + len2;
}

static int LMP_IRQ __get_rtp_header_len(u8 codec, u8 *buf, int len)
{
    int hlen = ((buf[0] << 2) & 60) + 12, ext = 0, r;
    u8 *p;

    if (buf[0] & 0x10) {
        p = buf + hlen + 2;
        ext = ((p[0] << 8 | p[1]) << 2) + 4;
    }
    r = ext + hlen + 1;
    if (codec == 0) {
        while (buf[r] != 0x9c) {
            if (++r > len) {
                r = len;
                break;
            }
        }
    }
    return r;
}

/* lmp_a2dp.c ~520 */
INLINE int __get_media_packet_play_time(u8 codec, u8 *buf, int len)
{
    static const u16 sample_rates[4] = { 16000, 32000, 44100, 48000 };
    int h;

    switch (codec) {
    case 0:
        h = __get_rtp_header_len(0, buf, len);
        if (h - 1 < len)
            return ((buf[h - 1] & 15) * 128000000) / sample_rates[buf[h] >> 6];
        return 0;
    case 2:
        h = __get_rtp_header_len(2, buf, len);
        if (h < len && buf[h] == 0xfc)
            return 23219;
        return 0;
    }
    return 0;
}

int LMP_IRQ lmp_get_sbc_remain_time_form_list(struct lmp_connection *conn, struct list_head *head)
{
    struct list_head *node;
    struct bredr_rx_bulk *rx;
    int t = 0;

    for (node = head->next; node != head; node = node->next) {
        rx = RX_OF(node);
        if ((RX_BITS(rx) & 15) == 2 && conn->sbc_channel == RX_CID(rx))
            t += __get_media_packet_play_time(conn->codec_type, rx->data + 4, rx->len - 4);
    }
    return t / 1000;
}

int LMP_IRQ lmp_private_get_sbc_remain_time(void *_conn, u8 include_tws)
{
    struct lmp_connection *conn = _conn;
    int t;

    if (!(config_btctler_modules & 1))
        return 0;
    local_irq_disable();
    if (conn == NULL || conn->link == NULL || (conn->core.state & 8) || !conn->sbc_open ||
        conn->clear_a2dp_packet_num) {
        local_irq_enable();
        return 0;
    }
    t = lmp_get_sbc_remain_time_form_list(conn, &conn->link->rx_head) +
        lmp_get_sbc_remain_time_form_list(conn, &conn->rx_acl_u);
    if (include_tws && CONFIG_BTCTLER_TWS_ENABLE && tws_conn.monitor_start)
        t += lmp_get_sbc_remain_time_form_list(conn, &tws_conn.rx_pending);
    local_irq_enable();
    return t;
}

static int LMP_CODE __get_last_a2dp_seqn_from_list(struct lmp_connection *conn, struct list_head *head)
{
    struct list_head *node;
    struct bredr_rx_bulk *rx;

    for (node = head->prev; node != head; node = node->prev) {
        rx = RX_OF(node);
        if ((RX_BITS(rx) & 15) == 2 && conn->sbc_channel == RX_CID(rx))
            return RX_SEQ(rx);
    }
    return -22;
}

/* lmp_a2dp.c ~619 */
INLINE int __get_last_a2dp_seqn(struct lmp_connection *conn)
{
    int seq;

    local_irq_disable();
    seq = __get_last_a2dp_seqn_from_list(conn, &tws_conn.rx_pending);
    if (seq < 0) {
        seq = __get_last_a2dp_seqn_from_list(conn, &conn->link->rx_head);
        if (seq < 0)
            seq = __get_last_a2dp_seqn_from_list(conn, &conn->rx_acl_u);
    }
    local_irq_enable();
    return seq;
}

int LMP_CODE __lmp_private_clear_a2dp_packet(void *_conn, u16 seqn_number)
{
    struct lmp_connection *conn = _conn;
    struct list_head *node, *next;
    struct bredr_rx_bulk *rx;
    int last = -1, seq, r;

    if (!(config_btctler_modules & 1))
        return 0;
    local_irq_disable();
    if (conn == NULL || conn->link == NULL || !conn->sbc_open) {
        local_irq_enable();
        return -22;
    }
    link_conn_qos_disable(conn->link);
    for (node = conn->rx_acl_u.next; node != &conn->rx_acl_u; node = next) {
        next = node->next;
        rx = RX_OF(node);
        if ((RX_BITS(rx) & 0xc0) != 0x40)
            continue;
        seq = RX_SEQ(rx);
        if (seqn_number && (s16)(seq - seqn_number) >= 0) {
            link_conn_rx_bulk_avaliable(conn->link, NULL);
            last = seq;
            goto done;
        }
        __list_del_entry(node);
        bredr_rx_bulk_free(rx);
        last = seq;
    }
    r = lmp_private_abandon_sbc_data(conn, &conn->link->rx_head, seqn_number);
    last = r > -1 ? r : last;
    if (CONFIG_BTCTLER_TWS_ENABLE) {
        if (seqn_number == 0 || last == -1 || (s16)(last - seqn_number + 1) < 0) {
            r = tws_phone_link_abandon_sbc_data(conn, seqn_number);
            last = r > -1 ? r : last;
        }
    }
    link_conn_rx_bulk_avaliable(conn->link, NULL);
    if (seqn_number == 0) {
        conn->clear_a2dp_packet_num = 0;
        local_irq_enable();
        return 0;
    }
    if (last == -1) {
        conn->clear_a2dp_packet_num = seqn_number;
        local_irq_enable();
        return 0;
    }
done:
    conn->clear_a2dp_packet_num = (s16)(last - seqn_number + 1) < 0 ? seqn_number : 0;
    local_irq_enable();
    return 0;
}

static void LMP_CODE tws_lmp_clear_a2dp_packet(u8 *data, u16 len, u8 rx)
{
    u16 seqn;
    struct lmp_connection *conn;

    if (!(config_btctler_modules & 1))
        return;
    seqn = ((u16 *)data)[1];
    conn = lmp_conn_for_handle(((u16 *)data)[0]);
    LOG_V("tws_clear_a2dp: %d\n", seqn);
    __lmp_private_clear_a2dp_packet(conn, seqn);
}

/* The callback has the SDK's u8 argument ABI, as in local_media_sync.c. */
struct lmp_func_stub { u32 id; void (*func)(u8 *, u16, u8); };
static const struct lmp_func_stub clear_a2dp_packet_stub
    __attribute__((used, section(".tws_func_stub"), aligned(4))) = { 0x43413244, tws_lmp_clear_a2dp_packet };

int LMP_CODE lmp_private_clear_a2dp_packet(void *_conn, u16 seqn_number)
{
    struct lmp_connection *conn = _conn;
    u16 buf[2];
    int role, state;

    if (!(config_btctler_modules & 1))
        return 0;
    LOG_V("clear_a2dp_packet: %d\n", seqn_number);
    if (CONFIG_BTCTLER_TWS_ENABLE) {
        role = tws_api_get_role_internal();
        state = tws_api_get_tws_state();
        if (role == 0) {
            if (state & 2) {
                if (seqn_number == 0)
                    seqn_number = __get_last_a2dp_seqn(conn);
                conn->clear_a2dp_packet_num = seqn_number;
                buf[0] = conn->core_s.handle;
                buf[1] = seqn_number;
                return tws_api_send_data_to_slave(buf, 4, 0x43413244);
            }
        } else {
            if (conn->clear_a2dp_packet_num)
                return 0;
            seqn_number = 0;
        }
    }
    return __lmp_private_clear_a2dp_packet(conn, seqn_number);
}

int LMP_CODE lmp_private_is_clearing_a2dp_packet(void *_conn)
{
    struct lmp_connection *conn = _conn;

    if (!(config_btctler_modules & 1))
        return 0;
    if (conn == NULL || conn->clear_a2dp_packet_num == 0)
        return 0;
    return 1;
}

int LMP_CODE lmp_private_a2dp_channel_exist(void *_conn)
{
    struct lmp_connection *conn = _conn;

    if (!(config_btctler_modules & 1))
        return 0;
    local_irq_disable();
    if (conn == NULL || (conn->core.state & 8) || conn->link == NULL || !conn->sbc_open) {
        local_irq_enable();
        return 0;
    }
    local_irq_enable();
    return 1;
}

void LMP_CODE lmp_private_close_sbc_channel(void *_conn)
{
    struct lmp_connection *conn = _conn;

    if (!(config_btctler_modules & 1))
        return;
    LOG_I("close_sbc_channel: %d\n", conn->sbc_channel);
    local_irq_disable();
    if (conn == NULL || !conn->sbc_open) {
        local_irq_enable();
        return;
    }
    if (!(conn->core.state & 8) && conn->link) {
        __lmp_private_clear_a2dp_packet(conn, 0);
        link_conn_a2dp_channel_close(conn->link);
    }
    conn->sbc_open = 0;
    local_irq_enable();
    os_sem_post(&conn->sbc_sem);
}
