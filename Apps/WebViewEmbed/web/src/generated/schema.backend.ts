import type * as T from './schema';

export type Invoke = (command: string, payload: unknown) => Promise<unknown>;

export function makeBackend(invoke: Invoke)
{
    return {
        ping: (): Promise<T.PingResponse> =>
            invoke('ping', {}) as Promise<T.PingResponse>,
    };
}
