import type * as T from './schema';

export type Handlers = {
    greet(req: T.GreetRequest): T.Greeting | Promise<T.Greeting>;
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
        case 'greet': return await handlers.greet(payload as T.GreetRequest);
        default: throw new UnknownCommandError(command);
    }
}
