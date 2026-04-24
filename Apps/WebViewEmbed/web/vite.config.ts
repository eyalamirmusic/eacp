import { defineConfig } from 'vite';

export default defineConfig({
    base: './',
    build: {
        target: 'es2020',
        assetsInlineLimit: 0,
        rollupOptions: {
            output: {
                entryFileNames: '[name]-[hash].js',
                chunkFileNames: '[name]-[hash].js',
                assetFileNames: '[name]-[hash].[ext]',
            },
        },
    },
    server: {
        strictPort: true,
        port: 5173,
    },
});
