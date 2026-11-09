#include "GameInputBackend.h"
#include "EvdevGamepad-Linux.h"
#include <eacp/Core/Utils/Logging.h>

#include <linux/input.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace eacp::Graphics
{
namespace
{
constexpr auto evdevDirectory = "/dev/input";
constexpr auto evdevSysfsDirectory = "/sys/class/input";

// udev creates a node owned by root:input and grants the seat's user access a
// moment later, so a pad that is refused on arrival is tried again on that
// attribute change, and reported as unreadable only if this passes first.
constexpr auto evdevPermissionGraceSeconds = 2.0;

// An event stamped further from now() than this is on some other clock.
constexpr auto evdevClockToleranceSeconds = 1.0;

constexpr auto longBits = sizeof(unsigned long) * 8;

// One GameInput's end of the process-wide feed. `wasAccepting` is the reader
// thread's alone. `resumed` is set on the main thread when the GameInput took
// focus back, since the reader may never have seen it lose focus: nothing
// wakes it for that.
struct EvdevSink
{
    GameInputQueue* queue = nullptr;
    const std::atomic<bool>* active = nullptr;
    std::atomic<bool> resumed {false};
    bool wasAccepting = false;

    bool accepting() const { return active->load(std::memory_order_relaxed); }
};

using Sink = std::shared_ptr<EvdevSink>;

class FileDescriptor
{
public:
    FileDescriptor() = default;

    explicit FileDescriptor(int fdToUse)
        : fd(fdToUse)
    {
    }

    FileDescriptor(FileDescriptor&& other) noexcept
        : fd(std::exchange(other.fd, -1))
    {
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept
    {
        if (this != &other)
        {
            close();
            fd = std::exchange(other.fd, -1);
        }

        return *this;
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    ~FileDescriptor() { close(); }

    int get() const { return fd; }
    bool isValid() const { return fd >= 0; }

private:
    void close()
    {
        if (fd >= 0)
            ::close(fd);

        fd = -1;
    }

    int fd = -1;
};

// The kernel's bitmask of `Bits` codes as EVIOCGBIT and EVIOCGKEY fill it.
template <std::size_t Bits>
struct EvdevBits
{
    static constexpr auto wordCount = (Bits + longBits - 1) / longBits;

    std::bitset<Bits> toBits() const
    {
        auto bits = std::bitset<Bits> {};

        for (auto bit = std::size_t {0}; bit < Bits; ++bit)
            if ((words[bit / longBits] >> (bit % longBits)) & 1UL)
                bits.set(bit);

        return bits;
    }

    unsigned long words[wordCount] {};
};

bool isEventNode(std::string_view name)
{
    constexpr auto prefix = std::string_view {"event"};

    if (!name.starts_with(prefix) || name.size() == prefix.size())
        return false;

    return std::all_of(name.begin() + (std::ptrdiff_t) prefix.size(),
                       name.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}

std::string devicePath(const std::string& node)
{
    return std::string {evdevDirectory} + "/" + node;
}

std::filesystem::path sysfsDevice(const std::string& node)
{
    return std::filesystem::path {evdevSysfsDirectory} / node / "device";
}

std::string firstLineOf(const std::filesystem::path& path)
{
    auto file = std::ifstream {path};
    auto line = std::string {};
    std::getline(file, line);
    return line;
}

// The driver bound to the device the input node hangs off: the HID or USB
// interface above it. A uinput device has none.
std::string driverOf(const std::string& node)
{
    auto error = std::error_code {};
    const auto target = std::filesystem::read_symlink(
        sysfsDevice(node) / "device" / "driver", error);

    return error ? std::string {} : target.filename().string();
}

// sysfs answers what a node can report without opening it, which is how a pad
// the user may not read is told apart from the keyboards they may not read
// either. The key bitmap is printed as hex longs, most significant first.
bool sysfsSaysGamepad(const std::string& node)
{
    auto words = std::vector<unsigned long> {};
    auto text =
        std::istringstream {firstLineOf(sysfsDevice(node) / "capabilities" / "key")};

    for (auto word = std::string {}; text >> word;)
        words.push_back(std::strtoul(word.c_str(), nullptr, 16));

    std::reverse(words.begin(), words.end());

    const auto index = (std::size_t) BTN_GAMEPAD / longBits;
    return index < words.size()
           && ((words[index] >> (BTN_GAMEPAD % longBits)) & 1UL) != 0;
}

EvdevDeviceInfo readDeviceInfo(int fd, const std::string& node)
{
    auto info = EvdevDeviceInfo {};
    auto keys = EvdevBits<KEY_CNT> {};
    auto axes = EvdevBits<ABS_CNT> {};
    auto properties = EvdevBits<INPUT_PROP_CNT> {};

    if (::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys.words)), keys.words) >= 0)
        info.keys = keys.toBits();

    if (::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(axes.words)), axes.words) >= 0)
        info.axes = axes.toBits();

    if (::ioctl(fd, EVIOCGPROP(sizeof(properties.words)), properties.words) >= 0)
        info.properties = properties.toBits();

    for (auto code = 0; code < ABS_CNT; ++code)
    {
        auto abs = input_absinfo {};

        if (info.axes.test((std::size_t) code)
            && ::ioctl(fd, EVIOCGABS(code), &abs) >= 0)
            info.ranges[code] = {abs.minimum, abs.maximum};
    }

    auto id = input_id {};

    if (::ioctl(fd, EVIOCGID, &id) >= 0)
    {
        info.vendor = id.vendor;
        info.product = id.product;
    }

    info.driver = driverOf(node);
    return info;
}

