/*
 * Generic PCI Express Root Port emulation
 *
 * Copyright (C) 2017 Red Hat Inc
 *
 * Authors:
 *   Marcel Apfelbaum <marcel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/pci/msix.h"
#include "hw/pci/pcie_port.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_GEN_PCIE_ROOT_PORT                "pcie-root-port"
OBJECT_DECLARE_SIMPLE_TYPE(GenPCIERootPort, GEN_PCIE_ROOT_PORT)

#define GEN_PCIE_ROOT_PORT_AER_OFFSET           0x100
#define GEN_PCIE_ROOT_PORT_ACS_OFFSET \
        (GEN_PCIE_ROOT_PORT_AER_OFFSET + PCI_ERR_SIZEOF)

#define GEN_PCIE_ROOT_PORT_MSIX_NR_VECTOR       1
#define GEN_PCIE_ROOT_DEFAULT_IO_RANGE          4096

#define PHISON_MODEL_BRIDGE_ENABLED(grp) ((grp->phison_model_bridge_port > 0) && (grp->phison_model_bridge_ip)) 
#define PHISON_MODEL_BRIDGE_RECONNECT_ENABLED(grp) ((grp->reconnect == true)) 

typedef enum {
    PHISON_CONN_CONNECTED,
    PHISON_CONN_DISCONNECTED,
    PHISON_CONN_RECONNECTING,
} PhisonConnState;

struct GenPCIERootPort {
    /*< private >*/
    PCIESlot parent_obj;
    /*< public >*/

    bool migrate_msix;

    /* additional resources to reserve */
    PCIResReserve res_reserve;

    PhisonConnState  phison_model_bridge_conn_state;
    QEMUTimer       *phison_model_bridge_reconnect_timer;
    int              phison_model_bridge_reconnect_fd;   /* in-progress connect fd */
    bool              reconnect;   /* in-progress connect fd */

    DeviceState *phison_source;
    char *phison_model_bridge_ip;
    uint16_t phison_model_bridge_port;
    int phison_model_bridge_socket;
};

static uint8_t gen_rp_aer_vector(const PCIDevice *d)
{
    return 0;
}

static int gen_rp_interrupts_init(PCIDevice *d, Error **errp)
{
    int rc;

    rc = msix_init_exclusive_bar(d, GEN_PCIE_ROOT_PORT_MSIX_NR_VECTOR, 0, errp);

    if (rc < 0) {
        assert(rc == -ENOTSUP);
    } else {
        msix_vector_use(d, 0);
    }

    return rc;
}

static void gen_rp_interrupts_uninit(PCIDevice *d)
{
    msix_uninit_exclusive_bar(d);
}

static bool gen_rp_test_migrate_msix(void *opaque, int version_id)
{
    GenPCIERootPort *rp = opaque;

    return rp->migrate_msix;
}


#include "qemu/range.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"

#define PHISON_CAP_ID_HEADER    0u
#define PHISON_CAP_ID_EXT_FLAG  0x10000u

#define PHISON_PCIE_CAP_SIZE    0x3C
#define PHISON_AER_SIZE         0x48
#define PHISON_ACS_SIZE         8

