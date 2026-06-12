import { useEffect } from 'react';
import { backend } from './generated/backend';
import { useCalculator } from './generated/hooks';

// Every key the pad shows. `key` is the wire value CalculatorApi
// understands; `id` becomes the data-testid (key-<id>) so tests and
// agents have stable handles.
const KEYS: { key: string; label: string; id: string; kind?: 'fn' | 'op' | 'zero' }[] = [
    { key: 'C', label: 'C', id: 'clear', kind: 'fn' },
    { key: '±', label: '±', id: 'negate', kind: 'fn' },
    { key: '%', label: '%', id: 'percent', kind: 'fn' },
    { key: '/', label: '÷', id: 'divide', kind: 'op' },
    { key: '7', label: '7', id: '7' },
    { key: '8', label: '8', id: '8' },
    { key: '9', label: '9', id: '9' },
    { key: '*', label: '×', id: 'multiply', kind: 'op' },
    { key: '4', label: '4', id: '4' },
    { key: '5', label: '5', id: '5' },
    { key: '6', label: '6', id: '6' },
    { key: '-', label: '−', id: 'minus', kind: 'op' },
    { key: '1', label: '1', id: '1' },
    { key: '2', label: '2', id: '2' },
    { key: '3', label: '3', id: '3' },
    { key: '+', label: '+', id: 'plus', kind: 'op' },
    { key: '0', label: '0', id: '0', kind: 'zero' },
    { key: '.', label: '.', id: 'decimal' },
    { key: '=', label: '=', id: 'equals', kind: 'op' },
];

const KEYBOARD: Record<string, string> = {
    Enter: '=', '=': '=', Escape: 'C', Backspace: '⌫',
    '+': '+', '-': '-', '*': '*', '/': '/', '.': '.', '%': '%',
    ...Object.fromEntries([...'0123456789'].map((d) => [d, d])),
};

export default function App()
{
    const state = useCalculator();
    const press = (key: string) => void backend.press({ key });

    useEffect(() =>
    {
        const onKeyDown = (event: KeyboardEvent) =>
        {
            const key = KEYBOARD[event.key];
            if (!key) return;
            event.preventDefault();
            press(key);
        };
        window.addEventListener('keydown', onKeyDown);
        return () => window.removeEventListener('keydown', onKeyDown);
    }, []);

    return (
        <main className="calc" data-testid="calculator">
            <section className="screen" onMouseDown={(e) => e.preventDefault()}>
                <div className="expression" data-testid="calc-expression">
                    {state.expression || ' '}
                </div>
                <div
                    className={state.error ? 'display error' : 'display'}
                    data-testid="calc-display"
                >
                    {state.display}
                </div>
            </section>
            <section className="keys">
                {KEYS.map((k) => (
                    <button
                        key={k.id}
                        data-testid={`key-${k.id}`}
                        className={`key ${k.kind ?? ''}`}
                        type="button"
                        onClick={() => press(k.key)}
                    >
                        {k.label}
                    </button>
                ))}
            </section>
        </main>
    );
}
