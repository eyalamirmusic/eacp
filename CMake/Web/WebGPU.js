// Requests the WebGPU adapter and device before main() runs, since both are
// asynchronous in a browser and eacp's Device is not. A --pre-js on every web
// executable that links eacp-gpu: it holds the program back with a run
// dependency until the device has come up (or has failed to), and leaves it as
// Module.preinitializedWebGPUDevice, where emscripten_webgpu_get_device finds
// it. A page or a node with no navigator.gpu leaves it unset, and every
// eacp::GPU::Device is then invalid.
//
// The device asks for every feature the adapter offers (but Chrome's
// experimental ones) and its highest limits, which is what the native backends
// take from their devices too. A validation error nothing captured is printed
// through the program's own stderr, so a test run shows it.

Module['preRun'] = [].concat(Module['preRun'] || [], [function()
{
    var gpu = typeof navigator !== 'undefined' ? navigator['gpu'] : undefined;

    if (!gpu)
        return;

    var report = function(text)
    {
        (Module['printErr'] || console.error)(text);
    };

    addRunDependency('eacp-webgpu');

    gpu['requestAdapter']({'powerPreference': 'high-performance'})
        .then(function(adapter)
        {
            if (!adapter)
                return null;

            var info = adapter['info'] || {};
            Module['eacpWebGPUAdapterName'] =
                [info['vendor'], info['architecture'], info['description']]
                    .filter(function(part) { return part; })
                    .join(' ') || 'WebGPU';

            var features = [];

            adapter['features'].forEach(function(feature)
            {
                if (feature.indexOf('chromium-') !== 0)
                    features.push(feature);
            });

            var limits = {};

            for (var name in adapter['limits'])
                if (typeof adapter['limits'][name] === 'number')
                    limits[name] = adapter['limits'][name];

            return adapter['requestDevice']({'requiredFeatures': features,
                                             'requiredLimits': limits});
        })
        .then(function(device)
        {
            if (!device)
                return;

            device.addEventListener('uncapturederror', function(event)
            {
                report('WebGPU: ' + event['error']['message']);
            });

            device['lost'].then(function(info)
            {
                if (info['reason'] !== 'destroyed')
                    report('WebGPU: the device was lost: ' + info['message']);
            });

            Module['preinitializedWebGPUDevice'] = device;
        })
        .catch(function(error)
        {
            report('WebGPU: no device: ' + error);
        })
        .finally(function()
        {
            removeRunDependency('eacp-webgpu');
        });
}]);
