import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join, posix } from 'node:path';

export const shortShaLength = 7;
export const largestFileListingBytes = 256 * 1024 * 1024;
export const hiddenNamespacePattern = /detail|impl/i;
export const getShortSha = (sha) => sha.slice(0, shortShaLength);
export const getBlobUrlTemplate = (repository) => `https://github.com/${repository}/blob/<ref>/<path>`;

const getDirectoryOf = (path) => path.slice(0, path.lastIndexOf('/'));
const getFileNameOf = (path) => path.slice(path.lastIndexOf('/') + 1);
const getStemOf = (fileName) => fileName.replace(/\.[^.]+$/, '');

export function isSkippedPath(path, config) {
    return path.split('/').some((part) => config.skipDirectories.includes(part));
}

export function isPublicHeader(path, config) {
    const fileName = getFileNameOf(path);
    const hasHeaderExtension = config.headerExtensions.some((extension) => fileName.endsWith(extension));
    if (!hasHeaderExtension) return false;
    const stem = getStemOf(fileName);
    const isPlatformFile = config.platformFileSuffixes.some(
        (tag) => stem.endsWith(tag) || stem.includes(`${tag}-`) || `-${stem}` === tag,
    );
    const isInternalFile = config.internalHeaderSuffixes.some(
        (suffix) => stem.length > suffix.length && stem.endsWith(suffix),
    );
    return !isPlatformFile && !isInternalFile;
}

function findModuleRoot(path, config) {
    return config.moduleRoots.find((root) => path.startsWith(`${root}/`)) ?? null;
}

export function groupTreeIntoModules(paths, config) {
    const modulesByDirectory = new Map();
    const getModule = (directory, root) => {
        if (!modulesByDirectory.has(directory)) {
            modulesByDirectory.set(directory, { directory, root, readmePath: null, headerPaths: [] });
        }
        return modulesByDirectory.get(directory);
    };

    for (const path of paths) {
        const root = findModuleRoot(path, config);
        if (!root || isSkippedPath(path.slice(root.length), config)) continue;
        const directory = getDirectoryOf(path);
        if (directory === root) continue;
        if (getFileNameOf(path) === 'README.md') {
            getModule(directory, root).readmePath = path;
        } else if (isPublicHeader(path, config)) {
            getModule(directory, root).headerPaths.push(path);
        }
    }

    const modules = [...modulesByDirectory.values()];
    for (const module of modules) module.headerPaths.sort();
    return modules.sort((first, second) => first.directory.localeCompare(second.directory));
}

function stripStringsAndLineComments(line) {
    return line.replace(/"(?:[^"\\]|\\.)*"/g, '""').replace(/\/\/.*$/, '');
}

function getFirstSentence(text) {
    const collapsed = text.replace(/\s+/g, ' ').trim();
    const sentenceEnd = collapsed.search(/[.!?](\s|$)/);
    return sentenceEnd === -1 ? collapsed : collapsed.slice(0, sentenceEnd + 1);
}

function readCommentLine(line) {
    const trimmed = line.trim();
    if (trimmed.startsWith('//')) return trimmed.replace(/^\/\/+\s?/, '');
    return null;
}

const declarationPatterns = [
    { kind: 'type', pattern: /^(?:template\s*<.*>\s*)?(?:class|struct|union)\s+(?:\[\[[^\]]*\]\]\s*)?(\w+)\b(?!\s*;)/ },
    { kind: 'type', pattern: /^enum\s+(?:class\s+|struct\s+)?(\w+)\b(?!\s*;)/ },
    { kind: 'type', pattern: /^using\s+(\w+)\s*=/ },
    { kind: 'type', pattern: /^using\s+(?!namespace)[\w:]+::(\w+)\s*;/ },
    { kind: 'function', pattern: /^(?:\[\[[^\]]*\]\]\s*)?(?:inline\s+|constexpr\s+|static\s+)*[\w:<>,*&\s]+?[\s*&](\w+)\s*\(/ },
];

const nonFunctionWords = new Set(['if', 'for', 'while', 'switch', 'return', 'sizeof', 'decltype', 'operator']);

