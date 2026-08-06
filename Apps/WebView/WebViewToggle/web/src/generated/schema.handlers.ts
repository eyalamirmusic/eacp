import type * as T from './schema';

export type Handlers = {
    getState(): T.PanelState | Promise<T.PanelState>;
    pingFromPage(): void | Promise<void>;
    heartbeat(): void | Promise<void>;
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
        case 'getState': return await handlers.getState();
        case 'pingFromPage': return await handlers.pingFromPage();
        case 'heartbeat': return await handlers.heartbeat();
        default: throw new UnknownCommandError(command);
    }
}
