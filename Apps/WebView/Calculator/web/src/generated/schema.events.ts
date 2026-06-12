import type * as T from './schema';

export interface Events
{
    'calculator': T.CalculatorState;
}

export type EventName = keyof Events;

export interface EventBus
{
    subscribe<K extends EventName>(
        name: K,
        handler: (payload: Events[K]) => void,
    ): () => void;
}
