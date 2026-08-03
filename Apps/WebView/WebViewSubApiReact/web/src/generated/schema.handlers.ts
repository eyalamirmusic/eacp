import type * as T from './schema';

export type Handlers = {
    getCounter(): T.Counter | Promise<T.Counter>;
};

export class UnknownCommandError extends Error
{
    httpStatus = 404;
    constructor(command: string)
    {
        super(`Unknown command: ${command}`);
    }
}

export async function dispatch(handlers: Handlers, command: string, _payload: unknown): Promise<unknown>
{
    switch (command)
    {
        case 'getCounter': return await handlers.getCounter();
        default: throw new UnknownCommandError(command);
    }
}
