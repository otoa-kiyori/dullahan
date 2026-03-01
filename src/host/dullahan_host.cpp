/*
    @brief Dullahan - a headless browser rendering engine
           based around the Chromium Embedded Framework
    @author Callum Prentice 2017

    Copyright (c) 2017, Linden Research, Inc.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.
*/

#define NOMINMAX

#include "cef_app.h"
#include "cef_render_process_handler.h"

///////////////////////////////////////////////////////////////////////////////
// dullahan_privacy_app
//
// Render-process CefApp that installs a JS shim when the browser process has
// set the --dullahan-protect-privacy command-line switch.  The shim locks
// navigator properties and neutralises AudioContext/OfflineAudioContext
// fingerprinting before any page script can read those values.
//
// It is wired in as the |application| argument to CefExecuteProcess() on every
// platform so that the render sub-process has access to CefRenderProcessHandler
// callbacks.
///////////////////////////////////////////////////////////////////////////////

// JavaScript code registered as a V8 extension via CefRegisterExtension().
// Runs synchronously in every frame's V8 context before any page script.
static const char kPrivacyShimJS[] = R"js(
(function () {
    // Lock a property with a non-configurable, non-writable descriptor.
    // A try/catch guards against properties that are already non-configurable
    // with a different value (shouldn't happen, but safe to skip).
    function tryLock(obj, prop, value) {
        try {
            Object.defineProperty(obj, prop, {
                value: value,
                writable: false,
                configurable: false,
                enumerable: true
            });
        } catch (_) {}
    }

    // --- Navigator: hardware / device fingerprints ---
    // Return plausible but generic values that don't identify the host machine.
    tryLock(Navigator.prototype, 'hardwareConcurrency', 2);
    tryLock(Navigator.prototype, 'deviceMemory', 1);
    tryLock(Navigator.prototype, 'maxTouchPoints', 0);

    // --- Navigator: identity / automation fingerprints ---
    // platform reveals OS architecture; webdriver flags the browser as automated
    // (may be true in CEF builds); pdfViewerEnabled reveals plugin presence.
    // navigator.language/languages are intentionally left as-is — the SL viewer
    // passes its own locale setting through to CEF and must not be overridden.
    tryLock(Navigator.prototype, 'platform', 'Win32');
    tryLock(Navigator.prototype, 'pdfViewerEnabled', false);
    // webdriver: the C++ --disable-blink-features=AutomationControlled flag is the
    // authoritative fix.  This JS attempt is belt-and-suspenders for any CEF variant
    // where the flag has no effect.
    try { delete Navigator.prototype.webdriver; } catch (_) {}
    tryLock(Navigator.prototype, 'webdriver', false);

    // --- Navigator: plugin / MIME enumeration ---
    // The installed plugin list (navigator.plugins) and MIME type list
    // (navigator.mimeTypes) reveal OS state and are used by FingerprintJS.
    // Return empty array-like objects so no plugin information leaks.
    (function () {
        try {
            const _emptyPlugins = {
                length: 0,
                item: function () { return null; },
                namedItem: function () { return null; },
                refresh: function () {},
                [Symbol.iterator]: function* () {}
            };
            tryLock(Navigator.prototype, 'plugins', _emptyPlugins);
        } catch (_) {}
        try {
            const _emptyMimes = {
                length: 0,
                item: function () { return null; },
                namedItem: function () { return null; },
                [Symbol.iterator]: function* () {}
            };
            tryLock(Navigator.prototype, 'mimeTypes', _emptyMimes);
        } catch (_) {}
    })();

    // --- Navigator: network connection info ---
    // navigator.connection (NetworkInformation API) exposes downlink, effectiveType,
    // rtt, etc. that fingerprint network conditions.  Suppress it entirely.
    try {
        if (typeof Navigator !== 'undefined' && 'connection' in Navigator.prototype) {
            Object.defineProperty(Navigator.prototype, 'connection', {
                get: function () { return undefined; },
                configurable: false,
                enumerable: true
            });
        }
    } catch (_) {}

    // --- Screen: color depth ---
    // screen.colorDepth and screen.pixelDepth expose display hardware capabilities.
    // Lock to 24 (the universally common value on modern displays).
    try {
        if (typeof Screen !== 'undefined') {
            tryLock(Screen.prototype, 'colorDepth', 24);
            tryLock(Screen.prototype, 'pixelDepth', 24);
        }
    } catch (_) {}

    // --- Battery Status API ---
    // getBattery() is a fingerprint source; reject with a generic error so the
    // API appears absent without throwing a synchronous exception.
    if (typeof Navigator.prototype.getBattery === 'function') {
        Navigator.prototype.getBattery = function () {
            return Promise.reject(new Error('Not supported'));
        };
    }

    // --- AudioContext fingerprinting ---
    // FingerprintJS creates an OfflineAudioContext, runs an oscillator through
    // a DynamicsCompressor, and hashes the resulting Float32 sample data.
    // Intercept startRendering() and zero every channel of the rendered buffer
    // before the promise resolves so the caller always gets silence.
    if (typeof OfflineAudioContext !== 'undefined') {
        try {
            const _startRendering = OfflineAudioContext.prototype.startRendering;
            OfflineAudioContext.prototype.startRendering = function () {
                return _startRendering.call(this).then(function (buf) {
                    for (let c = 0; c < buf.numberOfChannels; c++) {
                        buf.getChannelData(c).fill(0);
                    }
                    return buf;
                });
            };
        } catch (_) {}
    }

    // Also cover the synchronous copyFromChannel path used by some fingerprinters.
    if (typeof AudioBuffer !== 'undefined' && AudioBuffer.prototype.copyFromChannel) {
        try {
            const _copyFromChannel = AudioBuffer.prototype.copyFromChannel;
            AudioBuffer.prototype.copyFromChannel = function (dest, ch, offset) {
                _copyFromChannel.call(this, dest, ch, offset);
                if (dest && dest.fill) { dest.fill(0); }
            };
        } catch (_) {}
    }

    // --- Canvas fingerprinting (Brave-style noise) ---
    // Replaces the blunt --disable-reading-from-canvas Chromium switch, which broke
    // legitimate canvas use (image export, QR scanners, drawing apps, etc.).
    //
    // Technique: one bit is flipped in one RGBA channel of every canvas pixel readback.
    // The channel index (_ch) is chosen once per V8 context — random per page load,
    // stable within it — so the fingerprint hash is different every visit while the
    // visible image is indistinguishable from the original.
    //
    // Covered surfaces:
    //   1. CanvasRenderingContext2D.getImageData        — direct pixel readback
    //   2. HTMLCanvasElement.toDataURL                  — most fingerprinting tests use this
    //   3. HTMLCanvasElement.toBlob                     — async variant of toDataURL
    //   4. WebGLRenderingContext.readPixels             — WebGL pixel readback
    //   5. WebGL2RenderingContext.readPixels            — WebGL2 variant
    (function () {
        // RGBA channel to corrupt — random per page load, constant within the session.
        const _ch = Math.floor(Math.random() * 4);

        // Flip one bit in the chosen channel of a pixel buffer.
        // Accepts Uint8Array (WebGL readPixels) or Uint8ClampedArray (ImageData.data).
        function _flipBit(buf) {
            if ((buf instanceof Uint8Array || buf instanceof Uint8ClampedArray) &&
                buf.length >= 4)
            {
                buf[_ch] ^= 1;
            }
        }

        // Render a canvas to a temporary copy with one flipped pixel so the original
        // canvas is never modified.  Returns null if the copy cannot be made (e.g. the
        // canvas is cross-origin tainted), in which case callers fall back to the real API.
        function _noisedCopy(src) {
            try {
                const tmp = document.createElement('canvas');
                tmp.width  = src.width;
                tmp.height = src.height;
                const ctx = tmp.getContext('2d');
                if (!ctx) { return null; }
                ctx.drawImage(src, 0, 0);
                const px = ctx.getImageData(0, 0, 1, 1);
                _flipBit(px.data);
                ctx.putImageData(px, 0, 0);
                return tmp;
            } catch (_) { return null; }
        }

        // 1. getImageData
        try {
            const _getImageData = CanvasRenderingContext2D.prototype.getImageData;
            CanvasRenderingContext2D.prototype.getImageData = function () {
                const d = _getImageData.apply(this, arguments);
                _flipBit(d.data);
                return d;
            };
        } catch (_) {}

        // 2. toDataURL
        try {
            const _toDataURL = HTMLCanvasElement.prototype.toDataURL;
            HTMLCanvasElement.prototype.toDataURL = function (type, quality) {
                if (this.width === 0 || this.height === 0) {
                    return _toDataURL.call(this, type, quality);
                }
                const tmp = _noisedCopy(this);
                return tmp ? _toDataURL.call(tmp, type, quality)
                           : _toDataURL.call(this, type, quality);
            };
        } catch (_) {}

        // 3. toBlob
        try {
            const _toBlob = HTMLCanvasElement.prototype.toBlob;
            HTMLCanvasElement.prototype.toBlob = function (callback, type, quality) {
                if (this.width === 0 || this.height === 0) {
                    return _toBlob.call(this, callback, type, quality);
                }
                const tmp = _noisedCopy(this);
                if (tmp) { _toBlob.call(tmp, callback, type, quality); }
                else     { _toBlob.call(this, callback, type, quality); }
            };
        } catch (_) {}

        // 4. WebGLRenderingContext.readPixels
        // pixels buffer is always args[6]; the offset variant (WebGL2) passes a number
        // there instead — _flipBit's instanceof check safely ignores that case.
        try {
            const _rp1 = WebGLRenderingContext.prototype.readPixels;
            WebGLRenderingContext.prototype.readPixels = function () {
                _rp1.apply(this, arguments);
                _flipBit(arguments[6]);
            };
        } catch (_) {}

        // 5. WebGL2RenderingContext.readPixels
        try {
            const _rp2 = WebGL2RenderingContext.prototype.readPixels;
            WebGL2RenderingContext.prototype.readPixels = function () {
                _rp2.apply(this, arguments);
                _flipBit(arguments[6]);
            };
        } catch (_) {}
    })();

    // --- Timing precision ---
    // Reduce performance.now() granularity to 0.1 ms to limit timing-based
    // side-channel attacks (Spectre mitigations already do this in some
    // configurations; we make it explicit and consistent).
    if (typeof Performance !== 'undefined') {
        try {
            const _now = Performance.prototype.now;
            Performance.prototype.now = function () {
                return Math.round(_now.call(this) * 10) / 10;
            };
        } catch (_) {}
    }

    // --- Geolocation API ---
    // Replace navigator.geolocation with a stub that always returns Linden Lab
    // HQ (945 Battery St, San Francisco, CA 94111) instead of the real device
    // location.  Using the prototype so the override applies to every frame.
    (function () {
        const _llCoords = {
            latitude:         37.7988,
            longitude:       -122.3984,
            accuracy:         100,
            altitude:         null,
            altitudeAccuracy: null,
            heading:          null,
            speed:            null
        };
        const _fakeGeo = {
            getCurrentPosition: function (success, _error, _opts) {
                setTimeout(function () {
                    success({ coords: _llCoords, timestamp: Date.now() });
                }, 0);
            },
            watchPosition: function (success, _error, _opts) {
                setTimeout(function () {
                    success({ coords: _llCoords, timestamp: Date.now() });
                }, 0);
                return 0;
            },
            clearWatch: function (_id) {}
        };
        tryLock(Navigator.prototype, 'geolocation', _fakeGeo);
    })();

    // --- Timezone ---
    // Lock the reported timezone to America/Los_Angeles so it is consistent
    // with the geolocation anchor above.
    //
    // Two surfaces are covered:
    //   1. Intl.DateTimeFormat().resolvedOptions().timeZone  — the string identifier
    //      used by FingerprintJS and most modern fingerprinters.
    //   2. Date.prototype.getTimezoneOffset()               — the numeric UTC offset
    //      (PST = 480 min, i.e. UTC-8).  A fixed value is used; the small DST
    //      inaccuracy in summer is an acceptable trade-off for privacy.
    (function () {
        const _fakeZone   = 'America/Los_Angeles';
        const _fakeTZOff  = 480; // UTC-8 (PST)

        // Capture the real system timezone once so we can replace it selectively:
        // only swap when the formatter would have reported the real local zone.
        // Formatters created with an explicit timeZone option are left untouched.
        let _realZone = '';
        try { _realZone = Intl.DateTimeFormat().resolvedOptions().timeZone; } catch (_) {}

        if (typeof Intl !== 'undefined' && Intl.DateTimeFormat) {
            try {
                const _resolvedOptions = Intl.DateTimeFormat.prototype.resolvedOptions;
                Intl.DateTimeFormat.prototype.resolvedOptions = function () {
                    // Clone before mutating — the native return may be a frozen object.
                    const opts = Object.assign({}, _resolvedOptions.call(this));
                    if (opts.timeZone === _realZone) {
                        opts.timeZone = _fakeZone;
                    }
                    return opts;
                };
            } catch (_) {}
        }

        try {
            Date.prototype.getTimezoneOffset = function () { return _fakeTZOff; };
        } catch (_) {}
    })();

    // --- Font metric fingerprinting ---
    // CSS-based font detectors (FingerprintJS, BrowserLeaks, amiunique…) work by
    // rendering text in a hidden element and reading offsetWidth / getBoundingClientRect
    // to detect whether the OS has a given font installed.  No canvas is involved, so
    // --disable-reading-from-canvas gives no protection here.
    //
    // Technique (same as Font Fingerprint Defender and Brave's font protection):
    //   Inject ±1e-6 px of random noise into every float measurement returned by
    //   the four layout APIs fingerprinters rely on.  The noise is:
    //     • Imperceptible — 0.000001 px is far below a physical pixel.
    //     • Per-call random — each measurement call gets independent noise, so
    //       the accumulated hash changes on every page load.
    //     • Injected before any page script — CefRegisterExtension fires earlier
    //       than a browser-extension content script, so there is no race window.
    //
    // Covered surfaces:
    //   1. Element.prototype.getBoundingClientRect   — primary CSS metric path
    //   2. Range.prototype.getBoundingClientRect     — text-node metric path
    //   3. Element.prototype.getClientRects          — multi-rect variant
    //   4. CanvasRenderingContext2D.prototype.measureText — canvas text metrics
    //   5. document.fonts.check()                   — direct FontFaceSet probe
    (function () {
        // One seed per V8 context (= per page load), reused for every call so that
        // measuring the same element twice gives the same result within a session.
        const _sessionNoise = (Math.random() * 0.000002) - 0.000001;

        function _noisedRect(r) {
            return new DOMRect(r.x + _sessionNoise, r.y + _sessionNoise,
                               r.width + _sessionNoise, r.height + _sessionNoise);
        }

        // 1. Element.getBoundingClientRect
        try {
            const _eBCR = Element.prototype.getBoundingClientRect;
            Element.prototype.getBoundingClientRect = function () {
                return _noisedRect(_eBCR.call(this));
            };
        } catch (_) {}

        // 2. Range.getBoundingClientRect
        try {
            const _rBCR = Range.prototype.getBoundingClientRect;
            Range.prototype.getBoundingClientRect = function () {
                return _noisedRect(_rBCR.call(this));
            };
        } catch (_) {}

        // 3. Element.getClientRects
        // Returns a DOMRectList; we return a noised array with a compatible .item() method.
        try {
            const _gCR = Element.prototype.getClientRects;
            Element.prototype.getClientRects = function () {
                const out = Array.from(_gCR.call(this), _noisedRect);
                out.item = function (i) { return out[i] || null; };
                return out;
            };
        } catch (_) {}

        // 4. CanvasRenderingContext2D.measureText
        // TextMetrics is not directly constructable, so we return a plain object.
        // Each numeric property gets independent noise so all metrics are affected.
        // Undefined properties (older Chromium) are passed through as-is via _addNoise.
        try {
            const _measureText = CanvasRenderingContext2D.prototype.measureText;
            const _addNoise = function (v) { return typeof v === 'number' ? v + _sessionNoise : v; };
            CanvasRenderingContext2D.prototype.measureText = function (text) {
                const m = _measureText.call(this, text);
                return {
                    width:                    _addNoise(m.width),
                    actualBoundingBoxLeft:    _addNoise(m.actualBoundingBoxLeft),
                    actualBoundingBoxRight:   _addNoise(m.actualBoundingBoxRight),
                    fontBoundingBoxAscent:    _addNoise(m.fontBoundingBoxAscent),
                    fontBoundingBoxDescent:   _addNoise(m.fontBoundingBoxDescent),
                    actualBoundingBoxAscent:  _addNoise(m.actualBoundingBoxAscent),
                    actualBoundingBoxDescent: _addNoise(m.actualBoundingBoxDescent),
                    emHeightAscent:           _addNoise(m.emHeightAscent),
                    emHeightDescent:          _addNoise(m.emHeightDescent),
                    hangingBaseline:          _addNoise(m.hangingBaseline),
                    alphabeticBaseline:       _addNoise(m.alphabeticBaseline),
                    ideographicBaseline:      _addNoise(m.ideographicBaseline)
                };
            };
        } catch (_) {}

        // 5. FontFaceSet.prototype.check() — direct font presence probe.
        // Patching the prototype covers all documents and iframes, not just the
        // current document.fonts instance (which the instance-level patch would miss).
        try {
            if (typeof FontFaceSet !== 'undefined') {
                FontFaceSet.prototype.check = function () { return false; };
            } else if (typeof document !== 'undefined' && document.fonts) {
                document.fonts.check = function () { return false; };
            }
        } catch (_) {}
    })();

    // --- WebGL fingerprinting ---
    // Even with WEBGL_debug_renderer_info disabled, gl.getParameter(RENDERER/VENDOR)
    // still returns the real GPU string.  getSupportedExtensions() returns a
    // GPU/driver-specific list.  Both are stabilized to generic WebKit values.
    //
    // Also intercepted:
    //   - UNMASKED_RENDERER_WEBGL (0x9246) and UNMASKED_VENDOR_WEBGL (0x9245)
    //     from the WEBGL_debug_renderer_info extension — these are the primary
    //     GPU identification constants used by fingerprinters.
    //   - getExtension('WEBGL_debug_renderer_info') — returns null so the extension
    //     object itself is never handed to page code.
    (function () {
        const _RENDERER          = 0x1F01;
        const _VENDOR            = 0x1F00;
        const _UNMASKED_RENDERER = 0x9246;
        const _UNMASKED_VENDOR   = 0x9245;

        // A fixed extension list modelled on a generic WebKit WebGL implementation.
        // Stable across all sessions so it does not contribute to a fingerprint.
        // WEBGL_debug_renderer_info is intentionally absent.
        const _EXTS = [
            'ANGLE_instanced_arrays', 'EXT_blend_minmax',
            'EXT_color_buffer_half_float', 'EXT_frag_depth',
            'EXT_sRGB', 'EXT_shader_texture_lod',
            'EXT_texture_filter_anisotropic',
            'OES_element_index_uint', 'OES_standard_derivatives',
            'OES_texture_float', 'OES_texture_float_linear',
            'OES_texture_half_float', 'OES_texture_half_float_linear',
            'OES_vertex_array_object',
            'WEBGL_color_buffer_float', 'WEBGL_depth_texture',
            'WEBGL_draw_buffers', 'WEBGL_lose_context'
        ];

        function _patchWebGL(proto) {
            try {
                const _gp = proto.getParameter;
                proto.getParameter = function (p) {
                    if (p === _RENDERER || p === _UNMASKED_RENDERER) { return 'WebKit WebGL'; }
                    if (p === _VENDOR   || p === _UNMASKED_VENDOR)   { return 'WebKit'; }
                    return _gp.call(this, p);
                };
            } catch (_) {}
            try {
                proto.getSupportedExtensions = function () { return _EXTS.slice(); };
            } catch (_) {}
            try {
                const _getExt = proto.getExtension;
                proto.getExtension = function (name) {
                    if (name === 'WEBGL_debug_renderer_info') { return null; }
                    return _getExt.call(this, name);
                };
            } catch (_) {}
        }

        if (typeof WebGLRenderingContext  !== 'undefined') {
            _patchWebGL(WebGLRenderingContext.prototype);
        }
        if (typeof WebGL2RenderingContext !== 'undefined') {
            _patchWebGL(WebGL2RenderingContext.prototype);
        }
    })();

    // --- RTCPeerConnection — WebRTC fingerprinting ---
    // Three vectors are covered:
    //   1. Local IP leak via STUN: strip iceServers so no ICE candidates are gathered.
    //   2. onicecandidate: suppress the event so mDNS hostname UUIDs are not exposed.
    //      (mDNS candidate UUIDs are stable per Chromium profile and act as device IDs.)
    //   3. RTCRtpSender/Receiver.getCapabilities(): static methods that return the full
    //      codec list without requiring a PeerConnection — replaced with a fixed generic
    //      list so the Chromium build version cannot be inferred from codec parameters.
    (function () {
        if (typeof RTCPeerConnection === 'undefined') { return; }
        try {
            const _RPC = RTCPeerConnection;
            function _SafeRTCPeerConnection(config) {
                const safe = config ? Object.assign({}, config, { iceServers: [] }) : {};
                const pc = new _RPC(safe);
                // Suppress ICE candidate events — prevents mDNS UUID device fingerprint.
                pc.addEventListener('icecandidate', function (e) {
                    e.stopImmediatePropagation();
                }, true);
                return pc;
            }
            _SafeRTCPeerConnection.prototype = _RPC.prototype;
            Object.defineProperty(window, 'RTCPeerConnection', {
                value: _SafeRTCPeerConnection, writable: true, configurable: true
            });
        } catch (_) {}

        // Fixed generic codec capability lists.  Real Chrome returns a build-specific
        // list with precise profile-level-id and packetization-mode parameters.
        const _audioCaps = { codecs: [
            { mimeType: 'audio/opus',  clockRate: 48000, channels: 2 },
            { mimeType: 'audio/G722',  clockRate: 8000,  channels: 1 },
            { mimeType: 'audio/PCMU',  clockRate: 8000,  channels: 1 },
            { mimeType: 'audio/PCMA',  clockRate: 8000,  channels: 1 }
        ], headerExtensions: [] };
        const _videoCaps = { codecs: [
            { mimeType: 'video/VP8', clockRate: 90000 },
            { mimeType: 'video/VP9', clockRate: 90000 }
        ], headerExtensions: [] };

        try {
            if (typeof RTCRtpSender !== 'undefined') {
                RTCRtpSender.getCapabilities = function (kind) {
                    if (kind === 'audio') { return _audioCaps; }
                    if (kind === 'video') { return _videoCaps; }
                    return null;
                };
            }
        } catch (_) {}
        try {
            if (typeof RTCRtpReceiver !== 'undefined') {
                RTCRtpReceiver.getCapabilities = function (kind) {
                    if (kind === 'audio') { return _audioCaps; }
                    if (kind === 'video') { return _videoCaps; }
                    return null;
                };
            }
        } catch (_) {}
    })();

    // --- Math fingerprinting ---
    // Platform-specific floating-point results from trig/hyperbolic functions are
    // collected and hashed by some fingerprinters.  A per-session noise value is
    // added to each non-zero result — too small to affect any computation but
    // enough to make the fingerprint hash differ across sessions.
    (function () {
        const _mathNoise = (Math.random() - 0.5) * 1e-15;
        ['sin', 'cos', 'tan', 'asin', 'acos', 'atan', 'atan2',
         'sinh', 'cosh', 'tanh', 'exp', 'log'].forEach(function (fn) {
            try {
                const _orig = Math[fn];
                Math[fn] = function () {
                    const r = _orig.apply(Math, arguments);
                    return r === 0 ? r : r + _mathNoise;
                };
            } catch (_) {}
        });
    })();

    // --- Speech synthesis voices ---
    // getVoices() returns an OS-specific list of TTS voices — highly identifying.
    // Return an empty list so no voice information is exposed.
    try {
        if (typeof SpeechSynthesis !== 'undefined') {
            SpeechSynthesis.prototype.getVoices = function () { return []; };
        }
    } catch (_) {}

    // --- CSS media query fingerprinting ---
    // FingerprintJS checks prefers-color-scheme, color-gamut, pointer type, etc.
    // to fingerprint OS appearance settings and hardware capabilities.
    // Known fingerprinting queries are intercepted and returned with neutral values;
    // all other queries are passed through to the real matchMedia unmodified so
    // that legitimate responsive-design code continues to work.
    (function () {
        if (typeof window === 'undefined' ||
            typeof window.matchMedia !== 'function') { return; }
        try {
            const _mm = window.matchMedia.bind(window);
            // feature → the value that represents a neutral/common result.
            // Entries are ordered most-specific first so that 'any-pointer' is
            // matched before 'pointer' and 'any-hover' before 'hover', preventing
            // the shorter name from incorrectly matching inside the longer one.
            const _neutral = {
                'any-pointer':            'fine',
                'any-hover':              'hover',
                'pointer':                'fine',
                'hover':                  'hover',
                'prefers-color-scheme':   'light',
                'prefers-reduced-motion': 'no-preference',
                'prefers-contrast':       'no-preference',
                'forced-colors':          'none',
                'color-gamut':            'srgb'
            };
            window.matchMedia = function (query) {
                for (const feature in _neutral) {
                    // Match whole feature name followed by ':' or whitespace to avoid
                    // false positives from one name being a substring of another.
                    const _re = new RegExp('(?:^|[\\s(])' + feature + '[\\s:]');
                    if (_re.test(query)) {
                        const matches = query.indexOf(_neutral[feature]) >= 0;
                        return {
                            matches:             matches,
                            media:               query,
                            onchange:            null,
                            addEventListener:    function () {},
                            removeEventListener: function () {},
                            addListener:         function () {},
                            removeListener:      function () {},
                            dispatchEvent:       function () { return false; }
                        };
                    }
                }
                return _mm(query);
            };
        } catch (_) {}
    })();

    // --- window.chrome ---
    // Chromium exposes a window.chrome object whose exact shape identifies the
    // CEF/Chrome version.  Replace it with a minimal stub that satisfies existence
    // checks without leaking version information.
    //
    // Check the existing property descriptor first: if it is already non-configurable
    // (e.g. set by Chromium's own binding code before our extension runs), we cannot
    // redefine it and must leave it in place rather than throwing a silent no-op.
    try {
        if (typeof window !== 'undefined') {
            const _chromeDesc = Object.getOwnPropertyDescriptor(window, 'chrome');
            if (!_chromeDesc || _chromeDesc.configurable) {
                Object.defineProperty(window, 'chrome', {
                    value:        { runtime: {} },
                    writable:     false,
                    configurable: false,
                    enumerable:   true
                });
            }
        }
    } catch (_) {}
})();
)js";

