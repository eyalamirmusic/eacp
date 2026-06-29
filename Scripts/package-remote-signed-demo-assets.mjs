#!/usr/bin/env node

import { join } from 'node:path';

import {
  cleanDir,
  env,
  log,
  repoRoot,
  requireMacOS,
  run,
  sha256File,
  writeJson,
} from './lib/cli.mjs';
import {
  ensureTamberSigningIdentity,
  signPath,
  verifyAppHubPrivilegedHelper,
  verifyCodeSignature,
} from './lib/macos-signing.mjs';

const version = env('VERSION', '1.0.0');
const releaseTag = env('RELEASE_TAG', `remote-demo-v${version}`);
const releaseBaseUrl = env(
  'RELEASE_BASE_URL',
  `https://github.com/Tamber-Inc/eacp/releases/download/${releaseTag}`,
);
const outDir = env('OUT_DIR', join(repoRoot, 'dist', 'remote-signed-demo'));
const buildDir = env('BUILD_DIR', join(repoRoot, 'build-remote-signed-demo'));

const appHubZip = 'AppHub-remote-demo.app.zip';
const demoZip = `TamberLocalUpdateDemo-${version}.app.zip`;
const demoAppName = 'Tamber Local Update Demo.app';
const demoBinaryName = 'Tamber Local Update Demo';
const productId = 'com.tamber.RealUpdateDemo';

requireMacOS('Remote signed demo packaging');

log('Import Tamber Developer ID signing identity');
ensureTamberSigningIdentity();

log('Configure release build');
run('cmake', [
  '-S',
  repoRoot,
  '-B',
  buildDir,
  '-DCMAKE_BUILD_TYPE=Release',
  `-DEACP_REAL_UPDATE_DEMO_VERSION=${version}`,
]);

log('Build signed-demo targets');
run('cmake', ['--build', buildDir, '--target', 'AppHub', 'RealUpdateDemo']);

const appHubApp = join(buildDir, 'Apps', 'System', 'AppHub', 'AppHub.app');
const appHubHelper = join(
  appHubApp,
  'Contents',
  'Library',
  'LaunchServices',
  'com.tamber.AppHub.PrivilegedHelper',
);
const demoApp = join(buildDir, 'Apps', 'System', 'RealUpdateDemo', demoAppName);

log('Sign AppHub helper and app');
signPath(appHubHelper);
signPath(appHubApp);
verifyAppHubPrivilegedHelper(appHubApp);

log('Sign Demo App');
signPath(demoApp);

log('Verify Demo App version');
run(join(demoApp, 'Contents', 'MacOS', demoBinaryName), ['--version']);

log('Package release assets');
cleanDir(outDir);
run('ditto', ['-c', '-k', '--keepParent', appHubApp, join(outDir, appHubZip)]);
run('ditto', ['-c', '-k', '--keepParent', demoApp, join(outDir, demoZip)]);

log('Verify packaged AppHub artifact');
const packagedVerifyDir = join(buildDir, 'packaged-apphub-verify');
cleanDir(packagedVerifyDir);
run('ditto', ['-x', '-k', join(outDir, appHubZip), packagedVerifyDir]);
const packagedAppHub = join(packagedVerifyDir, 'AppHub.app');
verifyCodeSignature(packagedAppHub);
verifyAppHubPrivilegedHelper(packagedAppHub);

const demoSha = sha256File(join(outDir, demoZip));
const manifest = {
  productId,
  name: 'Tamber Local Update Demo',
  version,
  bundleName: demoAppName,
  artifact: {
    url: `${releaseBaseUrl}/${demoZip}`,
    sha256: demoSha,
  },
};
writeJson(join(outDir, 'manifest.json'), manifest);

log('Release assets');
run('ls', ['-lh', outDir]);
console.log(JSON.stringify(manifest, null, 2));
