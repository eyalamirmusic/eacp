// Drives the generated client with a namespace prefix configured.
//
// Deliberately plain TS rather than React: the assertions are about the
// transport in the generated backend.ts (which command name goes out, which
// event name is subscribed), so a component layer between the test and
// backend.invoke / backend.on would only add noise.

import { backend, configureBridge, expose } from './generated/backend';

// This app's api is mounted as a sub-API named `nested` (see Types.h), so its
// commands reach the wire as "nested.greet". This client was generated with
// `greet` at the root, so without this line every call would miss.
//
// Called BEFORE the first subscribe / invoke below — the only ordering this
// feature requires.
configureBridge({ prefix: 'nested.' });

// Results are CREATED on arrival rather than pre-rendered and filled, so the
// test's waitFor(selector) gates on the value actually landing instead of
// racing an empty placeholder.
function publish(id: string, text: string) {
  const node = document.createElement('div');
  node.dataset.testid = id;
  node.textContent = text;
  document.getElementById('app')?.appendChild(node);
}

// Subscribed after configureBridge ran but before anything is published. The
// generated client builds its transport at module load and resolves the prefix
// here, at subscribe time — which is what makes the ordering above sufficient.
backend.on?.('ticks', (tick) => publish('tick-count', String(tick.count)));

// The invoke path. A reply proves the command routed to "nested.greet".
void backend
  .greet({ name: 'world' })
  .then((greeting) => publish('greeting', greeting.text))
  .catch((err: unknown) => publish('greeting', `ERROR ${String(err)}`));

// Lets a test ask what a RAW command name resolves to, bypassing the generated
// client, so the wire shape itself can be asserted.
//
// Registered through the generated `expose`, which the template deliberately
// does NOT prefix: it names a JS function for C++ to call by exact string via
// WebViewBridge::call, not a routed command. Were it prefixed, the C++ side
// calling "probeCommand" would never find this.
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

// Booted marker, published last and unconditionally, so a test that never sees
// its result fails on a missing value rather than on page load.
publish('ready', 'ready');