class dullahan_privacy_app :
    public CefApp,
    public CefRenderProcessHandler
{
public:
    // CefApp override - return ourselves as the render process handler.
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override
    {
        return this;
    }

    // CefRenderProcessHandler override.
    // Called once in the render process after WebKit has been initialised but
    // before any frame contexts are created.  This is the correct place to
    // register V8 extensions: the extension code is then injected into every
    // subsequent frame/iframe V8 context before any page script runs.
    void OnWebKitInitialized() override
    {
        CefRefPtr<CefCommandLine> cmdLine = CefCommandLine::GetGlobalCommandLine();
        if (!cmdLine->HasSwitch("dullahan-protect-privacy"))
        {
            return;
        }

        // nullptr handler is fine for pure-JS extensions (no 'native function' declarations).
        CefRegisterExtension("dullahan/privacy", kPrivacyShimJS, nullptr);
    }

    // CefRenderProcessHandler override.
    // Called for every new V8 context, including Web Worker contexts.
    // CefRegisterExtension covers main frames and iframes automatically, but does
    // NOT reach Web Worker V8 isolates.  Workers can bypass all JS shims by
    // running unpatched APIs inside a worker blob — this callback closes that gap
    // by evaluating the shim directly when the context has no associated frame
    // (which indicates a Worker rather than a normal document context).
    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override
    {
        if (frame)
        {
            // Normal frame/iframe — already covered by CefRegisterExtension.
            return;
        }

        CefRefPtr<CefCommandLine> cmdLine = CefCommandLine::GetGlobalCommandLine();
        if (!cmdLine->HasSwitch("dullahan-protect-privacy"))
        {
            return;
        }

        // Inject the privacy shim into the worker's V8 context.
        CefRefPtr<CefV8Value>     retval;
        CefRefPtr<CefV8Exception> exception;
        context->Eval(kPrivacyShimJS, CefString(), 0, retval, exception);
    }

    IMPLEMENT_REFCOUNTING(dullahan_privacy_app);
};

