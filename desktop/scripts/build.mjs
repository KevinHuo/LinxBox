#!/usr/bin/env node
/**
 * Build the three runtime targets of LinxBox Desktop:
 *
 *  1. `dist/`          - ESM build of the Electron main process, the daemon,
 *                        the contract and shared layers. `tsc` emits these
 *                        directly; the main process is not sandboxed and can
 *                        use relative ESM imports.
 *  2. `dist-preload/`  - CommonJS bundle of the preload script. A sandboxed
 *                        preload script runs in a restricted loader that
 *                        cannot `require` relative files, so the preload must
 *                        be a single bundled file. The tree carries its own
 *                        `package.json` with `"type":"commonjs"`.
 *  3. `dist/renderer/` - browser bundle of the renderer. The renderer is
 *                        loaded from a `file://` document, where ES module
 *                        scripts are blocked by CORS, so it is bundled into a
 *                        classic script.
 */
import { execFileSync } from 'node:child_process';
import { copyFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { build } from 'esbuild';

const root = dirname(dirname(fileURLToPath(import.meta.url)));
const tsc = join(root, 'node_modules', '.bin', 'tsc');

const compile = (project) => {
  execFileSync(tsc, ['-p', project], { cwd: root, stdio: 'inherit' });
};

compile('tsconfig.build.json');

await build({
  entryPoints: [join(root, 'preload', 'index.ts')],
  outfile: join(root, 'dist-preload', 'preload', 'index.js'),
  bundle: true,
  platform: 'node',
  format: 'cjs',
  target: 'node22',
  external: ['electron'],
  sourcemap: true,
  logLevel: 'warning',
});

await build({
  entryPoints: [join(root, 'renderer', 'app.ts')],
  outfile: join(root, 'dist', 'renderer', 'app.js'),
  bundle: true,
  platform: 'browser',
  format: 'iife',
  target: 'es2023',
  sourcemap: true,
  logLevel: 'warning',
});

await build({
  entryPoints: [join(root, 'renderer', 'float.ts')],
  outfile: join(root, 'dist', 'renderer', 'float.js'),
  bundle: true,
  platform: 'browser',
  format: 'iife',
  target: 'es2023',
  sourcemap: true,
  logLevel: 'warning',
});

const preloadRoot = join(root, 'dist-preload');
mkdirSync(preloadRoot, { recursive: true });
writeFileSync(
  join(preloadRoot, 'package.json'),
  `${JSON.stringify({ type: 'commonjs' }, null, 2)}\n`,
);

const rendererDir = join(root, 'dist', 'renderer');
mkdirSync(rendererDir, { recursive: true });
for (const asset of ['index.html', 'styles.css', 'float.html', 'float.css']) {
  copyFileSync(join(root, 'renderer', asset), join(rendererDir, asset));
}

console.info('build: dist/main, dist-preload/preload, dist/renderer ready');
