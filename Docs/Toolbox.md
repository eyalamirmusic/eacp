# Toolbox

What eacp's Core, Network and bundled dependencies already provide. Before
writing any file-local helper, grep this file for the verb. If it is missing,
add it to Core with a test rather than beside the call site. JSON goes through
Miro reflection (`MIRO_REFLECT`, `toJSON`, `fromJSON`), never hand-walked.

## Strings — `Core/Utils/Strings.h` (`eacp::Strings`)

- `trim(s)` — strip leading and trailing whitespace
- `toLower(s)` — ASCII lower case
- `capitalize(s)` / `uncapitalize(s)` — change the first character's case
- `equalsCaseInsensitive(a, b)` — ASCII case-insensitive compare
- `lastNonEmptyLine(text)` — the last line with content
- `lineContaining(text, needle)` — the first line containing a substring
- `hexCharToInt(c)` — one hex digit to its value
- `widen(utf8)` / `narrow(wide)` — UTF-8 to and from `std::wstring`
- `toString(value)` — strings, bool, char, enums and numbers to text
- `concat(args...)` — `toString` of each argument, joined
- `tryParse<T>(s)` — whole string to an integer, float or double, or nullopt
- `tryParseBool(s)` — text to bool, or nullopt
- `parseOr<T>(s, fallback)` — `tryParse` with a default
- `tryParseInt` / `tryParseFloat` / `parseIntOr` / `parseFloatOr` — fixed-type forms
- In #177, pending: `toUpper`, `split(text, delimiter, maximumParts)`,
  `join(parts, separator)`, `parseHex(text)`
- Not here: `startsWith`/`endsWith`/`contains` — use `std::string_view`'s own

## Formatting — `Core/Utils/Format.h` (`eacp::Strings`)

- `format(pattern, args...)` — fmt-style `{}` formatting, no fmt header needed
- `vformat(pattern, args)` — the type-erased form under `format`
- `shortestRoundTrip(float)` — the shortest text that parses back exactly

## Logging and console — `Core/Utils/Logging.h`, `Core/Utils/Console.h`

- `LOG(args...)` — timestamped log line, arguments `concat`ed
- `LOGF(pattern, args...)` — timestamped log line through `Strings::format`
- `logMessage(text)` — log one already-built line
- `setLogFile(path)` — also write the log to a file
- `Console::print(line)` / `Console::printError(line)` — plain stdout/stderr line

## Files — `Core/Utils/Files.h` (`eacp::Files`)

