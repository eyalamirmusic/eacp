#include "Camera.h"

#include <eacp/Core/Android/Jni.h>
#include <eacp/Core/Android/Permissions-Android.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Logging.h>

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace eacp::Cameras
{
namespace
{
constexpr auto cameraPermission = "android.permission.CAMERA";
constexpr auto maxReaderImages = 4;
constexpr auto displayRotationRefresh = std::chrono::seconds {1};

bool succeeded(camera_status_t status, const char* what)
{
    if (status == ACAMERA_OK)
        return true;

    LOG("Camera: ", what, " failed (", (int) status, ")");
    return false;
}

bool succeeded(media_status_t status, const char* what)
{
    if (status == AMEDIA_OK)
        return true;

    LOG("Camera: ", what, " failed (", (int) status, ")");
    return false;
}

template <auto Release>
struct NdkDeleter
{
    template <typename T>
    void operator()(T* object) const
    {
        Release(object);
    }
};

using ManagerPtr =
    std::unique_ptr<ACameraManager, NdkDeleter<ACameraManager_delete>>;
using MetadataPtr =
    std::unique_ptr<ACameraMetadata, NdkDeleter<ACameraMetadata_free>>;
using IdListPtr =
    std::unique_ptr<ACameraIdList, NdkDeleter<ACameraManager_deleteCameraIdList>>;
using DevicePtr = std::unique_ptr<ACameraDevice, NdkDeleter<ACameraDevice_close>>;
using ReaderPtr = std::unique_ptr<AImageReader, NdkDeleter<AImageReader_delete>>;
using ImagePtr = std::unique_ptr<AImage, NdkDeleter<AImage_delete>>;
using OutputContainerPtr =
    std::unique_ptr<ACaptureSessionOutputContainer,
                    NdkDeleter<ACaptureSessionOutputContainer_free>>;
using OutputPtr =
    std::unique_ptr<ACaptureSessionOutput, NdkDeleter<ACaptureSessionOutput_free>>;
using TargetPtr =
    std::unique_ptr<ACameraOutputTarget, NdkDeleter<ACameraOutputTarget_free>>;
using RequestPtr =
    std::unique_ptr<ACaptureRequest, NdkDeleter<ACaptureRequest_free>>;
using SessionPtr =
    std::unique_ptr<ACameraCaptureSession, NdkDeleter<ACameraCaptureSession_close>>;

struct FrameSize
{
    int width = 0;
    int height = 0;
    double maxFrameRate = 0.0;
};

struct FpsRange
{
    int low = 0;
    int high = 0;
};

struct DeviceInfo
{
    CameraDevice device;
    int sensorOrientation = 0;
    Vector<FrameSize> sizes;
    Vector<FpsRange> fpsRanges;
};

ManagerPtr makeManager()
{
    return ManagerPtr {ACameraManager_create()};
}

Vector<std::string> cameraIds(ACameraManager* manager)
{
    auto result = Vector<std::string> {};
    ACameraIdList* list = nullptr;

    if (!succeeded(ACameraManager_getCameraIdList(manager, &list), "getCameraIdList")
        || list == nullptr)
        return result;

    auto owned = IdListPtr {list};

    for (auto i = 0; i < owned->numCameras; ++i)
        result.add(owned->cameraIds[i]);

    return result;
}

ACameraMetadata_const_entry entryFor(const ACameraMetadata* metadata,
                                     std::uint32_t tag)
{
    auto entry = ACameraMetadata_const_entry {};

    if (ACameraMetadata_getConstEntry(metadata, tag, &entry) != ACAMERA_OK)
        entry.count = 0;

    return entry;
}

std::string nameFor(const std::string& id, bool isFront, bool isExternal)
{
    if (isExternal)
        return "External camera (" + id + ")";

    return (isFront ? "Front camera (" : "Back camera (") + id + ")";
}

double minFrameDurationRate(const ACameraMetadata* metadata, int width, int height)
{
    auto durations =
        entryFor(metadata, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS);

    for (auto i = 0u; i + 3 < durations.count; i += 4)
    {
        const auto* item = durations.data.i64 + i;

        if (item[0] == AIMAGE_FORMAT_YUV_420_888 && item[1] == width
            && item[2] == height && item[3] > 0)
            return 1.0e9 / (double) item[3];
    }

    return 0.0;
}

std::optional<DeviceInfo> describe(ACameraManager* manager, const std::string& id)
{
    ACameraMetadata* raw = nullptr;

    if (!succeeded(
            ACameraManager_getCameraCharacteristics(manager, id.c_str(), &raw),
            "getCameraCharacteristics")
        || raw == nullptr)
        return std::nullopt;

    auto metadata = MetadataPtr {raw};
    auto info = DeviceInfo {};

    auto facing = entryFor(metadata.get(), ACAMERA_LENS_FACING);
    auto facingValue =
        facing.count > 0 ? (int) facing.data.u8[0] : (int) ACAMERA_LENS_FACING_BACK;
    auto isFront = facingValue == ACAMERA_LENS_FACING_FRONT;
    auto isExternal = facingValue == ACAMERA_LENS_FACING_EXTERNAL;

    info.device.id = id;
    info.device.isFrontFacing = isFront;
    info.device.name = nameFor(id, isFront, isExternal);

    auto orientation = entryFor(metadata.get(), ACAMERA_SENSOR_ORIENTATION);

    if (orientation.count > 0)
        info.sensorOrientation = orientation.data.i32[0];

    auto ranges =
        entryFor(metadata.get(), ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
    auto fastest = 0;

    for (auto i = 0u; i + 1 < ranges.count; i += 2)
    {
        auto range = FpsRange {ranges.data.i32[i], ranges.data.i32[i + 1]};
        info.fpsRanges.add(range);
        fastest = std::max(fastest, range.high);
    }

    auto configs =
        entryFor(metadata.get(), ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);

    for (auto i = 0u; i + 3 < configs.count; i += 4)
    {
        const auto* item = configs.data.i32 + i;

        if (item[0] != AIMAGE_FORMAT_YUV_420_888
            || item[3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT)
            continue;

        auto size = FrameSize {item[1], item[2], (double) fastest};
        auto sizeRate =
            minFrameDurationRate(metadata.get(), size.width, size.height);

        if (sizeRate > 0.0 && (fastest == 0 || sizeRate < fastest))
            size.maxFrameRate = sizeRate;

        info.sizes.add(size);
    }

    return info;
}

std::optional<DeviceInfo> chooseDevice(ACameraManager* manager,
                                       const std::optional<std::string>& wanted)
{
    auto fallback = std::optional<DeviceInfo> {};

    for (const auto& id: cameraIds(manager))
    {
        auto info = describe(manager, id);

        if (!info.has_value())
            continue;

        if (wanted.has_value())
        {
            if (*wanted == id)
                return info;

            continue;
        }

        if (!info->device.isFrontFacing)
            return info;

        if (!fallback.has_value())
            fallback = info;
    }

    return fallback;
}

FrameSize chooseSize(const DeviceInfo& info, const CameraConfig& config)
{
    auto best = FrameSize {config.width, config.height, 0.0};
    auto bestScore = std::numeric_limits<int>::max();

    for (const auto& size: info.sizes)
    {
        auto score = std::abs(size.width - config.width)
                     + std::abs(size.height - config.height);

        if (score < bestScore)
        {
            bestScore = score;
            best = size;
        }
    }

    return best;
}

std::optional<FpsRange> chooseFpsRange(const DeviceInfo& info, double frameRate)
{
    auto best = std::optional<FpsRange> {};
    auto bestDistance = std::numeric_limits<double>::max();

    for (const auto& range: info.fpsRanges)
    {
        auto distance = std::abs((double) range.high - frameRate);
        auto steadier =
            best.has_value() && distance == bestDistance && range.low > best->low;

        if (distance < bestDistance || steadier)
        {
            bestDistance = distance;
            best = range;
        }
    }

    return best;
}

struct DisplayJava
{
    void resolve(Jni::Lookup& java)
    {
        activity = java.findClass("android/app/Activity");
        display = java.findClass("android/view/Display");

        getDisplay = java.method(activity, "getDisplay", "()Landroid/view/Display;");
        getRotation = java.method(display, "getRotation", "()I");
    }

    jclass activity = nullptr;
    jclass display = nullptr;

    jmethodID getDisplay = nullptr;
    jmethodID getRotation = nullptr;
};

std::optional<int> readDisplayRotationDegrees()
{
    auto* env = Jni::currentEnv();

    if (env == nullptr)
        return std::nullopt;

    const auto* java = Jni::resolveOnce<DisplayJava>(env);

    if (java == nullptr)
        return std::nullopt;

    auto frame = Jni::LocalFrame {env};
    auto activity = Jni::activity(env);

    if (activity == nullptr)
        return std::nullopt;

    auto display = env->CallObjectMethod(activity, java->getDisplay);

    if (Jni::failed(env) || display == nullptr)
        return std::nullopt;

    auto rotation = env->CallIntMethod(display, java->getRotation);

    if (Jni::failed(env))
        return std::nullopt;

    return (rotation & 3) * 90;
}

int uprightRotation(int sensorOrientation, int displayDegrees, bool isFront)
{
    if (isFront)
        return (sensorOrientation + displayDegrees) % 360;

    return (sensorOrientation - displayDegrees + 360) % 360;
}

struct Plane
{
    std::uint8_t* data = nullptr;
    int length = 0;
    std::int32_t rowStride = 0;
    std::int32_t pixelStride = 0;
};

std::optional<Plane> planeOf(const AImage* image, int index)
{
    auto plane = Plane {};

    if (AImage_getPlaneData(image, index, &plane.data, &plane.length) != AMEDIA_OK
        || AImage_getPlaneRowStride(image, index, &plane.rowStride) != AMEDIA_OK
        || AImage_getPlanePixelStride(image, index, &plane.pixelStride) != AMEDIA_OK
        || plane.data == nullptr)
        return std::nullopt;

    return plane;
}

bool holds(const Plane& plane, int rows, int columns)
{
    auto lastSample = (std::int64_t) (rows - 1) * plane.rowStride
                      + (std::int64_t) (columns - 1) * plane.pixelStride;

    return plane.pixelStride > 0 && lastSample < plane.length;
}

bool isInterleavedCbCr(const Plane& u, const Plane& v)
{
    return u.pixelStride == 2 && v.pixelStride == 2 && v.data == u.data + 1
           && u.rowStride == v.rowStride;
}

bool packNv12(const AImage* image, int width, int height, Vector<std::uint8_t>& out)
{
    auto luma = planeOf(image, 0);
    auto cb = planeOf(image, 1);
    auto cr = planeOf(image, 2);

    if (!luma || !cb || !cr)
        return false;

    auto chromaWidth = width / 2;
    auto chromaHeight = height / 2;
    auto lumaBytes = width * height;

    if (!holds(*luma, height, width) || !holds(*cb, chromaHeight, chromaWidth)
        || !holds(*cr, chromaHeight, chromaWidth))
        return false;

    out.resize(lumaBytes + width * chromaHeight);

    auto* dst = out.data();

    for (auto y = 0; y < height; ++y)
        std::memcpy(
            dst + y * width, luma->data + y * luma->rowStride, (std::size_t) width);

    auto* chroma = dst + lumaBytes;

    if (isInterleavedCbCr(*cb, *cr))
    {
        for (auto y = 0; y < chromaHeight; ++y)
            std::memcpy(chroma + y * width,
                        cb->data + y * cb->rowStride,
                        (std::size_t) (chromaWidth * 2));

        return true;
    }

    for (auto y = 0; y < chromaHeight; ++y)
    {
        const auto* cbRow = cb->data + y * cb->rowStride;
        const auto* crRow = cr->data + y * cr->rowStride;
        auto* row = chroma + y * width;

        for (auto x = 0; x < chromaWidth; ++x)
        {
            row[2 * x] = cbRow[x * cb->pixelStride];
            row[2 * x + 1] = crRow[x * cr->pixelStride];
        }
    }

    return true;
}
} // namespace

// Published by swapping the freshly packed buffer in, so the bytes the frame
// callback and the render thread read are never written while they do.
struct LatestFrame
{
    const std::uint8_t*
        publish(Vector<std::uint8_t>& packed, int width, int height, int rotation)
    {
        std::lock_guard<std::mutex> lock(mutex);

        std::swap(data, packed);
        frameWidth = width;
        frameHeight = height;
        rotationDegrees = rotation;
        ++sequence;
        return data.data();
    }

    bool copyInto(FramePixels& out)
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (frameWidth <= 0 || sequence == out.sequence)
            return false;

        out.width = frameWidth;
        out.height = frameHeight;
        out.format = PixelFormat::NV12;
        out.data = data;
        out.sequence = sequence;
        out.rotationDegrees = rotationDegrees;
        out.yuvMatrix = YuvMatrix::BT601;
        out.fullRangeYuv = true;
        return true;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        frameWidth = 0;
        frameHeight = 0;
    }

    std::mutex mutex;
    Vector<std::uint8_t> data;
    int frameWidth = 0;
    int frameHeight = 0;
    int rotationDegrees = 0;
    std::uint64_t sequence = 0;
};

struct CaptureContext
{
    void setFrameArrived(const Callback& callbackToUse)
    {
        std::lock_guard<std::mutex> lock(callbacksMutex);
        frameArrived = callbackToUse;
    }

    void setFrameCallback(const FrameCallback& callbackToUse)
    {
        std::lock_guard<std::mutex> lock(callbacksMutex);
        frameCallback = callbackToUse;
    }

    void notifyFrameArrived()
    {
        auto arrived = Callback {};

        {
            std::lock_guard<std::mutex> lock(callbacksMutex);
            arrived = frameArrived;
        }

        arrived();
    }

    void deliver(const CameraFrame& frame)
    {
        auto callback = FrameCallback {};

        {
            std::lock_guard<std::mutex> lock(callbacksMutex);
            callback = frameCallback;
        }

        callback(frame);
    }

    LatestFrame latest;

    std::mutex callbacksMutex;
    Callback frameArrived = [] {};
    FrameCallback frameCallback = [](const CameraFrame&) {};
};

struct Camera::Native
{
    Native()
    {
        deviceCallbacks.context = this;
        deviceCallbacks.onDisconnected = [](void* self, ACameraDevice*)
        { static_cast<Native*>(self)->deviceLost("disconnected"); };
        deviceCallbacks.onError = [](void* self, ACameraDevice*, int error)
        {
            static_cast<Native*>(self)->deviceLost("error " + std::to_string(error));
        };

        sessionCallbacks.context = this;
        sessionCallbacks.onClosed = [](void*, ACameraCaptureSession*) {};
        sessionCallbacks.onReady = [](void*, ACameraCaptureSession*) {};
        sessionCallbacks.onActive = [](void*, ACameraCaptureSession*) {};

        imageListener.context = this;
        imageListener.onImageAvailable = [](void* self, AImageReader* source)
        { static_cast<Native*>(self)->onImage(source); };
    }

    ~Native()
    {
        *alive = false;
        stop();
    }

    bool start(const CameraConfig& config)
    {
        if (running.load())
            return true;

        if (!Android::hasPermission(cameraPermission))
        {
            static auto warned = std::atomic<bool> {false};

            if (!warned.exchange(true))
                LOG("Camera: start() without the CAMERA permission; call "
                    "Camera::requestPermission first");

            return false;
        }

        stop();
        ++generation;

        if (!open(config))
        {
            stop();
            return false;
        }

        running.store(true);
        return true;
    }

    bool open(const CameraConfig& config)
    {
        manager = makeManager();

        if (manager == nullptr)
        {
            LOG("Camera: no camera manager");
            return false;
        }

        auto info = chooseDevice(manager.get(), config.deviceId);

        if (!info.has_value())
        {
            LOG("Camera: no camera to open");
            return false;
        }

        sensorOrientation = info->sensorOrientation;
        frontFacing = info->device.isFrontFacing;
        discardLateFrames = config.discardLateFrames;
        displayReadAt = {};

        auto size = chooseSize(*info, config);

        ACameraDevice* rawDevice = nullptr;

        if (!succeeded(ACameraManager_openCamera(manager.get(),
                                                 info->device.id.c_str(),
                                                 &deviceCallbacks,
                                                 &rawDevice),
                       "openCamera"))
            return false;

        device.reset(rawDevice);

        AImageReader* rawReader = nullptr;

        if (!succeeded(AImageReader_new(size.width,
                                        size.height,
                                        AIMAGE_FORMAT_YUV_420_888,
                                        maxReaderImages,
                                        &rawReader),
                       "AImageReader_new"))
            return false;

        reader.reset(rawReader);

        if (!succeeded(AImageReader_setImageListener(reader.get(), &imageListener),
                       "AImageReader_setImageListener"))
            return false;

        ANativeWindow* window = nullptr;

        if (!succeeded(AImageReader_getWindow(reader.get(), &window),
                       "AImageReader_getWindow")
            || window == nullptr)
            return false;

        setDelivering(true);
        return createSession(window, chooseFpsRange(*info, config.frameRate));
    }

    bool createSession(ANativeWindow* window, const std::optional<FpsRange>& fps)
    {
        ACaptureSessionOutputContainer* rawOutputs = nullptr;
        ACaptureSessionOutput* rawOutput = nullptr;
        ACameraOutputTarget* rawTarget = nullptr;
        ACaptureRequest* rawRequest = nullptr;

        if (!succeeded(ACaptureSessionOutputContainer_create(&rawOutputs),
                       "ACaptureSessionOutputContainer_create"))
            return false;

        outputs.reset(rawOutputs);

        if (!succeeded(ACaptureSessionOutput_create(window, &rawOutput),
                       "ACaptureSessionOutput_create"))
            return false;

        output.reset(rawOutput);

        if (!succeeded(
                ACaptureSessionOutputContainer_add(outputs.get(), output.get()),
                "ACaptureSessionOutputContainer_add")
            || !succeeded(ACameraOutputTarget_create(window, &rawTarget),
                          "ACameraOutputTarget_create"))
            return false;

        target.reset(rawTarget);

        if (!succeeded(ACameraDevice_createCaptureRequest(
                           device.get(), TEMPLATE_PREVIEW, &rawRequest),
                       "createCaptureRequest"))
            return false;

        request.reset(rawRequest);

        if (!succeeded(ACaptureRequest_addTarget(request.get(), target.get()),
                       "ACaptureRequest_addTarget"))
            return false;

        if (fps.has_value())
        {
            std::int32_t range[] = {fps->low, fps->high};
            succeeded(
                ACaptureRequest_setEntry_i32(
                    request.get(), ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, range),
                "setting the frame rate range");
        }

        ACameraCaptureSession* rawSession = nullptr;

        if (!succeeded(
                ACameraDevice_createCaptureSession(
                    device.get(), outputs.get(), &sessionCallbacks, &rawSession),
                "createCaptureSession"))
            return false;

        session.reset(rawSession);

        auto* requests = request.get();

        return succeeded(ACameraCaptureSession_setRepeatingRequest(
                             session.get(), nullptr, 1, &requests, nullptr),
                         "setRepeatingRequest");
    }

    void setDelivering(bool shouldDeliver)
    {
        std::lock_guard<std::mutex> lock(deliveryMutex);
        delivering = shouldDeliver;
    }

    void stop()
    {
        running.store(false);
        setDelivering(false);

        if (session != nullptr)
            ACameraCaptureSession_stopRepeating(session.get());

        session.reset();
        device.reset();
        request.reset();
        target.reset();

        if (outputs != nullptr && output != nullptr)
            ACaptureSessionOutputContainer_remove(outputs.get(), output.get());

        output.reset();
        outputs.reset();
        reader.reset();
        manager.reset();
        context.latest.clear();
    }

    void deviceLost(const std::string& reason)
    {
        LOG("Camera: device ", reason, "; stopping");
        running.store(false);

        auto guard = alive;
        auto lostGeneration = generation.load();
        auto stopOnMain = [this, guard, lostGeneration]
        {
            if (*guard && generation.load() == lostGeneration)
                stop();
        };

        Threads::callAsync(stopOnMain);
    }

    int displayRotationDegrees()
    {
        auto now = std::chrono::steady_clock::now();

        if (displayReadAt == std::chrono::steady_clock::time_point {}
            || now - displayReadAt >= displayRotationRefresh)
        {
            if (auto degrees = readDisplayRotationDegrees())
                displayDegrees = *degrees;

            displayReadAt = now;
        }

        return displayDegrees;
    }

    void onImage(AImageReader* source)
    {
        std::lock_guard<std::mutex> lock(deliveryMutex);

        if (!delivering)
            return;

        AImage* rawImage = nullptr;
        auto status = discardLateFrames
                          ? AImageReader_acquireLatestImage(source, &rawImage)
                          : AImageReader_acquireNextImage(source, &rawImage);

        if (status != AMEDIA_OK || rawImage == nullptr)
            return;

        auto image = ImagePtr {rawImage};
        auto width = std::int32_t {0};
        auto height = std::int32_t {0};
        auto timestampNs = std::int64_t {0};

        if (AImage_getWidth(image.get(), &width) != AMEDIA_OK
            || AImage_getHeight(image.get(), &height) != AMEDIA_OK || width <= 1
            || height <= 1)
            return;

        width &= ~1;
        height &= ~1;
        AImage_getTimestamp(image.get(), &timestampNs);

        if (!packNv12(image.get(), width, height, packed))
            return;

        image.reset();

        auto rotation = uprightRotation(
            sensorOrientation, displayRotationDegrees(), frontFacing);
        const auto* pixels = context.latest.publish(packed, width, height, rotation);

        context.notifyFrameArrived();

        auto frame = CameraFrame(width,
                                 height,
                                 PixelFormat::NV12,
                                 width,
                                 (double) timestampNs * 1.0e-9,
                                 pixels,
                                 nullptr,
                                 rotation,
                                 YuvMatrix::BT601,
                                 true);
        context.deliver(frame);
    }

    CaptureContext context;

    ACameraDevice_StateCallbacks deviceCallbacks {};
    ACameraCaptureSession_stateCallbacks sessionCallbacks {};
    AImageReader_ImageListener imageListener {};

    ManagerPtr manager;
    DevicePtr device;
    ReaderPtr reader;
    OutputContainerPtr outputs;
    OutputPtr output;
    TargetPtr target;
    RequestPtr request;
    SessionPtr session;

    std::atomic<bool> running {false};
    std::atomic<std::uint64_t> generation {0};
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    std::mutex deliveryMutex;
    bool delivering = false;

    int sensorOrientation = 0;
    bool frontFacing = false;
    bool discardLateFrames = true;

    Vector<std::uint8_t> packed;
    int displayDegrees = 0;
    std::chrono::steady_clock::time_point displayReadAt {};
};

Camera::Camera()
    : impl()
{
}

Camera::~Camera() = default;

Vector<CameraDevice> Camera::devices()
{
    auto result = Vector<CameraDevice> {};
    auto manager = makeManager();

    if (manager == nullptr)
        return result;

    for (const auto& id: cameraIds(manager.get()))
        if (auto info = describe(manager.get(), id))
            result.add(info->device);

    return result;
}

Vector<CameraFormat> Camera::supportedFormats(const CameraDevice& device)
{
    auto result = Vector<CameraFormat> {};
    auto manager = makeManager();

    if (manager == nullptr)
        return result;

    auto info = describe(manager.get(), device.id);

    if (!info.has_value())
        return result;

    for (const auto& size: info->sizes)
    {
        auto format = CameraFormat {};
        format.width = size.width;
        format.height = size.height;
        format.maxFrameRate = size.maxFrameRate;
        format.pixelFormat = PixelFormat::NV12;
        result.add(format);
    }

    return result;
}

PermissionStatus Camera::permissionStatus()
{
    return Android::hasPermission(cameraPermission) ? PermissionStatus::Granted
                                                    : PermissionStatus::Denied;
}

void Camera::requestPermission(std::function<void(bool)> onResult)
{
    if (!onResult)
        onResult = [](bool) {};

    Android::requestPermission(cameraPermission, onResult);
}

void Camera::setFrameCallback(FrameCallback callback)
{
    if (!callback)
        callback = [](const CameraFrame&) {};

    impl->context.setFrameCallback(callback);
}

void Camera::setFrameArrivedCallback(Callback callback)
{
    if (!callback)
        callback = [] {};

    impl->context.setFrameArrived(callback);
}

bool Camera::start(const CameraConfig& config)
{
    return impl->start(config);
}

void Camera::stop()
{
    impl->stop();
}

bool Camera::isRunning() const
{
    return impl->running.load();
}

void* Camera::nativeSession() const
{
    return impl->session.get();
}

void* Camera::acquireLatestPixelBuffer()
{
    return nullptr;
}

void Camera::releasePixelBuffer(void*) {}

bool Camera::copyLatestFrame(FramePixels& out)
{
    return impl->context.latest.copyInto(out);
}
} // namespace eacp::Cameras
