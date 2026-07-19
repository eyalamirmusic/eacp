import type * as T from './schema';

export type Invoke = (command: string, payload: unknown) => Promise<unknown>;

export function makeBackend(invoke: Invoke)
{
    return {
        greet: (req: T.GreetRequest): Promise<T.Greeting> =>
            invoke('greet', req) as Promise<T.Greeting>,
    };
}
