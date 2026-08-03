// Generated. Do not edit by hand.
//
// Pre-wired React hooks for every registered bridge event.
// Initial values come from toJSON(T{}).

import { backend, isBackendAvailable } from './backend';
import { makeBridgeStore, makeNativeEvent } from './react';

export const useCounter = makeBridgeStore({
    backend,
    event: 'counter',
    fetch: backend.getCounter,
    shouldFetch: isBackendAvailable,
    initial: {"value":0},
});

export const usePulse = makeNativeEvent({
    backend,
    event: 'pulse',
    initial: {"beat":0},
});
