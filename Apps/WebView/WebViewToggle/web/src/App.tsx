import { useEffect } from 'react';
import { backend } from './generated/backend';
import { usePanelState } from './state';

export default function App()
{
    const state = usePanelState();

    // Constant page -> native traffic, so destroying the WebView always
    // happens with commands in flight rather than in a convenient lull.
    useEffect(() =>
    {
        const id = window.setInterval(() => void backend.heartbeat(), 50);
        return () => window.clearInterval(id);
    }, []);

    return (
        <main>
            <h1>Dynamic WebView</h1>
            <p>
                This page — React root, bridge and web process — was created when
                the native button opened the WebView, and is destroyed with it.
                Reopen and every counter starts from zero.
            </p>

            <div className="stats">
                <div className="stat">
                    <span className="value" data-testid="ticks">{state.ticks}</span>
                    <span className="label">native ticks (30 Hz timer)</span>
                </div>
                <div className="stat">
                    <span className="value" data-testid="heartbeats">{state.heartbeats}</span>
                    <span className="label">page heartbeats (every 50 ms)</span>
                </div>
                <div className="stat">
                    <span className="value" data-testid="page-clicks">{state.pageClicks}</span>
                    <span className="label">pings received by native</span>
                </div>
            </div>

            <button data-testid="ping" onClick={() => void backend.pingFromPage()}>
                Ping native
            </button>
        </main>
    );
}
