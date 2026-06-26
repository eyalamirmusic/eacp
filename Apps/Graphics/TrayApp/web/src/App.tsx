import { FormEvent, KeyboardEvent, useEffect, useRef, useState } from 'react';
import { backend } from './generated/backend';
import { usePlayback } from './generated/hooks';

type DownloadResult = {
    name: string;
    path: string;
    kind: string;
    size: number;
    score: number;
};

function formatSize(bytes: number): string
{
    if (bytes < 1024) return `${bytes} B`;
    const kb = bytes / 1024;
    if (kb < 1024) return `${kb.toFixed(kb < 10 ? 1 : 0)} KB`;
    const mb = kb / 1024;
    if (mb < 1024) return `${mb.toFixed(mb < 10 ? 1 : 0)} MB`;
    return `${(mb / 1024).toFixed(1)} GB`;
}

function isAudioResult(result: DownloadResult): boolean
{
    return ['wav', 'mp3', 'aif', 'aiff', 'flac', 'm4a', 'aac', 'ogg']
        .includes(result.kind.toLowerCase());
}

export default function App()
{
    const inputRef = useRef<HTMLInputElement>(null);
    const resultRefs = useRef<Array<HTMLButtonElement | null>>([]);
    const resultsRef = useRef<DownloadResult[]>([]);
    const selectedIndexRef = useRef(-1);
    const playback = usePlayback();
    const [query, setQuery] = useState('');
    const [results, setResults] = useState<DownloadResult[]>([]);
    const [selectedIndex, setSelectedIndex] = useState(-1);
    const [status, setStatus] = useState('Loading Downloads...');

    useEffect(() =>
    {
        resultsRef.current = results;
    }, [results]);

    useEffect(() =>
    {
        selectedIndexRef.current = selectedIndex;
    }, [selectedIndex]);

    useEffect(() =>
    {
        function onKeyDown(event: globalThis.KeyboardEvent)
        {
            if (event.metaKey && !event.ctrlKey && event.key.toLowerCase() === 'c')
            {
                const index = selectedIndexRef.current;
                const result = index >= 0 ? resultsRef.current[index] : undefined;
                if (!result)
                    return;

                event.preventDefault();
                event.stopPropagation();
                void copyResult(result);
                return;
            }

            if (event.key === 'Escape' || (event.ctrlKey && event.key.toLowerCase() === 'c'))
            {
                event.preventDefault();
                event.stopPropagation();
                void backend.dismiss();
            }
        }

        document.addEventListener('keydown', onKeyDown, true);

        const focusInput = () =>
        {
            inputRef.current?.focus();
            inputRef.current?.select();
        };
        focusInput();
        const firstRetry = window.setTimeout(focusInput, 0);
        const secondRetry = window.setTimeout(focusInput, 50);

        return () =>
        {
            document.removeEventListener('keydown', onKeyDown, true);
            window.clearTimeout(firstRetry);
            window.clearTimeout(secondRetry);
        };
    }, []);

    useEffect(() =>
    {
        let cancelled = false;

        const timeout = window.setTimeout(() =>
        {
            void backend.searchDownloads({ query })
                .then((response) =>
                {
                    if (cancelled) return;
                    setResults(response.results);
                    setSelectedIndex((index) =>
                        response.results.length === 0
                            ? -1
                            : Math.min(Math.max(index, -1), response.results.length - 1));
                    setStatus(response.results.length === 0
                        ? 'No matching audio files in Downloads'
                        : `${response.results.length} match${response.results.length === 1 ? '' : 'es'}`);
                })
                .catch((error) =>
                {
                    if (cancelled) return;
                    setResults([]);
                    setStatus(`Search failed: ${String(error)}`);
                });
        }, 40);

        return () =>
        {
            cancelled = true;
            window.clearTimeout(timeout);
        };
    }, [query]);

    useEffect(() =>
    {
        if (selectedIndex >= 0)
            resultRefs.current[selectedIndex]?.scrollIntoView({ block: 'nearest' });
    }, [selectedIndex]);

    function submit(event: FormEvent<HTMLFormElement>)
    {
        event.preventDefault();

        const value = query.trim();
        inputRef.current?.blur();
        void backend.submitPrompt({ text: value });
    }

    function armDrag(result: DownloadResult)
    {
        setStatus(`Drag ${result.name} out...`);
        void backend.armDrag({ paths: [result.path] });
    }

    function copyResult(result: DownloadResult, dismissAfterCopy = false)
    {
        setStatus(`Copying ${result.name}...`);
        void backend.copyFiles({ paths: [result.path] })
            .then((response) =>
            {
                if (response.copied && dismissAfterCopy)
                {
                    void backend.dismiss();
                    return;
                }

                setStatus(response.copied
                    ? `Copied ${result.name}`
                    : `Could not copy ${result.name}`);
            })
            .catch((error) =>
            {
                setStatus(`Copy failed: ${String(error)}`);
            });
    }

    function togglePlayback(result: DownloadResult)
    {
        if (playback.playing && playback.path === result.path)
        {
            void backend.stopAudio();
            return;
        }

        setStatus(`Playing ${result.name}`);
        void backend.playAudio({ path: result.path });
    }

    function selectResult(index: number, focusRow = false, autoplay = false)
    {
        if (results.length === 0)
        {
            setSelectedIndex(-1);
            return;
        }

        const next = Math.max(0, Math.min(index, results.length - 1));
        setSelectedIndex(next);

        if (focusRow)
            window.requestAnimationFrame(() => resultRefs.current[next]?.focus());

        if (autoplay)
        {
            const result = results[next];
            if (result && playback.path !== result.path)
            {
                setStatus(`Playing ${result.name}`);
                void backend.playAudio({ path: result.path });
            }
        }
    }

    function onInputKeyDown(event: KeyboardEvent<HTMLInputElement>)
    {
        if (event.key === 'ArrowDown')
        {
            event.preventDefault();
            selectResult(selectedIndex < 0 ? 0 : selectedIndex + 1, true, true);
        }
        else if (event.key === 'ArrowUp')
        {
            event.preventDefault();
            if (selectedIndex <= 0)
            {
                setSelectedIndex(-1);
                inputRef.current?.focus();
            }
            else
            {
                selectResult(selectedIndex - 1, true, true);
            }
        }
        else if (event.key === 'Enter' && selectedIndex >= 0)
        {
            event.preventDefault();
            const result = results[selectedIndex];
            if (result)
                copyResult(result, true);
        }
        else if (event.metaKey && event.key.toLowerCase() === 'c' && selectedIndex >= 0)
        {
            event.preventDefault();
            const result = results[selectedIndex];
            if (result)
                copyResult(result);
        }
    }

    function onResultKeyDown(event: KeyboardEvent<HTMLButtonElement>, index: number)
    {
        if (event.key === 'ArrowDown')
        {
            event.preventDefault();
            selectResult(index + 1, true, true);
        }
        else if (event.key === 'ArrowUp')
        {
            event.preventDefault();
            if (index <= 0)
            {
                setSelectedIndex(-1);
                inputRef.current?.focus();
            }
            else
            {
                selectResult(index - 1, true, true);
            }
        }
        else if (event.key === 'Enter')
        {
            event.preventDefault();
            const result = results[index];
            if (result)
                copyResult(result, true);
        }
        else if (event.key === '/')
        {
            event.preventDefault();
            setSelectedIndex(-1);
            inputRef.current?.focus();
            inputRef.current?.select();
        }
        else if (event.metaKey && event.key.toLowerCase() === 'c')
        {
            event.preventDefault();
            const result = results[index];
            if (result)
                copyResult(result);
        }
    }

    return (
        <main className="launcher-shell">
            <form className="composer" onSubmit={submit}>
                <div className="mark" aria-hidden="true">
                    <span/>
                    <span/>
                    <span/>
                    <span/>
                    <span/>
                    <span/>
                    <span/>
                    <span/>
                </div>

                <input
                    ref={inputRef}
                    autoFocus
                    name="prompt"
                    aria-label="Search audio in Downloads"
                    placeholder="Search audio in Downloads"
                    autoComplete="off"
                    spellCheck={false}
                    value={query}
                    onChange={(event) => setQuery(event.target.value)}
                    onKeyDown={onInputKeyDown}
                />

                <button className="chat-kind" type="button">
                    Audio
                    <span aria-hidden="true"/>
                </button>

                <button className="send" type="submit" aria-label="Send">
                    <svg viewBox="0 0 24 24" aria-hidden="true">
                        <path d="M12 19V5"/>
                        <path d="m5 12 7-7 7 7"/>
                    </svg>
                </button>
            </form>

            <section className="results" aria-label="Download search results">
                {results.map((result, index) => (
                    <button
                        key={result.path}
                        ref={(node) => { resultRefs.current[index] = node; }}
                        className={index === selectedIndex
                            ? 'result-row selected'
                            : 'result-row'}
                        type="button"
                        onFocus={() =>
                        {
                            setSelectedIndex(index);
                        }}
                        onKeyDown={(event) => onResultKeyDown(event, index)}
                        onMouseDown={() =>
                        {
                            setSelectedIndex(index);
                            armDrag(result);
                        }}
                        onClick={() =>
                        {
                            if (isAudioResult(result))
                                togglePlayback(result);
                        }}
                        title={result.path}
                    >
                        <span className="file-icon" aria-hidden="true">
                            {result.kind.slice(0, 1).toUpperCase() || 'F'}
                        </span>
                        <span className="file-main">
                            <span className="file-name">{result.name}</span>
                            <span className="file-path">{result.path}</span>
                        </span>
                        <span className="file-meta">
                            {isAudioResult(result) && (
                                <span className="play-chip">
                                    {playback.playing && playback.path === result.path
                                        ? 'Stop'
                                        : 'Play'}
                                </span>
                            )}
                            <span>{result.kind}</span>
                            <span>{formatSize(result.size)}</span>
                        </span>
                    </button>
                ))}

                {results.length === 0 && (
                    <div className="empty">{status}</div>
                )}
            </section>

            <div className="status">{status}</div>
        </main>
    );
}