// 支援 Timeout 的 phison_model_socket_use
// 成功回傳 0，一般錯誤回傳 -1，超時回傳 -2
static int phison_model_socket_use(int sock_fd, void *data, size_t size, bool is_send) 
{
    if (sock_fd < 0 || !data || size == 0) {
        return -1;
    }

    if (is_send) {
        // --- 執行發送邏輯 ---
        ssize_t sent = send(sock_fd, data, size, 0);
        if (sent < (ssize_t)size) {
            printf("[Socket %d] Send 失敗: %ld/%ld bytes\n", sock_fd, sent, size);
            return -1;
        }
    } 
    else {
        // --- 執行接收邏輯 ---
        
        // 1. 設定超時時間（例如：設定為 10 秒，可根據需求調整秒數或微秒）
        struct timeval timeout;
        timeout.tv_sec = 1;  // 10 秒
        timeout.tv_usec = 0;

        // 設定 Socket 接收超時屬性
        if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) < 0) {
            printf("[Socket %d] setsockopt SO_RCVTIMEO 失敗\n", sock_fd);
            return -1;
        }

        // 使用 MSG_WAITALL 確保收齊，此時若超過 2 秒沒收完會被中斷並回傳錯誤
        ssize_t received = recv(sock_fd, data, size, MSG_WAITALL);

        if (received < (ssize_t)size) {
            if (received == 0) {
                printf("[Socket %d] Connection closed by model\n", sock_fd);
                return -1; // 斷線回傳 -1
            } 
            
            // 檢查是否是因為超時引發的錯誤
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("[Socket %d] Recv TIMEOUT triggered (%ld 秒)\n", sock_fd, timeout.tv_sec);
                return -2; // 超時回傳 -2
            }

            printf("[Socket %d] Recv failed or incomplete: %ld/%ld bytes (errno: %d)\n", 
                    sock_fd, received, size, errno);
            return -1;
        }

        // 2. 接收成功後，建議把超時重設回 0（代表無限期阻塞），避免影響 QEMU 其他潛在的 Socket 操作
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    }

    return 0;
}

// ============================================================
// TCP keepalive + user timeout: lets the kernel detect a
// sudden-death peer without any application-level heartbeat
// ============================================================
static void phison_set_socket_keepalive(int fd)
{
    int enable = 1;
    int idle   = 2;   // start probing after 2s of no traffic
    int intvl  = 2;   // 2s between each probe
    int cnt    = 2;   // 2 consecutive unanswered probes = dead

    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));

    // Hard ceiling on "time since the last ACK we received", in ms.
    // This covers BOTH real outbound data waiting on a retransmit
    // AND the keepalive probes above waiting on a response — if
    // either goes unacknowledged past this timeout, the connection
    // is force-failed immediately instead of waiting out TCP's
    // much longer default retransmission backoff (which can run
    // into minutes). Effectively acts as an upper bound for the
    // keepalive sequence too, since it doesn't care whether the
    // unacked packet was a probe or actual data.
    unsigned int user_timeout_ms = 6000;
    setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout_ms, sizeof(user_timeout_ms));
}

// ============================================================
// One-port reconnect state machine
// ============================================================
static void phison_model_bridge_reconnect_start(GenPCIERootPort *grp);

static void phison_model_bridge_reconnect_fail(GenPCIERootPort *grp, int fd)
{
    if (fd >= 0) {
        qemu_set_fd_handler(fd, NULL, NULL, NULL);
        close(fd);
        grp->phison_model_bridge_reconnect_fd = -1;
    }
    if (grp->phison_model_bridge_socket >= 0) {
        qemu_set_fd_handler(grp->phison_model_bridge_socket, NULL, NULL, NULL);
        close(grp->phison_model_bridge_socket);
        grp->phison_model_bridge_socket = -1;
    }
    printf("[Bridge] Reconnect failed, retry in 1s\n");
    grp->phison_model_bridge_conn_state = PHISON_CONN_DISCONNECTED;
    timer_mod(grp->phison_model_bridge_reconnect_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
}

/* 你原本的，職責：清理 socket + 觸發 reconnect。維持不變 */
static void phison_model_bridge_on_disconnect(GenPCIERootPort *grp)
{
    if (grp->phison_model_bridge_conn_state == PHISON_CONN_RECONNECTING) return;

    printf("[Bridge] Disconnected, starting reconnect\n");

    if (grp->phison_model_bridge_socket >= 0) {
        qemu_set_fd_handler(grp->phison_model_bridge_socket, NULL, NULL, NULL);
        close(grp->phison_model_bridge_socket);
        grp->phison_model_bridge_socket = -1;
    }
    grp->phison_model_bridge_conn_state = PHISON_CONN_DISCONNECTED;
    phison_model_bridge_reconnect_start(grp);
}

/* 新加的，職責：給 qemu_set_fd_handler 用的正確簽名 wrapper，
   做 MSG_PEEK 偵測，確認真的斷線才 call 上面那個 */
static void phison_model_bridge_disconnect_handler(void *opaque)
{
    GenPCIERootPort *grp = opaque;
    int fd = grp->phison_model_bridge_socket;

    char buf[1];
    int ret = recv(fd, buf, sizeof(buf), MSG_PEEK | MSG_DONTWAIT);

    if (ret == 0) {
        printf("[Bridge] Disconnected (EOF), triggering reconnect\n");
        phison_model_bridge_on_disconnect(grp);
    } else if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        printf("[Bridge] Disconnected (errno=%d: %s), triggering reconnect\n",
               errno, strerror(errno));
        phison_model_bridge_on_disconnect(grp);
    }
}

