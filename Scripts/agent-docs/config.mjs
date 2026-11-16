import { readFileSync } from 'node:fs';

export const defaultConfig = {
    summary: '',
    skip_directories: ['detail', 'Detail', 'Test', 'Tests', 'Testing', 'cmake', 'ThirdParty', 'node_modules'],
    platform_file_suffixes: ['-Android', '-Linux', '-Windows', '-macOS', '-iOS', '-Posix', '-Apple'],
    internal_header_suffixes: ['Internal'],
    header_extensions: ['.h', '.hpp'],
};
export const requiredKeys = ['name', 'repository', 'module_roots'];

const keyValuePattern = /^([A-Za-z_]+)\s*=\s*(.+)$/;
const stringPattern = /^"((?:[^"\\]|\\.)*)"$/;

function parseTomlValue(text, lineNumber) {
    const trimmed = text.trim();
    const stringMatch = trimmed.match(stringPattern);
    if (stringMatch) return JSON.parse(`"${stringMatch[1]}"`);
    if (trimmed.startsWith('[') && trimmed.endsWith(']')) {
        const items = trimmed.slice(1, -1).split(',').map((item) => item.trim()).filter((item) => item !== '');
        return items.map((item) => parseTomlValue(item, lineNumber));
    }
    throw new Error(`agent-docs.toml line ${lineNumber}: only "strings" and ["arrays", "of strings"] are supported, got ${trimmed}`);
}

function stripComment(line) {
    return line.replace(/\s+#.*$/, '').trim();
}

function joinMultilineArrays(lines) {
    const statements = [];
    lines.forEach((rawLine, index) => {
        const line = stripComment(rawLine);
        if (line === '' || line.startsWith('#')) return;
        const previous = statements.at(-1);
        if (previous && previous.isOpenArray) {
            previous.text += ` ${line}`;
            previous.isOpenArray = !line.endsWith(']');
            return;
        }
        statements.push({ text: line, lineNumber: index + 1, isOpenArray: /=\s*\[/.test(line) && !line.endsWith(']') });
    });
    return statements;
}

export function parseConfigText(text) {
    const values = {};
    for (const { text: statement, lineNumber } of joinMultilineArrays(text.split('\n'))) {
        const match = statement.match(keyValuePattern);
        if (!match) throw new Error(`agent-docs.toml line ${lineNumber}: expected key = value, got ${statement}`);
        values[match[1]] = parseTomlValue(match[2], lineNumber);
    }
    return values;
}

export function resolveConfig(values) {
    const knownKeys = [...requiredKeys, 'slug', ...Object.keys(defaultConfig)];
    const unknownKeys = Object.keys(values).filter((key) => !knownKeys.includes(key));
    if (unknownKeys.length > 0) throw new Error(`agent-docs.toml: unknown keys ${unknownKeys.join(', ')}; known: ${knownKeys.join(', ')}`);
    const missingKeys = requiredKeys.filter((key) => !(key in values));
    if (missingKeys.length > 0) throw new Error(`agent-docs.toml: missing ${missingKeys.join(', ')}`);
    const merged = { ...defaultConfig, ...values };
    return {
        name: merged.name,
        slug: merged.slug ?? merged.name,
        summary: merged.summary,
        repository: merged.repository,
        moduleRoots: merged.module_roots,
        skipDirectories: merged.skip_directories,
        platformFileSuffixes: merged.platform_file_suffixes,
        internalHeaderSuffixes: merged.internal_header_suffixes,
        headerExtensions: merged.header_extensions,
    };
}

export function readConfig(path) {
    return resolveConfig(parseConfigText(readFileSync(path, 'utf8')));
}
