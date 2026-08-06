// Generated. Do not edit by hand.
//
// Pre-wired React hooks for every registered bridge event.
// Initial values come from toJSON(T{}).

import { backend, isBackendAvailable } from './backend';
import { makeBridgeStore } from './react';

export const useState = makeBridgeStore({
    backend,
    event: 'state',
    fetch: backend.getState,
    shouldFetch: isBackendAvailable,
    initial: {"heartbeats":0,"pageClicks":0,"ticks":0},
});
