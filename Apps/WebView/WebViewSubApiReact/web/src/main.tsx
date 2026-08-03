import { createRoot } from 'react-dom/client';
import App from './App';
import { configureBridge } from './generated/backend';

// This app's api is mounted as a sub-API named `nested` (see Types.h), so the
// generated client needs the prefix before it talks to anything — exactly as
// in WebViewSubApi's main.ts.
//
// It is placed where a page would naturally put it: first statement of the
// entry module, before a single component is rendered. Nothing here is
// deferred or conditional, and the page never subscribes by hand.
//
// It is nonetheless too late for the generated hooks, which is what the tests
// assert. `import App from './App'` above is hoisted and fully evaluated
// before this line runs, and App.tsx imports the generated hooks module —
// whose makeBridgeStore call subscribes in its own body, at module scope.
configureBridge({ prefix: 'nested.' });

const container = document.getElementById('app');
if (!container) throw new Error('#app container not found');

createRoot(container).render(<App />);
