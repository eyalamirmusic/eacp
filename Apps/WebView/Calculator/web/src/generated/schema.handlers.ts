import type * as T from './schema';

export type Handlers = {
    getCalculator(): T.CalculatorState | Promise<T.CalculatorState>;
    press(req: T.KeyRequest): void | Promise<void>;
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
        case 'getCalculator': return await handlers.getCalculator();
        case 'press': return await handlers.press(payload as T.KeyRequest);
        default: throw new UnknownCommandError(command);
    }
}
