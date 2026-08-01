import { useCounter, usePulse } from './generated/hooks';

// Both hooks come straight from the generated module — no hand-written
// subscription anywhere in this app, so what the tests observe is what eacp's
// codegen produced.
//
// data-value mirrors the rendered number so a test can wait for a SPECIFIC
// value. Both nodes exist from the first paint, so gating on the testid alone
// would return before the value landed and let the following read see the
// placeholder.
export default function App()
{
    const counter = useCounter();
    const pulse = usePulse();

    return (
        <main>
            <div data-testid="ready">ready</div>

            <div data-testid="counter" data-value={counter.value}>
                {counter.value}
            </div>

            <div data-testid="pulse" data-value={pulse.beat}>
                {pulse.beat}
            </div>
        </main>
    );
}
