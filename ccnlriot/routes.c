#include <stddef.h>
#include <stdint.h>
#include "net/gnrc/netapi.h"
#include "net/netopt.h"
#include "net/gnrc/netif.h"
#include "ccn-lite-riot.h"
#include "ccnl-pkt-ndntlv.h"
#include "routes.h"
#include "ccnlriot.h"

static char addr_str[CCNLRIOT_ADDRLEN * 3];

static inline int _addr_cmp(uint8_t *a, uint8_t *b, size_t addr_len)
{
    for (unsigned int i = 0; i < addr_len; i++) {
        if (a[i] != b[i]) {
            return 1;
        }
    }
    return 0;
}

static inline int _get_pos(uint8_t *addr, size_t addr_len)
{
    for (int i = 0; i < CCNLRIOT_NUMBER_OF_NODES; i++) {
        if (_addr_cmp(addr, ccnlriot_id[i], addr_len) == 0) {
            return i;
        }
    }
    return -1;
}

int ccnlriot_get_mypos(void)
{
    uint8_t hwaddr[CCNLRIOT_ADDRLEN];
#if USE_LONG && !defined(BOARD_NATIVE)
    int res = gnrc_netapi_get(CCNLRIOT_NETIF, NETOPT_ADDRESS_LONG, 0, hwaddr, sizeof(hwaddr));
#else
    int res = gnrc_netapi_get(CCNLRIOT_NETIF, NETOPT_ADDRESS, 0, hwaddr, sizeof(hwaddr));
#endif
    if (res < 0) {
        puts("ccnlriot_routes_setup: critical error, aborting");
        return -1;
    }
    return _get_pos(hwaddr, CCNLRIOT_ADDRLEN);
}

void ccnlriot_routes_setup(void)
{
    int my_pos = ccnlriot_get_mypos();
    if (my_pos < 0) {
        puts("ccnlriot_routes_setup: critical error, couldn't find my address in list, aborting");
        return;
    }

    printf("I am number %i in the list, adding ", my_pos);
    if (my_pos > 0) {
        printf("%s and ", gnrc_netif_addr_to_str(addr_str, sizeof(addr_str), ccnlriot_id[my_pos - 1], CCNLRIOT_ADDRLEN));
    }
    if (my_pos < CCNLRIOT_NUMBER_OF_NODES - 1) {
        printf("%s\n", gnrc_netif_addr_to_str(addr_str, sizeof(addr_str), ccnlriot_id[my_pos + 1], CCNLRIOT_ADDRLEN));
    }

    if (my_pos > 0) {
        if (ccnlriot_routes_add(ccnlriot_prefix1, ccnlriot_id[my_pos - 1], CCNLRIOT_ADDRLEN) < 0) {
            puts("ccnlriot_routes_setup: critical error setting up the first FIB entry, aborting");
            return;
        }
    }
    if (my_pos < CCNLRIOT_NUMBER_OF_NODES - 1) {
        if (ccnlriot_routes_add(ccnlriot_prefix2, ccnlriot_id[my_pos + 1], CCNLRIOT_ADDRLEN) < 0) {
            puts("ccnlriot_routes_setup: critical error setting up the second FIB entry, aborting");
            return;
        }
    }
}

int ccnlriot_routes_add(char *pfx, uint8_t *relay_addr, size_t addr_len)
{
    int suite = CCNL_SUITE_NDNTLV;

    struct ccnl_prefix_s *prefix = ccnl_URItoPrefix(pfx, suite, NULL, 0);

    if (!prefix) {
        puts("ccnlriot_routes_add: Error: prefix could not be created!");
        return -1;
    }

    sockunion sun;
    sun.sa.sa_family = AF_PACKET;
    memcpy(&(sun.linklayer.sll_addr), relay_addr, addr_len);
    sun.linklayer.sll_halen = addr_len;
    sun.linklayer.sll_protocol = htons(ETHERTYPE_NDN);

    /* TODO: set correct interface instead of always 0 */
    struct ccnl_face_s *fibface = ccnl_get_face_or_create(&ccnl_relay, 0, &sun.sa, sizeof(sun.linklayer));
    fibface->flags |= CCNL_FACE_FLAGS_STATIC;

    if (ccnl_fib_add_entry(&ccnl_relay, prefix, fibface) != 0) {
        printf("Error adding (%s : %s) to the FIB\n", pfx, gnrc_netif_addr_to_str(addr_str, sizeof(addr_str), relay_addr, addr_len));
        return -1;
    }

    return 0;
}