static void phison_model_bridge_on_connected(void *opaque)
{
    GenPCIERootPort *grp = opaque;
    int fd = grp->phison_model_bridge_reconnect_fd;

    qemu_set_fd_handler(fd, NULL, NULL, NULL);
    grp->phison_model_bridge_reconnect_fd = -1;

    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {

        printf("[Bridge] Connect failed: %s\n", strerror(err));
        phison_model_bridge_reconnect_fail(grp, fd);
        return;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    phison_set_socket_keepalive(fd);
    grp->phison_model_bridge_socket = fd;

    /* ← 補上：重新掛 disconnect handler，否則下一次斷線偵測不到 */
    qemu_set_fd_handler(fd, phison_model_bridge_disconnect_handler, NULL, grp);

    grp->phison_model_bridge_conn_state = PHISON_CONN_CONNECTED;
        printf("[Bridge] Connected (fd=%d)\n", fd);
    
}



static void phison_model_bridge_reconnect_start(GenPCIERootPort *grp)
{
    if (grp->phison_model_bridge_conn_state == PHISON_CONN_RECONNECTING) return;

    grp->phison_model_bridge_conn_state = PHISON_CONN_RECONNECTING;
    printf("[Bridge] Starting reconnect...\n");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        phison_model_bridge_reconnect_fail(grp, -1);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(grp->phison_model_bridge_port);
    if (inet_pton(AF_INET, grp->phison_model_bridge_ip, &addr.sin_addr) <= 0) {
        phison_model_bridge_reconnect_fail(grp, fd);
        return;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);
    connect(fd, (struct sockaddr *)&addr, sizeof(addr)); /* EINPROGRESS is OK */

    grp->phison_model_bridge_reconnect_fd = fd;
    qemu_set_fd_handler(fd, NULL, phison_model_bridge_on_connected, grp);
}

static void phison_model_bridge_reconnect_timer_cb(void *opaque)
{
    phison_model_bridge_reconnect_start((GenPCIERootPort *)opaque);
}

typedef struct QEMU_PACKED PhisonBridgeMMIOWriteInfo {
    uint32_t cap_id;   /* 0 = Type1 header, id = PCI CAP, 0x10000 | id = PCIe extended CAP */
    uint32_t offset;   /* relative offset from CAP start, DWORD aligned (header: absolute) */
    uint32_t write_data;   /* relative offset from CAP start, DWORD aligned (header: absolute) */
    uint32_t old_data; /* whole DWORD before write */
    uint32_t new_data; /* whole DWORD after write (effective value) */
    uint32_t size;     /* write length: 1/2/4 */
} PhisonBridgeMMIOWriteInfo;

QEMU_BUILD_BUG_ON(sizeof(PhisonBridgeMMIOWriteInfo) != 24);

/* ---------- cap id -> name (debug) ---------- */
static const char *phison_cap_name(uint32_t cap_id)
{
    if (cap_id & PHISON_CAP_ID_EXT_FLAG) {
        switch (cap_id & 0xFFFF) {
        case PCI_EXT_CAP_ID_ERR: return "AER";
        case PCI_EXT_CAP_ID_ACS: return "ACS";
        default:                 return "EXT_UNKNOWN";
        }
    }
    switch (cap_id) {
    case PHISON_CAP_ID_HEADER: return "TYPE1_HEADER";
    case PCI_CAP_ID_EXP:       return "PCIE";
    case PCI_CAP_ID_MSIX:      return "MSIX";
    default:                   return "UNKNOWN";
    }
}

/* ---------- locate cap id + relative offset ---------- */
/* addr/len: 原始 write 的位址與長度; rel_off 回傳 DWORD 對齊後的相對 offset */
static bool phison_locate_cap(PCIDevice *d, uint32_t addr, int len,
                              uint32_t *cap_id, uint32_t *rel_off)
{
    if (ranges_overlap(addr, len, 0, PCI_STD_HEADER_SIZEOF)) {
        *cap_id  = PHISON_CAP_ID_HEADER;
        *rel_off = addr;                         /* header: 絕對 offset */
        return true;
    }
    if (d->exp.exp_cap &&
        ranges_overlap(addr, len, d->exp.exp_cap, PHISON_PCIE_CAP_SIZE)) {
        *cap_id  = PCI_CAP_ID_EXP;
        *rel_off = addr - d->exp.exp_cap;
        return true;
    }
    if (d->exp.aer_cap &&
        ranges_overlap(addr, len, d->exp.aer_cap, PHISON_AER_SIZE)) {
        *cap_id  = PHISON_CAP_ID_EXT_FLAG | PCI_EXT_CAP_ID_ERR;
        *rel_off = addr - d->exp.aer_cap;
        return true;
    }
    if (d->exp.acs_cap &&
        ranges_overlap(addr, len, d->exp.acs_cap, PHISON_ACS_SIZE)) {
        *cap_id  = PHISON_CAP_ID_EXT_FLAG | PCI_EXT_CAP_ID_ACS;
        *rel_off = addr - d->exp.acs_cap;
        return true;
    }
    if (d->msix_cap &&
        ranges_overlap(addr, len, d->msix_cap, PCI_CAP_MSIX_SIZEOF)) {
        *cap_id  = PCI_CAP_ID_MSIX;
        *rel_off = addr - d->msix_cap;
        return true;
    }
    return false;
}

/* ---------- send (TODO) ---------- */
static void phison_send_info(GenPCIERootPort *grp, PhisonBridgeMMIOWriteInfo *info)
{
    /* TODO: non-blocking send to the bridge socket */
    int sock_fd = grp->phison_model_bridge_socket;
    int send_ret;
    // int vmid = get_vmid_from_qmp_chardev();
    // int stuck_seconds = 0;

    send_ret = phison_model_socket_use(sock_fd, (void*)info,
                                        sizeof(PhisonBridgeMMIOWriteInfo), true);
    if (send_ret != 0) {
        printf("[phison_send_info] Error happened when sending Phison Bridge MMIO Write Info\n");
    } 
    else
    {
        printf("[phison_send_info] Data sent succcessfully\n");
    }
}

static void gen_rp_aer_vector_update(PCIDevice *d)
{
    PCIERootPortClass *rpc = PCIE_ROOT_PORT_GET_CLASS(d);

    if (rpc->aer_vector) {
        pcie_aer_root_set_vector(d, rpc->aer_vector(d));
    }
}

static void gen_rp_write_config(PCIDevice *d, uint32_t address,
                            uint32_t val, int len)
{
    uint32_t root_cmd =
        pci_get_long(d->config + d->exp.aer_cap + PCI_ERR_ROOT_COMMAND);
    uint16_t slt_ctl, slt_sta;

    pcie_cap_slot_get(d, &slt_ctl, &slt_sta);

    pci_bridge_write_config(d, address, val, len);
    gen_rp_aer_vector_update(d);
    pcie_cap_slot_write_config(d, slt_ctl, slt_sta, address, val, len);
    pcie_aer_write_config(d, address, val, len);
    pcie_aer_root_write_config(d, address, val, len, root_cmd);
}

/* 取出 config 中 [addr, addr+len) 的值,小端組合 */
static uint32_t phison_cfg_get(PCIDevice *d, uint32_t addr, int len)
{
    uint32_t v = 0;
    for (int i = 0; i < len; i++) {
        v |= (uint32_t)d->config[addr + i] << (i * 8);
    }
    return v;
}

/* ---------- config_write hook ---------- */
// static void bridge_config_write(PCIDevice *d, uint32_t address,
//                                 uint32_t val, int len)
// {
//     // uint32_t aligned = address & ~3u;
//     uint32_t old_dw  = pci_get_long(d->config + aligned);
//     GenPCIERootPort *grp = GEN_PCIE_ROOT_PORT(d);
//     gen_rp_write_config(d, address, val, len);

//     uint32_t new_dw = pci_get_long(d->config + aligned);

//     /* debug: 想看「guest 有寫但沒生效」的情況, 暫時註解掉這個 return */
//     // if (old_dw == new_dw) {
//     //     return;
//     // }

//     uint32_t cap_id, rel_off;
//     if (!phison_locate_cap(d, address, len, &cap_id, &rel_off)) {
//         printf("[phison-bridge] abs=0x%03x not in known cap "
//                 "len=%d val=0x%x old=0x%08x new=0x%08x\n",
//                 aligned, len, val, old_dw, new_dw);
//         return;
//     }

//     PhisonBridgeMMIOWriteInfo info = {
//         .cap_id   = cap_id,
//         .offset   = rel_off,
//         .write_data = val,
//         .old_data = old_dw,
//         .new_data = new_dw,
//         .size     = len,
//     };

//     printf("[phison-bridge] abs=0x%03x cap=%s(0x%x) rel=0x%02x "
//             "len=%d val=0x%x old=0x%08x new=0x%08x diff=0x%08x "
//             "wmask=0x%08x w1c=0x%08x\n",
//             aligned, phison_cap_name(info.cap_id), info.cap_id, info.offset,
//             len, val, info.old_data, info.new_data,
//             info.old_data ^ info.new_data,
//             pci_get_long(d->wmask + aligned),
//             pci_get_long(d->w1cmask + aligned));

//     phison_send_info(grp, &info);
// }


static void bridge_config_write(PCIDevice *d, uint32_t address,
                                uint32_t val, int len)
{
    GenPCIERootPort *grp = GEN_PCIE_ROOT_PORT(d);
    uint32_t old_v = phison_cfg_get(d, address, len);

    gen_rp_write_config(d, address, val, len);

    uint32_t new_v = phison_cfg_get(d, address, len);

    uint32_t cap_id, rel_off;
    if (!phison_locate_cap(d, address, len, &cap_id, &rel_off)) {
        printf("[phison-bridge] abs=0x%03x not in known cap "
               "len=%d val=0x%x old=0x%x new=0x%x\n",
               address, len, val, old_v, new_v);
        return;
    }

    PhisonBridgeMMIOWriteInfo info = {
        .cap_id     = cap_id,
        .offset     = rel_off,
        .size       = len,
        .write_data = val,
        .old_data   = old_v,
        .new_data   = new_v,
    };

    printf("[phison-bridge] abs=0x%03x cap=%s(0x%x) rel=0x%02x len=%d "
           "val=0x%x old=0x%x new=0x%x wmask=0x%x w1c=0x%x\n",
           address, phison_cap_name(cap_id), cap_id, rel_off, len, val,
           old_v, new_v,
           phison_cfg_get(d->wmask ? (PCIDevice *)NULL : NULL, 0, 0), 0);

    uint32_t wm = 0, w1c = 0;
    for (int i = 0; i < len; i++) {
        wm  |= (uint32_t)d->wmask[address + i]   << (i * 8);
        w1c |= (uint32_t)d->w1cmask[address + i] << (i * 8);
    }
    printf("[phison-bridge] abs=0x%03x cap=%s(0x%x) rel=0x%02x len=%d "
           "val=0x%x old=0x%x new=0x%x wmask=0x%x w1c=0x%x\n",
           address, phison_cap_name(cap_id), cap_id, rel_off, len, val,
           old_v, new_v, wm, w1c);
    phison_send_info(grp, &info);
}

static void gen_rp_realize(DeviceState *dev, Error **errp)
{
    PCIDevice *d = PCI_DEVICE(dev);
    PCIESlot *s = PCIE_SLOT(d);
    GenPCIERootPort *grp = GEN_PCIE_ROOT_PORT(d);
    PCIERootPortClass *rpc = PCIE_ROOT_PORT_GET_CLASS(d);
    Error *local_err = NULL;

    rpc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    /*
     * reserving IO space led to worse issues in 6.1, when this hunk was
     * introduced. (see commit: 211afe5c69b59). Keep this broken for 6.1
     * machine type ABI compatibility only
     */
    if (s->hide_native_hotplug_cap && grp->res_reserve.io == -1 && s->hotplug) {
        grp->res_reserve.io = GEN_PCIE_ROOT_DEFAULT_IO_RANGE;
    }
    int rc = pci_bridge_qemu_reserve_cap_init(d, 0,
                                              grp->res_reserve, errp);

    if (rc < 0) {
        rpc->parent_class.exit(d);
        return;
    }

    if (!grp->res_reserve.io) {
        pci_word_test_and_clear_mask(d->wmask + PCI_COMMAND,
                                     PCI_COMMAND_IO);
        d->wmask[PCI_IO_BASE] = 0;
        d->wmask[PCI_IO_LIMIT] = 0;
    }
    grp->phison_model_bridge_socket = -1;


    if ((grp->phison_model_bridge_port != 0))
    {
        const char *ip_source = NULL;

        /* 情況 1：優先使用新參數 simulate_model_tester_ip */
        if (grp->phison_model_bridge_ip && grp->phison_model_bridge_ip[0] != '\0')
        {
            ip_source = "phison_model_bridge_ip";
        }
        /* 情況 2：從 phison_source (nvme device) 取得 phison_model_ip */
        else if (grp->phison_source)
        {
            char *ip = object_property_get_str(OBJECT(grp->phison_source),
                                            "phison_model_ip", NULL);
            if (ip && ip[0] != '\0')
            {
                g_free(grp->phison_model_bridge_ip); /* 可能是空字串，避免 leak */
                grp->phison_model_bridge_ip = ip;
                ip_source = "phison_source";
            }
            else
            {
                g_free(ip);
            }
        }

        /* 三個來源都沒有有效 IP */
        if (!ip_source)
        {
            error_setg(errp, "[Phison] Bridge port set, but no valid model IP configured.");
            return;
        }

        printf("[Phison] Using model IP (%s): %s\n",
            ip_source, grp->phison_model_bridge_ip);
    }


    if (PHISON_MODEL_BRIDGE_ENABLED(grp))
    {
        printf("[PCIe Root Port] Going into Bridge socket creation block\n");
        struct sockaddr_in server_addr = {0};
        grp->phison_model_bridge_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (grp->phison_model_bridge_socket < 0) {
            error_setg(errp, "nvme phison model socket construct fail.");
            return;
        }

        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(grp->phison_model_bridge_port);

        if (inet_pton(AF_INET, grp->phison_model_bridge_ip, &server_addr.sin_addr) <= 0) {
            error_setg(errp, "phison model bridge socket inet pton fail.");
            return;
        }
        //printf("inet_pton done\n");
        //fflush(stdout);

        if (connect(grp->phison_model_bridge_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            error_setg(errp, "phison model bridge socket connect fail.");
            return;
        }

        phison_set_socket_keepalive(grp->phison_model_bridge_socket);
        if (PHISON_MODEL_BRIDGE_RECONNECT_ENABLED(grp))
        {
            printf("Bridge Reconnect Enabled!\n");
            qemu_set_fd_handler(grp->phison_model_bridge_socket, phison_model_bridge_disconnect_handler, NULL, grp);
        }
        else 
        {
            printf("Bridge Reconnect Disabled!\n");
        }
        d->config_write = bridge_config_write;

        // Originally QEMU does not allow write to Link Disable bit
        // Now need to enable
        pci_set_word(d->wmask + d->exp.exp_cap + PCI_EXP_LNKCTL,
             pci_get_word(d->wmask + d->exp.exp_cap + PCI_EXP_LNKCTL)
             | PCI_EXP_LNKCTL_LD);
    }
    grp->phison_model_bridge_reconnect_fd    = -1;
    grp->phison_model_bridge_conn_state      = PHISON_CONN_CONNECTED;
    grp->phison_model_bridge_reconnect_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                            phison_model_bridge_reconnect_timer_cb, s);
}

static const VMStateDescription vmstate_rp_dev = {
    .name = "pcie-root-port",
    .priority = MIG_PRI_PCI_BUS,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pcie_cap_slot_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj.parent_obj.parent_obj, PCIESlot),
        VMSTATE_STRUCT(parent_obj.parent_obj.parent_obj.exp.aer_log,
                       PCIESlot, 0, vmstate_pcie_aer_log, PCIEAERLog),
        VMSTATE_MSIX_TEST(parent_obj.parent_obj.parent_obj.parent_obj,
                          GenPCIERootPort,
                          gen_rp_test_migrate_msix),
        VMSTATE_END_OF_LIST()
    }
};

