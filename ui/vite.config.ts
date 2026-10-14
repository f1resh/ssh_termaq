import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';
import { fileURLToPath } from 'node:url';

const r = (p: string) => fileURLToPath(new URL(p, import.meta.url));

export default defineConfig({
  plugins: [react()],
  // Assets are served by the native host from https://app.sshterm.local/ (WebView2 virtual host mapping).
  base: './',
  resolve: {
    alias: {
      '@': r('./src'),
      '@shared': r('./src/shared')
    }
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    target: 'chrome120',
    sourcemap: true,
    chunkSizeWarningLimit: 2000
  },
  server: {
    port: 5173,
    strictPort: true
  },
  test: {
    environment: 'node',
    include: ['src/**/*.test.ts', 'tests/**/*.test.ts']
  }
});
