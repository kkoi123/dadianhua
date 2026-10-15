#ifndef QDC_TRAFFIC_H
#define QDC_TRAFFIC_H
#include <stdint.h>
typedef struct {
    uint64_t received, sent, since;
    unsigned interfaces;
} QdcTraffic;
/* Shared by the entry master and authenticated CGI. reset is durable immediately;
 * ordinary samples checkpoint once the 30-second threshold is reached. Final
 * flush waits at most 1.5 seconds for the shared lock. No modem command is executed. */
int qdc_traffic_update(QdcTraffic *result, int reset, int flush);
#endif
