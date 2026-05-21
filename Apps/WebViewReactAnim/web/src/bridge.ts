import { useEffect, useRef, useState } from 'react';
import { backend } from './generated/backend';
import type { Tick } from './generated/schema';

export interface NativeTick extends Tick
{
    hz: number;
}

const windowMs = 1000;

export function useNativeTick(): NativeTick
{
    const [tick, setTick] = useState<NativeTick>({ angle: 0, hz: 0 });
    const counter = useRef({ count: 0, since: performance.now() });

    useEffect(() => backend.on?.('tick', (next) =>
    {
        counter.current.count++;
        const elapsed = performance.now() - counter.current.since;

        setTick((prev) =>
        {
            if (elapsed < windowMs)
                return { angle: next.angle, hz: prev.hz };

            const hz = Math.round(counter.current.count * 1000 / elapsed);
            counter.current = { count: 0, since: performance.now() };
            return { angle: next.angle, hz };
        });
    }), []);

    return tick;
}
