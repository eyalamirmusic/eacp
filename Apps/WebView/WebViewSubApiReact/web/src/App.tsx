import { useCounter, usePulse } from './generated/hooks';

// data-value mirrors the rendered number so a test can wait for a specific
// value: both nodes already exist at first paint, so the testid alone is no gate.
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
