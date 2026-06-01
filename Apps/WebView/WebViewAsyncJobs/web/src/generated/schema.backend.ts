import type * as T from './schema';

export type Invoke = (command: string, payload: unknown) => Promise<unknown>;

export function makeBackend(invoke: Invoke)
{
    return {
        search: (req: T.SearchRequest): Promise<T.SearchResults> =>
            invoke('search', req) as Promise<T.SearchResults>,
        searchBlocking: (req: T.SearchRequest): Promise<T.SearchResults> =>
            invoke('searchBlocking', req) as Promise<T.SearchResults>,
    };
}
