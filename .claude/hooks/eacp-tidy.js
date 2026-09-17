#!/usr/bin/env node
// PostToolUse hook: runs eacp-clang-tidy on the edited file and blocks
// (exit 2) when any eacp-* check fires. Exits 0 quietly when the file is
// not a C/C++/ObjC++ source or the tidy binary is not built (e.g. Windows).
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const EXTENSIONS = ['.cpp', '.cc', '.cxx', '.mm', '.h', '.hpp'];

function main()
{
    let input;
    try
    {
        input = JSON.parse(fs.readFileSync(0, 'utf8'));
    }
    catch
    {
        return 0;
    }

    const file = input?.tool_input?.file_path;
    if (!file || !EXTENSIONS.includes(path.extname(file).toLowerCase()))
        return 0;

    const projectDir = process.env.CLAUDE_PROJECT_DIR || process.cwd();
    if (path.relative(projectDir, file).startsWith('..'))
        return 0;

    const binary = process.platform === 'win32'
        ? 'eacp-clang-tidy.exe'
        : 'eacp-clang-tidy';
    const tidy = path.join(projectDir, 'ExtraClangRules', 'bin', binary);
    if (!fs.existsSync(tidy))
        return 0;

    const result = spawnSync(
        tidy,
        ['-p', path.join(projectDir, 'build'), '--checks=-*,eacp-*', file],
        { encoding: 'utf8' });

    const warnings = (result.stdout || '')
        .split('\n')
        .filter(line => /warning:.*eacp-/.test(line));

    if (warnings.length === 0)
        return 0;

    process.stderr.write(warnings.join('\n') + '\n');
    return 2;
}

process.exit(main());
