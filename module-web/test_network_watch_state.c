#include <assert.h>
#include <stdio.h>
#include "network-watch-state.h"
int main(void) {
    struct network_state s={0};
    assert(!network_observe(&s,"4|10.1.1.1|g1",0)); /* No restart at startup. */
    for(int i=0;i<100;i++)assert(!network_observe(&s,"4|10.1.1.1|g1",0));
    assert(network_observe(&s,"4|10.1.1.2|g1",0)); /* Address change. */
    assert(network_observe(&s,"4|10.1.1.2|g1",0)); /* Busy service must retain pending change. */
    network_ack(&s,"4|10.1.1.2|g1");
    assert(!network_observe(&s,"4|10.1.1.2|g1",0));
    assert(!network_observe(&s,"",0)); /* Down: do not restart until usable. */
    assert(network_observe(&s,"4|10.1.1.2|g1",0)); /* Same IP recovery. */
    network_ack(&s,"4|10.1.1.2|g1");
    assert(network_observe(&s,"4|10.1.1.2|g1",1)); /* Brief down/up within debounce. */
    network_ack(&s,"4|10.1.1.2|g1");
    assert(network_observe(&s,"5|10.1.1.2|g1",0)); /* Recreated interface. */
    network_ack(&s,"5|10.1.1.2|g1");
    assert(network_observe(&s,"5|10.1.1.2|g2",0)); /* Gateway change. */
    struct network_state boot={0};
    assert(!network_observe(&boot,"",0));
    assert(network_observe(&boot,"4|10.1.1.1|g1",0));
    puts("Network transitions passed: stable/startup, address/gateway change, same-IP recovery, brief outage, deferred request.");
    return 0;
}
