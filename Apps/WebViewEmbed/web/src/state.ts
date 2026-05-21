import { backend } from './generated/backend';
import { makeBridgeStore } from './generated/react';
import type { Parameters } from './generated/schema';

export const useParameters = makeBridgeStore({
    backend,
    event: 'parameters',
    fetch: backend.getParameters,
    initial: { level: 0.5, autoCycle: false, counter: 0 },
});

export const setParameters = (next: Parameters) => backend.setParameters(next);
