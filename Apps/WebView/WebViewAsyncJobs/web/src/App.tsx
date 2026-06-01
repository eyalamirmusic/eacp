import { useEffect, useRef, useState } from 'react';
import { backend } from './generated/backend';

interface Hit
{
    title: string;
    score: number;
}

type SearchFn = (req: { query: string }) => Promise<{ query: string; hits: Hit[] }>;

// Tiny debounce so nearly every keystroke fires a search — that's when the
// blocking command's serialisation becomes obvious.
const DEBOUNCE_MS = 10;

function SearchPanel(props: {
    title: string;
    subtitle: string;
    run: SearchFn;
    testid: string;
})
{
    const [query, setQuery] = useState('');
    const [hits, setHits] = useState<Hit[]>([]);
    const [lastMs, setLastMs] = useState<number | null>(null);
    const [peakMs, setPeakMs] = useState(0);
    const [inFlight, setInFlight] = useState(0);

    // Only the most recent request's results are applied (out-of-order guard).
    const latest = useRef(0);

    useEffect(() =>
    {
        const q = query.trim();
        if (q === '')
        {
            setHits([]);
            setLastMs(null);
            return;
        }

        const timer = setTimeout(() =>
        {
            const id = ++latest.current;
            const started = performance.now();
            setInFlight((n) => n + 1);

            void props.run({ query: q }).then((res) =>
            {
                const ms = Math.round(performance.now() - started);
                setInFlight((n) => n - 1);
                if (id !== latest.current) return; // superseded by a newer keystroke
                setHits(res.hits);
                setLastMs(ms);
                setPeakMs((m) => Math.max(m, ms));
            });
        }, DEBOUNCE_MS);

        return () => clearTimeout(timer);
    }, [query, props]);

    return (
        <div className="col" data-testid={props.testid}>
            <h2>{props.title}</h2>
            <p className="colsub">{props.subtitle}</p>

            <input
                data-testid="search-input"
                type="text"
                placeholder="Search…"
                value={query}
                onChange={(event) => setQuery(event.target.value)}
            />

            <div className="status" data-testid="search-status">
                {lastMs == null
                    ? 'Type to search'
                    : <>resolved in <strong>{lastMs}ms</strong></>}
                {inFlight > 0 && <span className="inflight"> · {inFlight} in flight</span>}
                {peakMs > 0 && <span className="peak"> · peak {peakMs}ms</span>}
            </div>

            <ul className="list">
                {hits.map((hit, index) => (
                    <li key={index} data-testid="search-hit" className="item">
                        <span className="text">{hit.title}</span>
                        <span className="badge done">{hit.score.toFixed(2)}</span>
                    </li>
                ))}
            </ul>
        </div>
    );
}

export default function App()
{
    return (
        <main className="wide">
            <header>
                <h1>async vs blocking</h1>
                <p className="sub">
                    Two realtime searches doing the <em>same</em> 200ms of work.
                    Left calls a C++ command returning{' '}
                    <code>Async&lt;SearchResults&gt;</code> (the wait happens
                    off-thread); right calls a plain synchronous command that{' '}
                    <code>sleep</code>s on the main thread. Type quickly in both —
                    async stays pinned near 200ms, while blocking's latency climbs
                    as calls pile up behind the frozen event loop.
                </p>
            </header>

            <div className="cols">
                <SearchPanel
                    testid="async-panel"
                    title="Async"
                    subtitle="Async<SearchResults> — co_awaits, never blocks the loop"
                    run={backend.search}
                />
                <SearchPanel
                    testid="blocking-panel"
                    title="Blocking (sync)"
                    subtitle="returns a value after sleeping 200ms on the main thread"
                    run={backend.searchBlocking}
                />
            </div>
        </main>
    );
}
