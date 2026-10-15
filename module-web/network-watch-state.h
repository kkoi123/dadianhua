/* Pure transition logic shared by the Linux watcher and host tests. */
#ifndef QDC_NETWORK_WATCH_STATE_H
#define QDC_NETWORK_WATCH_STATE_H
#include <stdio.h>
#include <string.h>
struct network_state { int initialized, lost; char previous[512]; };
static int network_observe(struct network_state *state, const char *signature, int lost_event) {
    if (!state->initialized) {
        state->initialized=1;
        snprintf(state->previous,sizeof state->previous,"%s",signature);
        state->lost=!signature[0] || lost_event;
        return signature[0] && lost_event;
    }
    if (lost_event || !signature[0]) state->lost=1;
    return signature[0] && (state->lost || strcmp(state->previous,signature));
}
static void network_ack(struct network_state *state, const char *signature) {
    snprintf(state->previous,sizeof state->previous,"%s",signature);
    state->lost=0;
}
#endif
