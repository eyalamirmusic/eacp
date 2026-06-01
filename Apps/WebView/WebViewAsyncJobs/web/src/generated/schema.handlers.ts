import type * as T from './schema';

export type Handlers = {
    search(req: T.SearchRequest): T.SearchResults | Promise<T.SearchResults>;
    searchBlocking(req: T.SearchRequest): T.SearchResults | Promise<T.SearchResults>;
};

export class UnknownCommandError extends Error
{
    httpStatus = 404;
    constructor(command: string)
    {
        super(`Unknown command: ${command}`);
    }
}

export async function dispatch(handlers: Handlers, command: string, payload: unknown): Promise<unknown>
{
    switch (command)
    {
        case 'search': return await handlers.search(payload as T.SearchRequest);
        case 'searchBlocking': return await handlers.searchBlocking(payload as T.SearchRequest);
        default: throw new UnknownCommandError(command);
    }
}
