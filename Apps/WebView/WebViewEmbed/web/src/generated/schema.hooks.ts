// Generated. Do not edit by hand.
//
// Pre-wired React hooks for every registered bridge event.
// Initial values come from toJSON(T{}).

import { backend, isBackendAvailable } from './backend';
import { makeBridgeStore } from './react';

export const useParameters = makeBridgeStore({
    backend,
    event: 'parameters',
    fetch: backend.getParameters,
    shouldFetch: isBackendAvailable,
    initial: {"autoCycle":false,"counter":0,"level":0.5},
});