///////////////////////////////////////////////////////////////////////////////
// Platform entry points
///////////////////////////////////////////////////////////////////////////////

#ifdef __linux__
#if defined(NO_STACK_PROTECTOR)
NO_STACK_PROTECTOR
#endif
int main(int argc, char* argv[])
{
    CefMainArgs main_args(argc, argv);
    CefRefPtr<dullahan_privacy_app> app(new dullahan_privacy_app);
    return CefExecuteProcess(main_args, app, nullptr);
}
#endif

#ifdef WIN32
#include <windows.h>

#define HOST_PROCESS_REAPER

#ifdef HOST_PROCESS_REAPER
// Ignore c:\program files (x86)\microsoft visual studio 12.0\vc\include\thr\xthread(196): warning C4702: unreachable code
#pragma warning( disable : 4702)
#include <thread>
#include <tlhelp32.h>

/*
  Nasty hack to stop flash from displaying a popup with "NO SANDBOX"
  Flashplayer will try to spawn a cmd.exe and echo this message into it, we
  use a process group to limit the number of processes allowed to 1, thus preventing
  popup.

  Limitation: NeedsWindows 8 or higher, the viewer already does put SLPlugin (and with that
  all sub processes) into a job, so all plugin instances get killed when the viewer does exit.
  Anything before Windows 8 will not allow a process being part of more than one job.

  Using the sandbox would fix this problem, but for using the sandbox the same executable
  must be used  for browser and all sub processes (see cef_sandbox_win.h); but the viewer
  uses slplugin.exe and llceflib_host.exe.
*/
void enablePPAPIFlashHack(LPSTR lpCmdLine)
{
    if (!lpCmdLine)
    {
        return;
    }

    std::string strCmdLine = lpCmdLine;

    std::string strType = "--type=ppapi";
    std::string::size_type i = strCmdLine.find(strType);

    if (i == std::string::npos)
    {
        return;
    }

    HANDLE hJob = CreateJobObject(nullptr, nullptr);
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, ::GetCurrentProcessId());

    if (!AssignProcessToJobObject(hJob, hProc))
    {
        ::CloseHandle(hProc);
        ::CloseHandle(hJob);
        return;
    }

    JOBOBJECT_BASIC_LIMIT_INFORMATION baseLimits = {};
    baseLimits.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    baseLimits.ActiveProcessLimit = 1;

    SetInformationJobObject(hJob, JobObjectBasicLimitInformation, &baseLimits, sizeof(baseLimits));

    ::CloseHandle(hProc);
    ::CloseHandle(hJob);
}

