import type * as T from './schema';

export type Invoke = (command: string, payload: unknown) => Promise<unknown>;

export function makeBackend(invoke: Invoke)
{
    return {
        getCounter: (): Promise<T.Counter> =>
            invoke('getCounter', {}) as Promise<T.Counter>,
    };
}
