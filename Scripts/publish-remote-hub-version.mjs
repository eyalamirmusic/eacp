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

const version = env('VERSION', '2.0.0');
const releaseTag = env('RELEASE_TAG', 'remote-demo-v1');
const releaseBaseUrl = env(
  'RELEASE_BASE_URL',
  `https://github.com/Tamber-Inc/eacp/releases/download/${releaseTag}`,
);
const outDir = env('OUT_DIR', join(repoRoot, 'dist', 'remote-hub-update'));
const buildDir = env('BUILD_DIR', join(repoRoot, `build-remote-hub-update-${version}`));

const appHubAppName = 'AppHub.app';
const appHubBinaryName = 'AppHub';
const appHubZip = `AppHub-${version}.app.zip`;
const productId = 'com.tamber.AppHub';

requireMacOS('Remote AppHub update publishing');

log('Import Tamber Developer ID signing identity');
ensureTamberSigningIdentity();

log(`Configure AppHub ${version}`);
run('cmake', [
  '-S',
  repoRoot,
  '-B',
  buildDir,
  '-DCMAKE_BUILD_TYPE=Release',
  `-DEACP_APPHUB_VERSION=${version}`,
]);

log(`Build AppHub ${version}`);
run('cmake', ['--build', buildDir, '--target', 'AppHub']);

const appHubApp = join(buildDir, 'Apps', 'System', 'AppHub', appHubAppName);
const appHubHelper = join(
  appHubApp,
  'Contents',
  'Library',
  'LaunchServices',
  'com.tamber.AppHub.PrivilegedHelper',
);

log(`Sign AppHub ${version}`);
signPath(appHubHelper);
signPath(appHubApp);
verifyAppHubPrivilegedHelper(appHubApp);

log('Verify AppHub version');
run(join(appHubApp, 'Contents', 'MacOS', appHubBinaryName), ['--version']);

log(`Package AppHub ${version}`);
cleanDir(outDir);
run('ditto', ['-c', '-k', '--keepParent', appHubApp, join(outDir, appHubZip)]);

log(`Verify packaged AppHub ${version}`);
const packagedVerifyDir = join(buildDir, 'packaged-apphub-verify');
cleanDir(packagedVerifyDir);
run('ditto', ['-x', '-k', join(outDir, appHubZip), packagedVerifyDir]);
const packagedAppHub = join(packagedVerifyDir, appHubAppName);
verifyCodeSignature(packagedAppHub);
verifyAppHubPrivilegedHelper(packagedAppHub);

const appHubSha = sha256File(join(outDir, appHubZip));
const manifest = {
  productId,
  name: appHubBinaryName,
  version,
  bundleName: appHubAppName,
  artifact: {
    url: `${releaseBaseUrl}/${appHubZip}`,
    sha256: appHubSha,
  },
};
writeJson(join(outDir, 'hub-manifest.json'), manifest);

log('Update release Hub manifest and app artifact');
run('gh', [
  'release',
  'upload',
  releaseTag,
  join(outDir, appHubZip),
  join(outDir, 'hub-manifest.json'),
  '--repo',
  'Tamber-Inc/eacp',
  '--clobber',
]);

log(`Published AppHub ${version}`);
console.log(JSON.stringify(manifest, null, 2));
