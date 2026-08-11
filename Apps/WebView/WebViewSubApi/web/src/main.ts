import { backend, configureBridge, expose } from './generated/backend';

// Nodes are created on arrival so tests never race an empty placeholder.
function publish(id: string, text: string) {
  const node = document.createElement('div');
  node.dataset.testid = id;
  node.textContent = text;
  document.getElementById('app')?.appendChild(node);
}

// Subscribed before the prefix is configured: configureBridge re-points live
// subscriptions, so this one still receives once the prefix changes.
backend.on?.('ticks', (tick) => publish('early-tick-count', String(tick.count)));

// The api is mounted as a sub-API named `nested` (see Types.h), so this client,
// generated with `greet` at the root, needs the prefix to reach "nested.greet".
configureBridge({ prefix: 'nested.' });

// The ordinary order: subscribed after configureBridge, so the prefix is
// applied at subscribe time.
backend.on?.('ticks', (tick) => publish('tick-count', String(tick.count)));

void backend
  .greet({ name: 'world' })
  .then((greeting) => publish('greeting', greeting.text))
  .catch((err: unknown) => publish('greeting', `ERROR ${String(err)}`));

// `expose` is deliberately not prefixed: it names a JS function for C++ to call
// by exact string via WebViewBridge::call, not a routed command.
expose('probeCommand', async (req: { command: string }) => {
  try {
    const reply = await window.eacp!.invoke<{ text: string }>(req.command, {
      name: 'wire',
    });
    return { served: true, text: reply.text };
  } catch {
    return { served: false, text: '' };
  }
});

publish('ready', 'ready');
