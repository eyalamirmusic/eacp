import type * as T from './schema';

export type Handlers = {
    ping(): T.PingResponse | Promise<T.PingResponse>;
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
        case 'ping': return await handlers.ping();
        default: throw new UnknownCommandError(command);
    }
}
