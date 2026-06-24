import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
    base: './',
    plugins: [react()],
    build: {
        target: 'es2020',
        assetsInlineLimit: 0,
    },
    server: {
        strictPort: true,
        port: 5174,
    },
});
