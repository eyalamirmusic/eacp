// Generated. Do not edit by hand.
//
// Pre-wired React hooks for every registered bridge event.
// Keyed states get useXxx / useXxxIds / useXxxItem; plain
// states get useXxx; push-only events get useXxx via
// makeNativeEvent. Initial values come from toJSON(T{}).

import { backend, isBackendAvailable } from './backend';
import { makeBridgeStore } from './react';

export const useCalculator = makeBridgeStore({
    backend,
    event: 'calculator',
    fetch: backend.getCalculator,
    shouldFetch: isBackendAvailable,
    initial: {"display":"0","error":false,"expression":""},
});
