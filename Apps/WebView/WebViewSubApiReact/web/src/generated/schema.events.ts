import type * as T from './schema';

export interface Events
{
    'counter': T.Counter;
    'pulse': T.Pulse;
}

export type EventName = keyof Events;

export interface EventBus
{
    subscribe<K extends EventName>(
        name: K,
        handler: (payload: Events[K]) => void,
    ): () => void;
}
