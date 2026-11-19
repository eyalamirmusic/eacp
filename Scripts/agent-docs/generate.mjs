#!/usr/bin/env node
// Usage: node generate.mjs [--config agent-docs.toml] [--out build/agent-docs] [--ref <name>] [--check | --print-slug]
import { copyFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { parseArgs } from 'node:util';
import {
    buildModuleTree,
    getPathsToRead,
    groupTreeIntoModules,
    listTree,
    readTexts,
    renderModuleReadmes,
    renderTopIndex,
    resolveBranchName,
    resolveCommit,
    topIndexPaths,
} from './agent-docs.mjs';
import { readConfig } from './config.mjs';

const { values } = parseArgs({
    options: {
        config: { type: 'string', default: 'agent-docs.toml' },
        out: { type: 'string', default: 'build/agent-docs' },
        ref: { type: 'string' },
        check: { type: 'boolean', default: false },
        'print-slug': { type: 'boolean', default: false },
    },
});

const config = readConfig(values.config);
if (values['print-slug']) {
    console.log(config.slug);
    process.exit(0);
}

const checkout = resolve(dirname(values.config));
const modules = groupTreeIntoModules(listTree(checkout), config);
const readTextByPath = readTexts(checkout, getPathsToRead(modules));
const nodesByDirectory = buildModuleTree(modules);
const readmes = renderModuleReadmes(nodesByDirectory, readTextByPath);
const stalePaths = [...readmes].filter(([path, text]) => readTextByPath.get(path) !== text).map(([path]) => path);

if (values.check) {
    if (stalePaths.length > 0) {
        console.error(`Stale module READMEs; run generate.mjs without --check to rewrite them:\n${stalePaths.join('\n')}`);
        process.exit(1);
    }
    console.log(`All ${readmes.size} module READMEs are current.`);
    process.exit(0);
}

for (const path of stalePaths) writeFileSync(join(checkout, path), readmes.get(path));

const outputDirectory = resolve(checkout, values.out);
const writeOutput = (path, write) => {
    mkdirSync(dirname(join(outputDirectory, path)), { recursive: true });
    write(join(outputDirectory, path));
};
if (outputDirectory !== checkout) {
    const publishedPaths = [...readmes.keys(), ...modules.flatMap((module) => module.headerPaths)];
    for (const path of publishedPaths) writeOutput(path, (target) => copyFileSync(join(checkout, path), target));
}

const reference = values.ref ?? resolveBranchName(checkout);
const updatedTextByPath = new Map([...readTextByPath, ...readmes]);
const topIndex = renderTopIndex({ config, sha: resolveCommit(checkout), reference, nodesByDirectory, readTextByPath: updatedTextByPath });
for (const path of topIndexPaths) writeOutput(path, (target) => writeFileSync(target, topIndex));
console.log(`Rewrote ${stalePaths.length} module READMEs; wrote ${outputDirectory} (top index ${topIndex.length} bytes).`);
