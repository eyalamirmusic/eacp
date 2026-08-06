import { createRoot } from 'react-dom/client';
import App from './App';
import './style.css';

const container = document.getElementById('app');
if (!container) throw new Error('#app container not found');

createRoot(container).render(<App />);