- `readFile(path)` — whole file as a string
- `writeFile(path, bytes)` — replace a file with bytes (text overload in #177)
- `writeFileAtomically(path, bytes)` — write to a sibling, then rename over
- `appendFile(path, bytes | text)` — append to the end
- `writeFileAt(path, offset, bytes)` — positional write, never truncates
- `exists` / `isFile` / `isDirectory` — what is at a path
- `createDirectories(dir)` — make a directory and its parents
- `copyFile(from, to)` / `copyDirectory(from, to)` — copy, overwriting
- `moveFile(from, to)` — rename, copy-and-remove across devices
- `removeFile(path)` — delete one file; `removeAll(path)` — delete a tree
- `legalFileName(name)` — strip characters no filesystem allows
- `nonexistentSibling(path)` / `nonexistentChild(dir, prefix, suffix)` — a free name
- `matchesWildcard(name, pattern)` — `*`/`?` lists separated by `;` or `,`
- `forEachEntry(dir, options, visitor)` — walk a directory, visitor steers
- `listDirectory(dir, options)` — entries (path, kind, depth, hidden)
- `listFiles(dir, options)` — file paths only; `DirectoryOptions` for recursion,
  hidden, symlinks, pattern, errors
- `openWithSystem(path)` — open with the default app
- `revealInFileManager(path)` — show in Finder/Explorer/the parent folder
- `executablePath()` / `applicationPath()` / `enclosingBundle(binary)` — where we run
- `resourcesDirectory()` / `getBundleResourcePath(name)` — bundled resources
- `filenameFromPath(path)` — the last component of a string path

## Paths — `Core/Utils/FilePath.h` (`eacp::FilePath`)

- `operator/`, `parentDirectory`, `fileName`, `stem`, `extension` — path parts
- `hasExtension(ext)` / `withExtension(ext)` / `sibling(name)` — derived paths
- `isAbsolute`, `str`, `c_str`, `nativeString`, `wide`, `fromWide`, `fromNative`
- `read()` — the file's contents
- `homeDirectory`, `documentsDirectory`, `downloadsDirectory`, `desktopDirectory`,
  `musicDirectory`, `moviesDirectory`, `picturesDirectory`, `tempDirectory`
- `appDataDirectory` / `cacheDirectory` — per-user data and cache roots
- `appSupportDirectory()` / `appCacheDirectory()` — `<root>/<Company>/<App>`
- `commonAppDataDirectory` / `applicationsDirectory` — machine-wide roots
- `appGroupContainerDirectory(id)` — Apple app-group container

## File objects — `Core/Utils/`

- `File` (`File.h`) — `exists`, `isRegularFile`, `isUnder(root)`, `size`,
  `modificationTime`, positional `read(offset, out)`
- `FileWriter` (`FileWriter.h`) — open truncate/append, `write`, `seek`,
  `position`, `flush`, `truncate`
- `TemporaryFile` (`TemporaryFile.h`) — reserved temp path removed on destruction
- `MemoryMappedFile` (`MemoryMappedFile.h`) — read-only map, `bytes()`, `text()`
- `Files::FileWatcher` (`FileWatcher.h`) — poll paths, report changed files
- `Files::SourceWatcher` (`SourceWatcher.h`) — `FileWatcher` over a source tree

## Time — `Core/Utils/Time.h` (`eacp::Time`)

- `MS` — a millisecond count, the unit every eacp API takes
- `sleep(MS)` / `sleepMS(int)` — block the calling thread
- `Deadline(timeout)` — `expired()`, `remaining()`
- In #177, pending: `sinceEpoch()`, `formatIso8601(MS)`

## Encoding and archives — `Core/Utils/Base64.h`, `Core/Utils/Zip.h`

- `Base64::encode(bytes)` / `Base64::decode(text)` — RFC 4648
- `Zip::Reader` — `entries`, `find`, `read`, `readText`, `extract`, `extractAll`
- `Zip::Writer` — `add`, `addFile`, `addDirectory`, `finish`, `writeTo`
- `Zip::compress` / `decompress` / `decompressText` — one zlib blob

## Small utilities — `Core/Utils/`

- `Singleton::get<T>()` / `getImmortal<T>()` — function-static instance
- `Pimpl<T>` — owning pointer-to-implementation
- `Random` — `getNext(min, max)`, `get<T>(min, max)`, optional seed
- `Range<T>` — `start`/`length`, `end`, `contains`, `empty`
- `getEnv(name)` / `getEnvValue(name)` / `setEnv` / `unsetEnv` — `Environment.h`
- `Containers.h` — `Vector`, `OwnedVector`, `Array`, `Span`, `OwningPointer`
- `Broadcaster.h` — `Broadcaster`, `Listener`
- `Maths/` — `Vec`, `Mat4`, `Constants.h`

## App — `Core/App/`

- `Args::Parser` (`Args.h`) — bind options/flags to variables, `parse()`, help
- `Apps::commandLineArgs()` / `setCommandLineArgs` (`AppEnvironment.h`)
- `Apps::getAppEnvironment()` — headless flag and arguments
- `runConsole(argc, argv, body)` (`App.h`) — command-line tool entry point
- `Apps::quit`, `restart`, `openExternalURL`, `setAppBadge`, `setQuitHandler`
- `chooseFile` / `chooseSaveFile` / `chooseDirectory` (+ `Async`) — pickers
- `Clipboard::copyText`, `copyFiles`, `getText`, `hasText` (`Clipboard.h`)
- `LoginItem.h` — launch at login

## Platform — `Core/Platform/Platform.h`

- `isMac`, `isIOS`, `isApple`, `isWindows`, `isAndroid`, `isLinux`,
  `isLinuxFamily`, `isPosix`, `name()` — compile-time OS checks
- `isStandalone()` / `isDLL()` — app or plugin copy
- `getAppName()`, `getCompanyName()`, `getAppVersion()` — from AppInfo
- Not here: `isDebuggerAttached`

## Processes — `Core/Process/Process.h` (`eacp::Processes`)

- `run(options)` / `run(executable, args)` — blocking, returns `ProcessResult`
- `runAsync(options)` — `Async<ProcessResult>` resolved on the main thread
- `Process` — `writeToInput`, `closeInput`, `wait`, `terminate`, `kill`, `output`
- `ProcessOptions` — `executable`, `arguments`, `workingDirectory`, `environment`,
  `commandLine`, `captureOutput`, `mergeErrorOutput`, `onOutput`
- `tokeniseCommandLine(line)` — split a command line JUCE's way
- `BuildRunner` (`BuildRunner.h`) — one command rerun on demand

## Threads — `Core/Threads/`

- `callAsync(func)` — run on the main thread
- `callAfter(MS, func)` — run on the main thread later
- `Timer(cb, MS | Hz)` — periodic callback
- `Thread` — joining thread, `start`, `join`
- `Async<T>` / `AsyncPromise<T>` — `then(onValue, onError)`, `resolve`, `reject`
- `runEventLoopFor(MS)` / `runEventLoopUntil(predicate, ...)` — pump the loop
- `BinarySemaphore` / `TaskSemaphore` (`TaskSemaphore.h`) — wait with timeout

## Plugins — `Core/Plugins/`

- `DynamicLibrary` — `open`, `findSymbol`, `findFunction<F>`, `getFunctionNames`
- `Plugins::unload(library, quiesce)` — close safely
- `Reloadable` / `EACP_RELOADABLE` — hot reload (`Docs/HotReload.md`)

## HTTP — `Network/HTTP/`, `Network/HTTPServer/` (`eacp::HTTP`)

- `httpRequest(req)` / `Request::perform()` — blocking request
- `downloadFile(req, path)` — stream a response to a file, resumable
- `asyncRequest` / `asyncDownload` (`HttpAsync.h`) — callback on the main thread
- `Request::post`, `addFormField`, `addFileField`, `addFileBytes` — multipart body
- `URL` — `parse`, `addParameter`, `getParameter`, `query`, `toString`
- `urlEncode` / `urlDecode` / `parseQueryString` / `parseQueryParameters`
- `setDefaultUserAgent` / `eacpUserAgent`
- `Server` — `get`, `post`, `put`, `del`, `addRoute`, `listen`, `boundPort`
- `Json::Server` / `Json::makeHandler` / `Json::setJson` — reflected JSON routes
- `throwError(message, status)` / `makePlainTextResponse` — error responses
- `Rpc::Server` / `Rpc::Client` (`HTTPRpc/`) — RPC over HTTP
- Not here: a server-side multipart parser

## Other networking — `Network/`

- `WebSocket::Connection` — `send`, `sendBinary`, `close`, `Callbacks`
- `WebSocket::Server` — `listen`, `send`, `broadcast`, `close`, `clientCount`
- `TCP::Connection` / `TCP::Listener` — `connect`, `send`, `receiveLine`, `accept`
- `IPC::Messenger` / `MessageServer` — named local message channel
- `IPC::Lock` / `ScopedLock` — named cross-process lock
- `OnlineResource` / `OnlineResources` — fetch-once files, registry

## ea_data_structures (`EA::`, re-exported into `eacp`)

- `Vector<T>` — `add`, `create`, `insert`, `contains`, `addIfNotThere`,
  `removeAllMatches`, `removeIndexesMatching`, `addFrom`, `size`
- `OwnedVector<T>` — vector of owned pointers, `createNew`, `removeItem`
- `Array<T, N>`, `Span<T>`, `OwningPointer<T>` / `makeOwned`
- `MapUtils::get(map, key)` — pointer or null; `get(map, key, default)` — value
- `MapUtils::contains(map, key)`
- `Vectors::` — `find`, `findIf`, `contains`, `getIndexOf`, `getIndexIf`, `countIf`,
  `eraseIf`, `removeFirstMatch`, `removeAt`, `filter`, `transform`, `fold`,
  `sort`, `stableSort`, `insertSorted`, `lowerBoundIndex`, `reverse`, `fill`
- `Ranges::map` / `linearMap` — rescale a value between ranges
- `Broadcaster` / `Listener` — change notification
- `Structures/`: `Fifo`, `CircularBuffer`, `StaticVector`, `SmallVector`,
  `MapVector`, `CopyOnWrite`

## Miro (`Miro::`)

- `MIRO_REFLECT(fields...)` — make a struct serialisable
- `toJSON(value)` / `fromJSON(value, json)` / `createFromJSON<T>(json)`
- `toJSONString(value, indent)` / `createFromJSONString<T>(text)`
- `Json::parse(text)` / `Json::print(value)` — raw JSON values
- `toXML` / `toXMLString` / `createFromXML` / `createFromXMLString`
- `enumNames<T>()` / `enumToString` / `enumFromString<T>` — enum text
- Not here: a binary serialiser
