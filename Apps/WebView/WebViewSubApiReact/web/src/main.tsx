import { createRoot } from 'react-dom/client';
import App from './App';
import { configureBridge } from './generated/backend';

// The api is mounted as a sub-API named `nested` (see Types.h), so the generated
// client needs the prefix. Still too late for the generated hooks: the `App`
// import is evaluated first and its hooks subscribe at module scope.
configureBridge({ prefix: 'nested.' });

const container = document.getElementById('app');
if (!container) throw new Error('#app container not found');

createRoot(container).render(<App />);
