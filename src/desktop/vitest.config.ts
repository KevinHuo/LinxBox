import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    environment: 'node',
    include: ['**/*.test.ts'],
    exclude: [
      'node_modules/**',
      'dist/**',
      'dist-preload/**',
      'release/**',
      'renderer-prototype/**',
    ],
    coverage: {
      provider: 'v8',
      reporter: ['text', 'lcov'],
      include: [
        'contracts/**/*.ts',
        'daemon/**/*.ts',
        'main/**/*.ts',
        'shared/**/*.ts',
        'renderer/view-model.ts',
        'renderer/fixtures.ts',
      ],
      // `*.wiring.ts` files only wire already-tested pure modules to Electron
      // or Node runtime APIs. They require a live host to exercise and are
      // excluded from the coverage denominator with this reason.
      //
      // `main/index.ts` and `preload/index.ts` are the same kind of file: they
      // only bind Electron's app, dialog, ipcMain and contextBridge APIs to
      // the tested modules, and they cannot run outside an Electron host.
      exclude: ['**/*.wiring.ts', '**/*.test.ts', 'main/index.ts', 'preload/index.ts'],
      thresholds: {
        lines: 90,
        branches: 90,
        functions: 90,
        statements: 90,
      },
    },
  },
});