// taken from http://magpcss.org/ceforum/viewtopic.php?f=6&t=15817&start=10#p37820
// works around a CEF issue (yet to be filed) where the host process is not destroyed
// after CEF exits in some case on Windows 7
// Making it switchable for now while I investigate it a bit
HANDLE GetParentProcess()
{
    HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    PROCESSENTRY32 ProcessEntry = {};
    ProcessEntry.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(Snapshot, &ProcessEntry))
    {
        DWORD CurrentProcessId = GetCurrentProcessId();

        do
        {
            if (ProcessEntry.th32ProcessID == CurrentProcessId)
            {
                break;
            }
        }
        while (Process32Next(Snapshot, &ProcessEntry));
    }

    CloseHandle(Snapshot);

    return OpenProcess(SYNCHRONIZE, FALSE, ProcessEntry.th32ParentProcessID);
}
#endif

#if defined(NO_STACK_PROTECTOR)
NO_STACK_PROTECTOR
#endif
int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                     LPSTR lpCmdLine, int nCmdShow)
{
#ifdef HOST_PROCESS_REAPER
    HANDLE ParentProcess = GetParentProcess();

    std::thread([ParentProcess]()
    {
        WaitForSingleObject(ParentProcess, INFINITE);
        ExitProcess(0);
    }).detach();
#endif

    CefMainArgs args(GetModuleHandle(nullptr));

    enablePPAPIFlashHack(lpCmdLine);

    CefRefPtr<dullahan_privacy_app> app(new dullahan_privacy_app);
    return CefExecuteProcess(args, app, nullptr);
}
#endif

// OS X Helper executable, we can probably share this between Win & Mac
#ifdef __APPLE__
#include "include/wrapper/cef_library_loader.h"

// Entry point function for sub-processes.
#if defined(NO_STACK_PROTECTOR)
NO_STACK_PROTECTOR
#endif
int main(int argc, char* argv[])
{
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInHelper())
    {
        return 1;
    }

    // Provide CEF with command-line arguments.
    CefMainArgs args(argc, argv);

    CefRefPtr<dullahan_privacy_app> app(new dullahan_privacy_app);

    // Execute the sub-process.
    return CefExecuteProcess(args, app, nullptr);
}
#endif
