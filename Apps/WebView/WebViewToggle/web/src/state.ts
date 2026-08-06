import { backend } from './generated/backend';
import { makeBridgeStore } from './generated/react';

export const usePanelState = makeBridgeStore({
    backend,
    event: 'state',
    fetch: backend.getState,
    initial: { ticks: 0, pageClicks: 0, heartbeats: 0 },
});
