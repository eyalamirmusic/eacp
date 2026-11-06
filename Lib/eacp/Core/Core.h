#pragma once

/** @file
    Core: the app lifecycle, the main-thread event loop, timers, threads, child
    processes, dynamic libraries and hot reload, files, zip, hashing, logging
    and string utilities. Builds on every platform, Linux and Android included;
    nothing here is behind an EACP_HAS_* gate.

    Entry points: Apps::run<T>() for an app, Apps::runConsole() for a
    command-line tool, Threads::callAsync / callAfter / Timer for deferred work.

    Rules: callbacks (Timer, callAsync, Async continuations, BuildRunner) run on
    the main thread unless a header says otherwise. Read Docs/Toolbox.md before
    writing a helper; Core probably has it. Use Threads::Thread rather than
    std::thread in headers. Reloadable (Plugins/Reloadable.h) is included
    directly, never through this header.
*/

#include "App/Alert.h"
#include "App/App.h"
#include "App/Clipboard.h"
#include "App/LoginItem.h"
#include "Maths/Maths.h"
#include "Plugins/DynamicLibrary.h"
#include "Plugins/HotReloadLoop.h"
#include "Plugins/ModuleInfo.h"
#include "Plugins/PluginExport.h"
#include "Plugins/ReloadableLibrary.h"
#include "Process/BuildRunner.h"
#include "Process/Process.h"
#include "Threads/Thread.h"
#include "Threads/Timer.h"
#include "Utils/Base64.h"
#include "Utils/Broadcaster.h"
#include "Utils/Console.h"
#include "Utils/File.h"
#include "Utils/FileWatcher.h"
#include "Utils/FileWriter.h"
#include "Utils/Files.h"
#include "Utils/MemoryMappedFile.h"
#include "Utils/Random.h"
#include "Utils/SourceWatcher.h"
#include "Utils/Range.h"
#include "Utils/Sha256.h"
#include "Utils/Singleton.h"
#include "Utils/TemporaryFile.h"
#include "Utils/Zip.h"