function matchDeclaration(statement) {
    if (statement.startsWith('#') || statement.startsWith('namespace')) return null;
    for (const { pattern } of declarationPatterns) {
        const match = statement.match(pattern);
        if (match && !nonFunctionWords.has(match[1])) return match[1];
    }
    return null;
}

function createHeaderScan() {
    return {
        namespaceDepths: [],
        braceDepth: 0,
        pendingComment: [],
        firstDeclarationComment: null,
        hasSeenDeclaration: false,
        isInsideBlockComment: false,
        blockComment: [],
        pendingNamespaceName: null,
        declaredNames: [],
    };
}

function getTextBeforeBlockCommentEnd(text) {
    const endIndex = text.indexOf('*/');
    return { text: endIndex === -1 ? text : text.slice(0, endIndex), hasEnded: endIndex !== -1 };
}

function scanBlockCommentLine(scan, trimmed) {
    if (scan.isInsideBlockComment) {
        const { text, hasEnded } = getTextBeforeBlockCommentEnd(trimmed);
        scan.blockComment.push(text.replace(/^\*\s?/, ''));
        scan.isInsideBlockComment = !hasEnded;
    } else if (trimmed.startsWith('/*')) {
        const { text, hasEnded } = getTextBeforeBlockCommentEnd(trimmed.slice(2));
        scan.blockComment = [text.replace(/^\*+\s?/, '')];
        scan.isInsideBlockComment = !hasEnded;
    } else {
        return false;
    }
    if (!scan.isInsideBlockComment) scan.pendingComment = scan.blockComment;
    return true;
}

function scanLineCommentOrBlank(scan, rawLine) {
    const commentText = readCommentLine(rawLine);
    if (commentText !== null) {
        scan.pendingComment.push(commentText);
        return true;
    }
    if (rawLine.trim() !== '') return false;
    scan.pendingComment = [];
    return true;
}

function isAtPublicNamespaceScope(scan) {
    const isAtNamespaceScope = scan.braceDepth === scan.namespaceDepths.length;
    const isInsideDetail = scan.namespaceDepths.some((name) => hiddenNamespacePattern.test(name));
    return isAtNamespaceScope && !isInsideDetail;
}

function recordDeclaration(scan, trimmed) {
    const name = isAtPublicNamespaceScope(scan) ? matchDeclaration(trimmed) : null;
    if (!name) return;
    if (!scan.hasSeenDeclaration && scan.pendingComment.length > 0) {
        scan.firstDeclarationComment = scan.pendingComment.join(' ');
    }
    scan.hasSeenDeclaration = true;
    if (!scan.declaredNames.includes(name)) scan.declaredNames.push(name);
}

function openBrace(scan) {
    if (scan.pendingNamespaceName !== null && scan.braceDepth === scan.namespaceDepths.length) {
        scan.namespaceDepths.push(scan.pendingNamespaceName);
    }
    scan.pendingNamespaceName = null;
    scan.braceDepth += 1;
}

function closeBrace(scan) {
    scan.braceDepth -= 1;
    if (scan.braceDepth < scan.namespaceDepths.length) scan.namespaceDepths.pop();
}