static Property gen_rp_props[] = {
    DEFINE_PROP_BOOL("x-migrate-msix", GenPCIERootPort,
                     migrate_msix, true),
    DEFINE_PROP_UINT32("bus-reserve", GenPCIERootPort,
                       res_reserve.bus, -1),
    DEFINE_PROP_SIZE("io-reserve", GenPCIERootPort,
                     res_reserve.io, -1),
    DEFINE_PROP_SIZE("mem-reserve", GenPCIERootPort,
                     res_reserve.mem_non_pref, -1),
    DEFINE_PROP_SIZE("pref32-reserve", GenPCIERootPort,
                     res_reserve.mem_pref_32, -1),
    DEFINE_PROP_SIZE("pref64-reserve", GenPCIERootPort,
                     res_reserve.mem_pref_64, -1),
    DEFINE_PROP_PCIE_LINK_SPEED("x-speed", PCIESlot,
                                speed, PCIE_LINK_SPEED_16),
    DEFINE_PROP_PCIE_LINK_WIDTH("x-width", PCIESlot,
                                width, PCIE_LINK_WIDTH_32),
    DEFINE_PROP_LINK("phison_source", GenPCIERootPort, phison_source, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_STRING("phison_model_bridge_ip", GenPCIERootPort, phison_model_bridge_ip),
    DEFINE_PROP_UINT16("phison_model_bridge_port", GenPCIERootPort, phison_model_bridge_port, 0),
    DEFINE_PROP_BOOL("reconnect", GenPCIERootPort, reconnect, false),
    DEFINE_PROP_END_OF_LIST()
};

static void gen_rp_dev_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    PCIERootPortClass *rpc = PCIE_ROOT_PORT_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_REDHAT;
    k->device_id = PCI_DEVICE_ID_REDHAT_PCIE_RP;
    dc->desc = "PCI Express Root Port";
    dc->vmsd = &vmstate_rp_dev;
    device_class_set_props(dc, gen_rp_props);

    device_class_set_parent_realize(dc, gen_rp_realize, &rpc->parent_realize);

    rpc->aer_vector = gen_rp_aer_vector;
    rpc->interrupts_init = gen_rp_interrupts_init;
    rpc->interrupts_uninit = gen_rp_interrupts_uninit;
    rpc->aer_offset = GEN_PCIE_ROOT_PORT_AER_OFFSET;
    rpc->acs_offset = GEN_PCIE_ROOT_PORT_ACS_OFFSET;
}

static const TypeInfo gen_rp_dev_info = {
    .name          = TYPE_GEN_PCIE_ROOT_PORT,
    .parent        = TYPE_PCIE_ROOT_PORT,
    .instance_size = sizeof(GenPCIERootPort),
    .class_init    = gen_rp_dev_class_init,
};

 static void gen_rp_register_types(void)
 {
    type_register_static(&gen_rp_dev_info);
 }
 type_init(gen_rp_register_types)
