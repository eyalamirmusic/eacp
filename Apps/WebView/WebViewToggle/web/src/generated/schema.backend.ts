import type * as T from './schema';

export type Invoke = (command: string, payload: unknown) => Promise<unknown>;

export function makeBackend(invoke: Invoke)
{
    return {
        getState: (): Promise<T.PanelState> =>
            invoke('getState', {}) as Promise<T.PanelState>,
        pingFromPage: (): Promise<void> =>
            invoke('pingFromPage', {}) as Promise<void>,
        heartbeat: (): Promise<void> =>
            invoke('heartbeat', {}) as Promise<void>,
    };
}