function trackBraces(scan, rawLine) {
    const code = stripStringsAndLineComments(rawLine);
    const namespaceMatch = code.match(/^\s*namespace\s+([\w:]*)\s*(\{)?/);
    if (namespaceMatch) scan.pendingNamespaceName = namespaceMatch[1] || 'anonymous';
    for (const character of code) {
        if (character === '{') openBrace(scan);
        else if (character === '}') closeBrace(scan);
    }
}

function scanHeaderLine(scan, rawLine) {
    const trimmed = rawLine.trim();
    if (scanBlockCommentLine(scan, trimmed) || scanLineCommentOrBlank(scan, rawLine)) return;
    recordDeclaration(scan, trimmed);
    scan.pendingComment = [];
    trackBraces(scan, rawLine);
}

export function parseHeader(source) {
    const scan = createHeaderScan();
    for (const rawLine of source.split('\n')) scanHeaderLine(scan, rawLine);

    const includedHeaders = [...source.matchAll(/^#include\s+"([^"]+)"/gm)].map((match) => match[1]);
    return {
        includedHeaders,
        summary: scan.firstDeclarationComment ? getFirstSentence(scan.firstDeclarationComment) : null,
        declaredNames: scan.declaredNames,
    };
}

function isUmbrellaHeader({ summary, declaredNames, includedHeaders }) {
    const declaresNothing = !summary && declaredNames.length === 0;
    return declaresNothing && includedHeaders.length > 0;
}

function getDeclaresText(declaredNames) {
    if (declaredNames.length === 0) return '';
    return `declares ${declaredNames.map((name) => `\`${name}\``).join(', ')}`;
}

export function getHeaderSummaryLine(source) {
    const parsed = parseHeader(source);
    if (isUmbrellaHeader(parsed)) return `umbrella header: includes ${parsed.includedHeaders.length} headers`;
    const declares = getDeclaresText(parsed.declaredNames);
    if (parsed.summary && declares) return `${parsed.summary} (${declares})`;
    return parsed.summary ?? declares;
}

export const readmeFileName = 'README.md';
export const topIndexPaths = ['index.md', 'llms.txt'];
export const namesInModuleSummary = 3;
export const generatedSectionStart = '<!-- agent-docs:start (generated by eacp-agent-docs; edits here are overwritten) -->';
export const generatedSectionEnd = '<!-- agent-docs:end -->';

const getRelativeLink = (fromDirectory, targetPath) => posix.relative(fromDirectory, targetPath);
export const getReadmePath = (directory) => `${directory}/${readmeFileName}`;
const getModuleName = (node) => node.directory.slice(`${node.root}/`.length);

function createModuleNode(directory, root) {
    return { directory, root, readmePath: null, headerPaths: [], childDirectories: [] };
}

function linkToAncestors(nodesByDirectory, { directory, root }) {
    let child = directory;
    while (getDirectoryOf(child) !== root) {
        const parent = getDirectoryOf(child);
        if (!nodesByDirectory.has(parent)) nodesByDirectory.set(parent, createModuleNode(parent, root));
        const siblings = nodesByDirectory.get(parent).childDirectories;
        if (!siblings.includes(child)) siblings.push(child);
        child = parent;
    }
}

export function buildModuleTree(modules) {
    const nodesByDirectory = new Map(modules.map((module) => [module.directory, { ...module, childDirectories: [] }]));
    for (const module of modules) linkToAncestors(nodesByDirectory, module);
    for (const node of nodesByDirectory.values()) node.childDirectories.sort();
    return nodesByDirectory;
}

export function getReadmeProse(readme) {
    const startIndex = readme.indexOf(generatedSectionStart);
    return startIndex === -1 ? readme : readme.slice(0, startIndex);
}

const nonProseParagraphStarts = ['-', '`', '|', '<', '>'];

function isPlainProseParagraph(paragraph) {
    return paragraph !== '' && !nonProseParagraphStarts.some((start) => paragraph.startsWith(start));
}

function getReadmeFirstSentence(readme) {
    const proseLines = getReadmeProse(readme).split('\n').filter((line) => !line.startsWith('#'));
    const paragraphs = proseLines.join('\n').trim().split(/\n\s*\n/);
    const paragraph = paragraphs.find(isPlainProseParagraph);
    return paragraph ? getFirstSentence(paragraph) : null;
}

function getModuleDeclaredNames(node, readTextByPath) {
    return node.headerPaths.flatMap((headerPath) => parseHeader(readTextByPath.get(headerPath)).declaredNames);
}

export function getModuleSummaryLine(node, readTextByPath) {
    const sentence = getReadmeFirstSentence(readTextByPath.get(getReadmePath(node.directory)) ?? '');
    if (sentence) return sentence;
    const names = getModuleDeclaredNames(node, readTextByPath).slice(0, namesInModuleSummary);
    if (names.length > 0) return getDeclaresText(names);
    return `submodules: ${node.childDirectories.map((directory) => getFileNameOf(directory)).join(', ')}`;
}

function renderHeaderLine(node, headerPath, readTextByPath) {
    const summaryLine = getHeaderSummaryLine(readTextByPath.get(headerPath));
    const description = summaryLine ? `: ${summaryLine}` : '';
    return `- [${getFileNameOf(headerPath)}](${getRelativeLink(node.directory, headerPath)})${description}`;
}

function renderModuleLink(fromDirectory, node, readTextByPath) {
    const link = getRelativeLink(fromDirectory, getReadmePath(node.directory));
    return `- [${getFileNameOf(node.directory)}](${link}): ${getModuleSummaryLine(node, readTextByPath)}`;
}

export function renderGeneratedSection(node, nodesByDirectory, readTextByPath) {
    const parts = [generatedSectionStart, ''];
    if (node.headerPaths.length > 0) {
        parts.push('## Headers', '', ...node.headerPaths.map((headerPath) => renderHeaderLine(node, headerPath, readTextByPath)), '');
    }
    if (node.childDirectories.length > 0) {
        const childLines = node.childDirectories.map((directory) =>
            renderModuleLink(node.directory, nodesByDirectory.get(directory), readTextByPath),
        );
        parts.push('## Submodules', '', ...childLines, '');
    }
    parts.push(generatedSectionEnd);
    return parts.join('\n');
}

export function replaceGeneratedSection(readme, generatedSection) {
    const startIndex = readme.indexOf(generatedSectionStart);
    if (startIndex === -1) return `${readme.trimEnd()}\n\n${generatedSection}\n`;
    const endIndex = readme.indexOf(generatedSectionEnd, startIndex);
    const after = endIndex === -1 ? '' : readme.slice(endIndex + generatedSectionEnd.length);
    return `${readme.slice(0, startIndex)}${generatedSection}${after}`;
}

function getCurrentReadme(node, readTextByPath) {
    return readTextByPath.get(getReadmePath(node.directory)) ?? `# ${getModuleName(node)}\n`;
}

export function renderModuleReadmes(nodesByDirectory, readTextByPath) {
    const readmes = new Map();
    for (const node of nodesByDirectory.values()) {
        const generatedSection = renderGeneratedSection(node, nodesByDirectory, readTextByPath);
        readmes.set(getReadmePath(node.directory), replaceGeneratedSection(getCurrentReadme(node, readTextByPath), generatedSection));
    }
    return readmes;
}

function renderTopModuleLines(nodesByDirectory, readTextByPath) {
    const topNodes = [...nodesByDirectory.values()].filter((node) => getDirectoryOf(node.directory) === node.root);
    topNodes.sort((first, second) => first.directory.localeCompare(second.directory));
    return topNodes.flatMap((node) => {
        const childLines = node.childDirectories.map(
            (childDirectory) => `  ${renderModuleLink('.', nodesByDirectory.get(childDirectory), readTextByPath)}`,
        );
        return [renderModuleLink('.', node, readTextByPath), ...childLines];
    });
}

export function renderTopIndex({ config, sha, reference, nodesByDirectory, readTextByPath }) {
    return [
        `# ${config.name}`,
        '',
        ...(config.summary ? [`> ${config.summary}`, ''] : []),
        `Ref: \`${reference}\` at ${getShortSha(sha)} (${config.repository}).`,
        `Links are relative to this file's directory; the same path in the repository is ${getBlobUrlTemplate(config.repository)}.`,
        "Each module's README.md is its documentation: hand-written rules and recipes, then its public headers with one-line summaries and its submodules.",
        '',
        '## Modules',
        '',
        ...renderTopModuleLines(nodesByDirectory, readTextByPath),
        '',
    ].join('\n');
}

export function resolveCommit(checkout) {
    return execFileSync('git', ['-C', checkout, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
}

export function resolveBranchName(checkout) {
    return execFileSync('git', ['-C', checkout, 'rev-parse', '--abbrev-ref', 'HEAD'], { encoding: 'utf8' }).trim();
}

export function listTree(checkout) {
    const listing = execFileSync('git', ['-C', checkout, 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], {
        encoding: 'utf8',
        maxBuffer: largestFileListingBytes,
    });
    return listing.split('\0').filter((path) => path !== '');
}

export function readTexts(checkout, paths) {
    return new Map(paths.map((path) => [path, readFileSync(join(checkout, path), 'utf8')]));
}

export function getPathsToRead(modules) {
    return modules.flatMap((module) => [...(module.readmePath ? [module.readmePath] : []), ...module.headerPaths]);
}