std::string nameOf(int fd)
{
    char name[256] {};

    if (::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
        return "unnamed";

    return name;
}

const char* familyName(GamepadFamily family)
{
    switch (family)
    {
        case GamepadFamily::Xbox:
            return "Xbox";
        case GamepadFamily::PlayStation:
            return "PlayStation";
        case GamepadFamily::Nintendo:
            return "Nintendo";
        default:
            return "generic";
    }
}

std::string hex4(uint16_t value)
{
    return Strings::format("{:04x}", value);
}

// One open gamepad node, the reader thread's own but for what `add` reads
// under the hub's lock.
struct EvdevDevice
{
    FileDescriptor fd;
    std::string node;
    int id = -1;
    EvdevGamepad pad;
    bool monotonicClock = false;
    bool dropping = false;
};

struct PendingNode
{
    std::string node;
    double retrySeconds = 0.0;
};

// evdev nodes are process-wide and every GameInput wants the same pads, so the
// first GameInput opens them and starts the one reader thread, and every
// GameInput's sink is fed from it; the last one to leave stops it. Main thread
// only, but for the reader.
//
// The reader polls the pads, an inotify watch on /dev/input for hotplug, and
// an eventfd that stops it or tells it a sink started accepting again.
// Connections and disconnections reach every sink; buttons and axes only the
// accepting ones, and a sink that starts accepting again is handed the whole
// state of every pad, since GameInput released everything while it was not.
class EvdevHub
{
public:
    // Null when the hub could not make the descriptor it is stopped with.
    static EvdevHub* join(const Sink& sink)
    {
        if (instance == nullptr)
        {
            auto wake = FileDescriptor {::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};

            if (!wake.isValid())
                return nullptr;

            instance = new EvdevHub(std::move(wake));
        }

        instance->add(sink);
        return instance;
    }

    static void leave(const Sink& sink)
    {
        if (instance == nullptr || !instance->remove(sink))
            return;

        delete instance;
        instance = nullptr;
    }

    // Main thread: a sink may have started accepting.
    void wake() const
    {
        const auto one = uint64_t {1};
        const auto written = ::write(wakeFd.get(), &one, sizeof(one));
        (void) written;
    }

private:
    explicit EvdevHub(FileDescriptor wakeToUse)
        : wakeFd(std::move(wakeToUse))
        , notifyFd(::inotify_init1(IN_CLOEXEC | IN_NONBLOCK))
    {
        if (notifyFd.isValid())
        {
            const auto mask = IN_CREATE | IN_ATTRIB | IN_DELETE | IN_MOVED_TO;

            if (::inotify_add_watch(notifyFd.get(), evdevDirectory, mask) < 0)
                notifyFd = FileDescriptor {};
        }

        // Before the reader starts, so the pads already plugged in reach the
        // first sink as it joins.
        scan(false);
        reader = std::thread([this] { run(); });
    }

    ~EvdevHub()
    {
        stopping.store(true);
        wake();
        reader.join();
    }

    void add(const Sink& sink)
    {
        auto lock = std::scoped_lock {mutex};
        const auto timestampSeconds = GameInputQueue::now();

        sinks.push_back(sink);
        sink->wasAccepting = sink->accepting();

        for (auto& device: devices)
        {
            connect(*sink, *device, timestampSeconds);

            if (sink->wasAccepting)
                device->pad.pushTo(*sink->queue, device->id, timestampSeconds);
        }

        ++joined;
    }

    // Once this returns nothing pushes into the sink's queue. True when it
    // was the last.
    bool remove(const Sink& sink)
    {
        auto lock = std::scoped_lock {mutex};
        std::erase(sinks, sink);
        return --joined == 0;
    }

    void run()
    {
        pthread_setname_np(pthread_self(), "eacp-gamepads");

        while (!stopping.load())
            waitAndHandle();
    }

    void waitAndHandle()
    {
        auto fds = std::vector<pollfd> {};
        fds.push_back({wakeFd.get(), POLLIN, 0});
        fds.push_back({notifyFd.get(), POLLIN, 0});

        for (auto& device: devices)
            fds.push_back({device->fd.get(), POLLIN, 0});

        const auto ready = ::poll(fds.data(), (nfds_t) fds.size(), pollTimeoutMs());

        if (ready < 0 && errno != EINTR)
            return;

        if (fds[0].revents != 0)
        {
            auto count = uint64_t {};
            const auto drained = ::read(wakeFd.get(), &count, sizeof(count));
            (void) drained;

            if (stopping.load())
                return;

            auto lock = std::scoped_lock {mutex};
            resumeSinks(GameInputQueue::now());
        }

        // Devices before hotplug, so a node that was unplugged and reused is
        // closed before its new device is looked at.
        auto gone = std::vector<std::string> {};

        for (auto index = std::size_t {2}; index < fds.size(); ++index)
        {
            auto& device = *devices[index - 2];

            if (fds[index].revents != 0 && !readDevice(device))
                gone.push_back(device.node);
        }

        for (auto& node: gone)
            detach(node);

        if (fds[1].revents != 0)
            readHotplug();

        retryPending();
    }

    int pollTimeoutMs() const
    {
        if (pending.empty())
            return -1;

        auto earliest = pending.front().retrySeconds;

        for (auto& entry: pending)
            earliest = std::min(earliest, entry.retrySeconds);

        const auto wait = earliest - GameInputQueue::now();
        return wait <= 0.0 ? 0 : (int) (wait * 1000.0) + 1;
    }

    // Every node in /dev/input, as at start or after inotify lost track.
    void scan(bool hotplugged)
    {
        auto error = std::error_code {};

        for (auto& entry:
             std::filesystem::directory_iterator {evdevDirectory, error})
        {
            const auto node = entry.path().filename().string();

            if (isEventNode(node))
                tryOpen(node, hotplugged);
        }
    }

    void readHotplug()
    {
        alignas(inotify_event) char buffer[4096];

        while (true)
        {
            const auto length = ::read(notifyFd.get(), buffer, sizeof(buffer));

            if (length <= 0)
                return;

            for (auto offset = 0L; offset < length;)
            {
                const auto* event =
                    reinterpret_cast<const inotify_event*>(buffer + offset);
                offset += (long) (sizeof(inotify_event) + event->len);

                if ((event->mask & IN_Q_OVERFLOW) != 0)
                {
                    ignored.clear();
                    scan(true);
                    continue;
                }

                const auto node =
                    event->len > 0 ? std::string {event->name} : std::string {};

                if (!isEventNode(node))
                    continue;

                if ((event->mask & IN_DELETE) != 0)
                {
                    ignored.erase(node);
                    detach(node);
                    continue;
                }

                if ((event->mask & (IN_CREATE | IN_MOVED_TO)) != 0)
                    ignored.erase(node);

                tryOpen(node, true);
            }
        }
    }

    void retryPending()
    {
        const auto timestampSeconds = GameInputQueue::now();
        auto due = std::vector<std::string> {};

        std::erase_if(pending,
                      [&](const PendingNode& entry)
                      {
                          if (entry.retrySeconds > timestampSeconds)
                              return false;

                          due.push_back(entry.node);
                          return true;
                      });

        for (auto& node: due)
            tryOpen(node, false);
    }

    bool isOpen(const std::string& node) const
    {
        return std::any_of(devices.begin(),
                           devices.end(),
                           [&](const auto& device) { return device->node == node; });
    }

    void tryOpen(const std::string& node, bool hotplugged)
    {
        if (isOpen(node) || ignored.contains(node))
            return;

        auto fd = FileDescriptor {
            ::open(devicePath(node).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)};

        if (!fd.isValid())
        {
            refused(node, errno, hotplugged);
            return;
        }

        std::erase_if(pending,
                      [&](const auto& entry) { return entry.node == node; });

        const auto info = readDeviceInfo(fd.get(), node);

        if (!isEvdevGamepad(info))
        {
            ignored.insert(node);
            return;
        }

        attach(std::move(fd), node, info);
    }

    // A refusal is worth a line only for a pad, and only once: the reader
    // keeps no device it cannot open, so the game would otherwise just never
    // see the controller.
    void refused(const std::string& node, int error, bool hotplugged)
    {
        if ((error != EACCES && error != EPERM) || !sysfsSaysGamepad(node))
            return;

        if (hotplugged)
        {
            std::erase_if(pending,
                          [&](const auto& entry) { return entry.node == node; });
            pending.push_back(
                {node, GameInputQueue::now() + evdevPermissionGraceSeconds});
            return;
        }

        if (!warned.insert(node).second)
            return;

        LOG("GameInput: gamepad \"",
            firstLineOf(sysfsDevice(node) / "name"),
            "\" at ",
            devicePath(node),
            " cannot be read (",
            std::strerror(error),
            "): it needs the uaccess ACL a desktop session grants, or the user "
            "in the group that owns the node (usually 'input')");
    }

    void attach(FileDescriptor fd,
                const std::string& node,
                const EvdevDeviceInfo& info)
    {
        auto clock = (int) CLOCK_MONOTONIC;
        const auto monotonic = ::ioctl(fd.get(), EVIOCSCLOCKID, &clock) == 0;
        const auto name = nameOf(fd.get());

        auto device = std::unique_ptr<EvdevDevice>(new EvdevDevice {
            std::move(fd), node, nextGamepadId++, EvdevGamepad {info}, monotonic});

        readCurrentState(*device);

        LOG("GameInput: gamepad ",
            device->id,
            " connected: ",
            name,
            " (",
            devicePath(node),
            ", ",
            info.driver.empty() ? std::string {"no driver"} : info.driver,
            ", ",
            hex4(info.vendor),
            ":",
            hex4(info.product),
            "), ",
            familyName(device->pad.family()));

        auto lock = std::scoped_lock {mutex};
        const auto timestampSeconds = GameInputQueue::now();

        for (auto& sink: sinks)
        {
            connect(*sink, *device, timestampSeconds);

            if (sink->accepting())
                device->pad.pushTo(*sink->queue, device->id, timestampSeconds);
        }

        devices.push_back(std::move(device));
    }

    void detach(const std::string& node)
    {
        auto lock = std::scoped_lock {mutex};
        const auto found =
            std::find_if(devices.begin(),
                         devices.end(),
                         [&](const auto& device) { return device->node == node; });

        if (found == devices.end())
            return;

        const auto id = (*found)->id;
        LOG("GameInput: gamepad ", id, " disconnected: ", devicePath(node));

        for (auto& sink: sinks)
            sink->queue->gamepadDisconnected(id, GameInputQueue::now());

        devices.erase(found);
    }

    // The device's held keys and axis positions as it reports them now: on
    // opening it, and after a SYN_DROPPED lost some of the events between.
    static void readCurrentState(EvdevDevice& device)
    {
        auto keys = EvdevBits<KEY_CNT> {};
        device.pad.reset();

        if (::ioctl(device.fd.get(), EVIOCGKEY(sizeof(keys.words)), keys.words) >= 0)
        {
            const auto held = keys.toBits();

            for (auto code = 0; code < KEY_CNT; ++code)
                if (held.test((std::size_t) code))
                    device.pad.apply(EV_KEY, (uint16_t) code, 1);
        }

        for (auto code = 0; code < ABS_CNT; ++code)
        {
            auto abs = input_absinfo {};

            if (::ioctl(device.fd.get(), EVIOCGABS(code), &abs) >= 0)
                device.pad.apply(EV_ABS, (uint16_t) code, abs.value);
        }
    }

    // False once the device is gone (ENODEV) or failing.
    bool readDevice(EvdevDevice& device)
    {
        input_event events[64];

        while (true)
        {
            const auto length = ::read(device.fd.get(), events, sizeof(events));

            if (length < 0 && errno == EINTR)
                continue;

            if (length < 0)
                return errno == EAGAIN || errno == EWOULDBLOCK;

            if (length == 0)
                return false;

            const auto count = (std::size_t) length / sizeof(input_event);
            auto lock = std::scoped_lock {mutex};

            for (auto index = std::size_t {0}; index < count; ++index)
                handle(device, events[index]);
        }
    }

    // Under the lock. A SYN_DROPPED means the kernel's buffer overflowed: what
    // follows up to the next SYN_REPORT is a partial packet, so it is skipped
    // and the state read whole from the device instead.
    void handle(EvdevDevice& device, const input_event& event)
    {
        if (event.type != EV_SYN)
        {
            if (!device.dropping)
                device.pad.apply(event.type, event.code, event.value);

            return;
        }

        if (event.code == SYN_DROPPED)
        {
            device.dropping = true;
            return;
        }

        if (event.code != SYN_REPORT)
            return;

        if (device.dropping)
        {
            device.dropping = false;
            readCurrentState(device);
        }

        const auto timestampSeconds = timestampOf(device, event);
        resumeSinks(timestampSeconds);

        for (auto& sink: sinks)
            if (sink->accepting())
                device.pad.pushTo(*sink->queue, device.id, timestampSeconds);
    }

    // The kernel's own stamp of the packet when it is on steady_clock's
    // CLOCK_MONOTONIC, so input is timed by when it happened rather than when
    // the reader woke.
    static double timestampOf(const EvdevDevice& device, const input_event& event)
    {
        const auto now = GameInputQueue::now();

        if (!device.monotonicClock)
            return now;

        const auto stamped =
            (double) event.input_event_sec + (double) event.input_event_usec * 1e-6;

        if (stamped > now || now - stamped > evdevClockToleranceSeconds)
            return now;

        return stamped;
    }

    // Under the lock.
    void resumeSinks(double timestampSeconds)
    {
        for (auto& sink: sinks)
        {
            const auto resumed = sink->resumed.exchange(false);
            const auto accepting = sink->accepting();

            if (accepting && (resumed || !sink->wasAccepting))
                for (auto& device: devices)
                    device->pad.pushTo(*sink->queue, device->id, timestampSeconds);

            sink->wasAccepting = accepting;
        }
    }

    // Linux shows no player number on a pad, so there is none to report.
    static void connect(EvdevSink& sink, const EvdevDevice& device, double timestamp)
    {
        sink.queue->gamepadConnected(device.id, device.pad.family(), -1, timestamp);
    }

    static inline EvdevHub* instance = nullptr;
    static inline int nextGamepadId = 0;

    FileDescriptor wakeFd;
    FileDescriptor notifyFd;
    std::mutex mutex;
    std::vector<Sink> sinks;
    std::vector<std::unique_ptr<EvdevDevice>> devices;
    std::vector<PendingNode> pending;
    std::set<std::string> ignored;
    std::set<std::string> warned;
    std::atomic<bool> stopping {false};
    int joined = 0;
    std::thread reader;
};

// Keys and mouse stay the window's: evdev keyboards and mice belong to the
// compositor or the X server, and are rarely readable by the user anyway.
struct EvdevBackend final : GameInputBackend
{
    EvdevBackend(EvdevHub& hubToUse, Sink sinkToUse)
        : hub(hubToUse)
        , sink(std::move(sinkToUse))
    {
    }

    ~EvdevBackend() override { EvdevHub::leave(sink); }

    bool ownsKeys() const override { return false; }
    bool ownsMouse() const override { return false; }

    void resumed() override
    {
        sink->resumed.store(true);
        hub.wake();
    }

    EvdevHub& hub;
    Sink sink;
};
} // namespace

std::unique_ptr<GameInputBackend>
    makeGameInputBackend(GameInputQueue& queue, const std::atomic<bool>& active)
{
    auto sink = std::make_shared<EvdevSink>();
    sink->queue = &queue;
    sink->active = &active;

    auto* hub = EvdevHub::join(sink);

    if (hub == nullptr)
        return nullptr;

    return std::make_unique<EvdevBackend>(*hub, std::move(sink));
}
} // namespace eacp::Graphics
