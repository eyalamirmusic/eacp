import { makeBackend } from './schema.backend';

interface EacpBridge
{
    invoke<Res = unknown, Req = unknown>(command: string, payload?: Req): Promise<Res>;
    on<T = unknown>(event: string, handler: (payload: T) => void): () => void;
}

declare global
{
    interface Window
    {
        eacp: EacpBridge;
    }
}

export const backend = makeBackend((command, payload) =>
    window.eacp.invoke(command, payload));
