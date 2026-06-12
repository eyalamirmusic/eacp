import type * as T from './schema';

export type Invoke = (command: string, payload: unknown) => Promise<unknown>;

export function makeBackend(invoke: Invoke)
{
    return {
        getCalculator: (): Promise<T.CalculatorState> =>
            invoke('getCalculator', {}) as Promise<T.CalculatorState>,
        press: (req: T.KeyRequest): Promise<void> =>
            invoke('press', req) as Promise<void>,
    };
}
