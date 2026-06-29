import { homedir, tmpdir } from 'node:os';
import { basename, join } from 'node:path';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';

import { capture, fileExists, requireEnv, requireMacOS, run } from './cli.mjs';

export const remoteDemoKeychainPath = join(
  homedir(),
  'Library',
  'Keychains',
  'tamber-eacp-remote-demo.keychain-db',
);

export const jobBlessKeychainPath = join(
  homedir(),
  'Library',
  'Keychains',
  'tamber-eacp-jobbless-demo.keychain-db',
);

const keychainPassword = 'tamber-eacp-remote-demo-local';

export function ensureTamberSigningIdentity(keychainPath = remoteDemoKeychainPath) {
  requireMacOS('Tamber Developer ID signing');
  requireEnv(
    ['APPLE_SIGNING_IDENTITY'],
    'Tamber Developer ID signing',
  );

  if (!fileExists(keychainPath)) {
    run('security', ['create-keychain', '-p', keychainPassword, keychainPath]);
  }

  run('security', ['unlock-keychain', '-p', keychainPassword, keychainPath]);
  run('security', ['set-keychain-settings', '-lut', '21600', keychainPath]);
  ensureKeychainSearchList(keychainPath);

  if (!hasSigningIdentity(keychainPath)) {
    requireEnv(
      ['APPLE_DEVELOPER_CERTIFICATE_BASE64', 'APPLE_DEVELOPER_CERTIFICATE_PASSWORD'],
      'Tamber Developer ID certificate import',
    );

    const tempDir = mkdtempSync(join(tmpdir(), 'eacp-codesign-'));
    const p12 = join(tempDir, 'cert.p12');
    try {
      writeFileSync(
        p12,
        Buffer.from(process.env.APPLE_DEVELOPER_CERTIFICATE_BASE64, 'base64'),
        { mode: 0o600 },
      );
      run('security', [
        'import',
        p12,
        '-k',
        keychainPath,
        '-P',
        process.env.APPLE_DEVELOPER_CERTIFICATE_PASSWORD,
        '-T',
        '/usr/bin/codesign',
        '-T',
        '/usr/bin/security',
      ]);
    } finally {
      rmSync(tempDir, { recursive: true, force: true });
    }
  }

  run('security', [
    'set-key-partition-list',
    '-S',
    'apple-tool:,apple:,codesign:',
    '-s',
    '-k',
    keychainPassword,
    keychainPath,
  ]);

  if (!hasSigningIdentity(keychainPath)) {
    throw new Error(`Signing identity not found after import: ${process.env.APPLE_SIGNING_IDENTITY}`);
  }
}

export function signPath(path, keychainPath = remoteDemoKeychainPath) {
  const timestampArgs = process.env.APPLE_CODESIGN_TIMESTAMP === '1'
    ? ['--timestamp']
    : [];

  run('codesign', [
    '--force',
    ...timestampArgs,
    '--options',
    'runtime',
    '--keychain',
    keychainPath,
    '--sign',
    process.env.APPLE_SIGNING_IDENTITY,
    path,
  ]);
  verifyCodeSignature(path);
}

export function verifyCodeSignature(path) {
  run('codesign', ['--verify', '--strict', '--verbose=2', path]);
}

export function verifyAppHubPrivilegedHelper(
  appBundle,
  helperLabel = 'com.tamber.AppHub.PrivilegedHelper',
) {
  verifyCodeSignature(join(
    appBundle,
    'Contents',
    'Library',
    'LaunchServices',
    helperLabel,
  ));
}

export function verifyMachODeploymentTargetAtMost(path, maximumVersion) {
  const result = capture('otool', ['-l', path]);
  const match = result.stdout.match(/^\s*minos\s+([0-9]+(?:\.[0-9]+)*)$/m);
  if (!match) {
    throw new Error(`Could not read Mach-O deployment target for ${path}`);
  }

  if (compareVersions(match[1], maximumVersion) > 0) {
    throw new Error(
      `${basename(path)} targets macOS ${match[1]}, expected ${maximumVersion} or older`,
    );
  }
}

export function verifyAppHubDeploymentTarget(
  appBundle,
  maximumVersion,
  helperLabel = 'com.tamber.AppHub.PrivilegedHelper',
) {
  verifyMachODeploymentTargetAtMost(
    join(appBundle, 'Contents', 'MacOS', 'AppHub'),
    maximumVersion,
  );
  verifyMachODeploymentTargetAtMost(
    join(appBundle, 'Contents', 'Library', 'LaunchServices', helperLabel),
    maximumVersion,
  );
}

export function adHocSignPath(path) {
  run('codesign', ['--force', '--deep', '--sign', '-', path]);
}

function hasSigningIdentity(keychainPath) {
  const result = capture('security', ['find-identity', '-v', '-p', 'codesigning', keychainPath], {
    check: false,
  });
  return result.stdout.includes(`"${process.env.APPLE_SIGNING_IDENTITY}"`);
}

function compareVersions(left, right) {
  const l = left.split('.').map((part) => Number.parseInt(part, 10));
  const r = right.split('.').map((part) => Number.parseInt(part, 10));
  const count = Math.max(l.length, r.length);
  for (let index = 0; index < count; index += 1) {
    const lv = l[index] ?? 0;
    const rv = r[index] ?? 0;
    if (lv < rv) return -1;
    if (lv > rv) return 1;
  }
  return 0;
}

function ensureKeychainSearchList(keychainPath) {
  const listed = capture('security', ['list-keychains', '-d', 'user']);
  const current = listed.stdout
    .split('\n')
    .map((line) => line.trim().replace(/^"|"$/g, ''))
    .filter(Boolean);

  if (current.includes(keychainPath)) {
    return;
  }

  run('security', ['list-keychains', '-d', 'user', '-s', ...current, keychainPath]);
}
