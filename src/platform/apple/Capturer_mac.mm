// macOS screen capture via ScreenCaptureKit.
//
// API history (why the code looks the way it does):
//   - CGDisplayCreateImage is unavailable on macOS 15+ (dui issue #239)
//   - macOS 26 REMOVED the macOS 14-era synchronous/plural APIs:
//       SCShareableContent.currentShareableContentWithError:
//       SCScreenshotManager.captureImageWithContentFilter:...
//       SCContentFilter.initWithDisplays:excludingWindows:
//     Calling them throws "unrecognized selector" at runtime (no compile
//     error with older SDKs, but the call crashes into the @catch below),
//     which made the host silently fall back to the test-pattern source.
//     This file therefore only uses the current API surface:
//       - SCShareableContent async getter (macOS 12.3+)
//       - SCScreenshotManager.captureImageWithFilter: (macOS 14.0+) with
//         one SCContentFilter (initWithDisplay:excludingWindows:) per
//         display, composited into the union canvas here
//
// Window exclusion: every StarDesk instance's windows are excluded from the
// capture. A StarDesk window renders the very stream we are capturing, so
// including it produces an infinite mirror-feedback loop (the "hall of
// mirrors" - each bounce also appears horizontally mirrored). This matters
// for two instances on one machine and would also happen with the window on
// the peer's screen in the other direction. SCScreenshotManager's
// captureImageInRect: (macOS 15.2+) has no window-exclusion option, so the
// filter-based path is used on all supported macOS versions.
//
// macOS 26 resilience: the SCStream-based filter path can wedge system-wide
// (one-shot captures never complete until reboot, even for other apps).
// Capture() therefore switches to captureImageInRect: after 5 consecutive
// filter timeouts, and Start() falls back to AppKit display geometry when
// the shareable-content fetch times out. On a healthy system the filter
// path (with window exclusion) is used.
//
// Requires Screen Recording permission; when missing, Start() reports an
// error and the host falls back to the test-pattern source.

#import <CoreGraphics/CoreGraphics.h>
#import <Cocoa/Cocoa.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <dispatch/dispatch.h>
#import <unistd.h>

#include <mutex>

#include "../Capturer.h"

// SCStream output/delegate: copies every delivered frame into the Impl's
// latest-frame buffer (SCStream keeps its delegate weakly, so the Impl
// owns this object).
@interface StreamOutputImpl : NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, assign) sdk::Capturer::Impl* impl;
@end

namespace sdk {

struct Capturer::Impl {
    bool available = false;
    bool started = false;
    std::string error;
    int width = 0;
    int height = 0;
    int originX = 0;
    int originY = 0;
    bool extend = false;
    // true = capture via SCScreenshotManager captureImageInRect: (no window
    // exclusion); used when the filter-based path is wedged (macOS 26 has a
    // bug where SCStream-based one-shot captures can hang system-wide)
    bool useInRect = false;
    int filterFailCount = 0;

    // SCStream continuous capture (mirror mode only; extend keeps the
    // one-shot composite path). Delivers frames at up to the display
    // refresh rate instead of the ~5fps of one-shot screenshots.
    bool streamMode = false;
    SCStream* stream = nil;              // retained
    StreamOutputImpl* output = nil;      // retained
    dispatch_queue_t sampleQueue = nil;  // retained
    int targetW = 0;                     // requested output box (0 = native)
    int targetH = 0;
    // latest frame (written by the output callback)
    std::mutex frameMutex;
    std::vector<uint8_t> frameBuf;
    int frameW = 0;
    int frameH = 0;
    uint64_t frameSeq = 0;
    double frameTime = 0; // CACurrentMediaTime() of the last frame
};

Capturer::Capturer() : m_impl(new Impl()) {}

Capturer::~Capturer()
{
    Stop();
}

bool Capturer::PlatformSupported()
{
    // SCScreenshotManager: macOS 14.0+
    if (@available(macOS 14.0, *)) {
        return true;
    }
    return false;
}

namespace {

// Fetch the shareable content asynchronously (the synchronous
// currentShareableContentWithError: was removed in macOS 26). Returns a +1
// owned SCShareableContent (caller releases), or nil on failure/timeout;
// on failure `err` receives a +1 owned NSError (caller releases).
SCShareableContent* FetchShareableContent(NSError** err)
{
    if (err) *err = nil;
    __block SCShareableContent* content = nil;
    __block NSError* error = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentWithCompletionHandler:
        ^(SCShareableContent* _Nullable c, NSError* _Nullable e) {
            content = [c retain];
            error = [e retain];
            dispatch_semaphore_signal(sem);
        }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
    if (content == nil && err) {
        *err = error; // transfer ownership
    }
    else {
        [error release];
    }
    return content;
}

// Build the display list for the current mode: mirror = main display only,
// extend = all displays. Content is non-nil; result is autoreleased.
NSArray<SCDisplay*>* SelectDisplays(SCShareableContent* content, bool extend)
{
    if (extend) {
        return content.displays;
    }
    CGDirectDisplayID mainID = (CGDirectDisplayID)[[[NSScreen mainScreen]
        deviceDescription][@"NSScreenNumber"] unsignedIntValue];
    for (SCDisplay* d in content.displays) {
        if (d.displayID == mainID) {
            return @[ d ];
        }
    }
    if (content.displays.count > 0) {
        return @[ content.displays[0] ];
    }
    return @[];
}

// Windows owned by any StarDesk instance (this process, the same bundle id,
// or a same-named process - covers both the .app bundle and the raw binary)
// are excluded from the capture to prevent the infinite mirror feedback.
NSArray<SCWindow*>* OwnAppWindows(SCShareableContent* content)
{
    NSMutableArray<SCWindow*>* out = [NSMutableArray array];
    if (content.windows.count == 0) {
        return out;
    }
    NSString* selfBundleId = [[NSBundle mainBundle] bundleIdentifier];
    NSString* selfProcName = [[NSProcessInfo processInfo] processName];
    const pid_t selfPid = getpid();
    for (SCWindow* w in content.windows) {
        SCRunningApplication* app = w.owningApplication;
        if (app == nil) {
            continue;
        }
        if (app.processID == selfPid ||
            (selfBundleId.length > 0 &&
             [app.bundleIdentifier isEqualToString:selfBundleId]) ||
            [app.applicationName localizedCaseInsensitiveCompare:selfProcName] ==
                NSOrderedSame) {
            [out addObject:w];
        }
    }
    return out;
}

// Backing-pixel scale of a display (2 for a Retina display in a 2x scaled
// mode). CGDisplayPixelsWide reports the VIRTUAL (point) framebuffer in
// macOS 26 scaled modes (2560x1600 panels at "1440x900" show 1440 there),
// so AppKit's backingScaleFactor is the reliable source. SCDisplay.displayID
// matches NSScreen's NSScreenNumber.
int DisplayScale(SCDisplay* display)
{
    for (NSScreen* s in [NSScreen screens]) {
        CGDirectDisplayID did = (CGDirectDisplayID)[[s deviceDescription][@"NSScreenNumber"]
            unsignedIntValue];
        if (did == display.displayID) {
            const int sc = (int)(s.backingScaleFactor + 0.5);
            return sc > 0 ? sc : 1;
        }
    }
    return 1;
}

// Start an SCStream continuous capture on the main display (mirror mode
// only; extend mode keeps the one-shot composite path). Frames are
// delivered on im->sampleQueue at up to the display refresh rate and copied
// into im->frameBuf by the output callback. Returns false when the stream
// cannot be started / delivers no frame within 3s (caller falls back to the
// one-shot screenshot path).
bool TryStartStream(Capturer::Impl* im, SCShareableContent* content)
{
    NSArray<SCDisplay*>* displays = SelectDisplays(content, false);
    if (displays.count == 0) {
        return false;
    }
    SCDisplay* d = displays[0];
    const int scale = DisplayScale(d);
    NSArray<SCWindow*>* excluded = OwnAppWindows(content);
    SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:d
                                                      excludingWindows:excluded];
    SCStreamConfiguration* conf = [[SCStreamConfiguration alloc] init];
    conf.width = (NSInteger)d.frame.size.width * scale;
    conf.height = (NSInteger)d.frame.size.height * scale;
    conf.pixelFormat = kCVPixelFormatType_32BGRA; // SCK only delivers BGRA; the output callback swizzles to RGBA
    // sRGB output: SCStream defaults to the display's P3 color space and the
    // pipeline is sRGB - P3 pixels would render noticeably yellow-green
    conf.colorSpaceName = kCGColorSpaceSRGB;
    conf.minimumFrameInterval = CMTimeMake(1, 60);
    conf.queueDepth = 4;
    conf.showsCursor = NO;

    im->output = [[StreamOutputImpl alloc] init];
    im->output.impl = im;
    NSError* err = nil;
    SCStream* stream = [[SCStream alloc] initWithFilter:filter configuration:conf delegate:im->output];
    im->stream = stream;
    im->sampleQueue = dispatch_queue_create("stardesk.capture", DISPATCH_QUEUE_SERIAL);
    BOOL ok = [stream addStreamOutput:im->output type:SCStreamOutputTypeScreen
                    sampleHandlerQueue:im->sampleQueue error:&err];
    if (ok) {
        __block bool started = false;
        __block bool startError = false;
        [stream startCaptureWithCompletionHandler:^(NSError* _Nullable e2) {
            if (e2 != nil) {
                startError = true;
            }
            started = true;
        }];
        // wait up to 3s for the start callback + the first delivered frame
        const double deadline = CACurrentMediaTime() + 3.0;
        while (CACurrentMediaTime() < deadline) {
            if (started && im->frameSeq > 0) {
                [filter release];
                [conf release];
                im->streamMode = true;
                return true;
            }
            if (started && startError) {
                break;
            }
            usleep(10000);
        }
    }
    // fall back to the one-shot path
    if (im->stream != nil) {
        [im->stream stopCaptureWithCompletionHandler:nil];
        [im->stream release];
        im->stream = nil;
    }
    if (im->output != nil) {
        [im->output release];
        im->output = nil;
    }
    if (im->sampleQueue != nil) {
        dispatch_release(im->sampleQueue);
        im->sampleQueue = nil;
    }
    [filter release];
    [conf release];
    return false;
}

// Top-down BGRA (premultiplied, kCGImageAlphaPremultipliedLast) copy of a
// bottom-up CGImage into `out`.
bool ConvertToRGBA(CGImageRef image, CaptureFrame& out)
{
    const size_t w = CGImageGetWidth(image);
    const size_t h = CGImageGetHeight(image);
    out.width = (int)w;
    out.height = (int)h;
    out.rgba.resize(w * h * 4);

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(out.rgba.data(), w, h, 8, w * 4, cs,
                                             kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (ctx == NULL) {
        return false;
    }
    // flip: CGImage is bottom-up, we want top-down
    CGContextTranslateCTM(ctx, 0, (CGFloat)h);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), image);
    CGContextRelease(ctx);
    return true;
}

// Capture every selected display with its own filter (excluding `excluded`
// windows) and composite the images into the union canvas. Display frames
// live in one global coordinate space, so a single flipped context places
// them all. `originX/originY` is the union top-left in global display
// coordinates; `unionW/unionH` the union size in points.
bool CaptureDisplaysComposite(NSArray<SCDisplay*>* displays, NSArray<SCWindow*>* excluded,
                              int originX, int originY, int unionW, int unionH,
                              CaptureFrame& out)
{
    // uniform backing scale for all displays when possible (the common
    // case); mixed-DPI layouts fall back to 1x
    int scale = 1;
    {
        int firstScale = 0;
        for (SCDisplay* d in displays) {
            const int s = DisplayScale(d);
            if (firstScale == 0) {
                firstScale = s;
            }
            else if (s != firstScale) {
                firstScale = 0;
                break;
            }
        }
        if (firstScale > 0) {
            scale = firstScale;
        }
    }

    const int cw = unionW * scale;
    const int ch = unionH * scale;
    out.width = cw;
    out.height = ch;
    out.rgba.assign((size_t)cw * ch * 4, 0);

    // topmost edge of the union in global display coordinates (display
    // frames use a bottom-left origin, the canvas is top-down)
    double maxY = 0;
    for (SCDisplay* d in displays) {
        maxY = MAX(maxY, d.frame.origin.y + d.frame.size.height);
    }

    // NOTE: captureImageWithFilter: returns TOP-DOWN images (unlike
    // captureImageInRect:, which is bottom-up) - the context must NOT be
    // flipped here, or the frame comes out upside down
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(out.rgba.data(), cw, ch, 8, cw * 4, cs,
                                             kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (ctx == NULL) {
        return false;
    }

    bool ok = true;
    for (SCDisplay* d in displays) {
        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:d
                                                          excludingWindows:excluded];
        SCStreamConfiguration* conf = [[SCStreamConfiguration alloc] init];
        conf.width = (NSInteger)d.frame.size.width * scale;
        conf.height = (NSInteger)d.frame.size.height * scale;
        conf.showsCursor = NO;
        conf.minimumFrameInterval = CMTimeMake(1, 30);

        __block CGImageRef image = NULL;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [SCScreenshotManager captureImageWithFilter:filter
                                      configuration:conf
                                  completionHandler:^(CGImageRef _Nullable img, NSError* _Nullable e) {
            if (img != NULL) {
                image = CGImageRetain(img);
            }
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));

        if (image != NULL) {
            const CGFloat x = (CGFloat)(d.frame.origin.x - originX) * scale;
            // top-down canvas: a display's top edge (global y = origin+height)
            // belongs at row (maxY - that edge) * scale
            const CGFloat y = (CGFloat)(maxY - d.frame.origin.y - d.frame.size.height) * scale;
            const CGFloat w = (CGFloat)d.frame.size.width * scale;
            const CGFloat h = (CGFloat)d.frame.size.height * scale;
            CGContextDrawImage(ctx, CGRectMake(x, y, w, h), image);
            CGImageRelease(image);
        }
        else {
            ok = false;
        }
        [conf release];
        [filter release];
    }
    CGContextRelease(ctx);
    return ok;
}

} // namespace

bool Capturer::Start(bool extend)
{
    m_impl->extend = extend;
    m_impl->error.clear();

    // preflight the screen-recording permission WITHOUT triggering the system
    // dialog (which would block); unpermitted -> caller falls back to the
    // test-pattern source and the UI shows a hint
    if (@available(macOS 10.15, *)) {
        if (!CGPreflightScreenCaptureAccess()) {
            m_impl->available = false;
            m_impl->error = "screen recording permission not granted";
            return false;
        }
    }

    if (@available(macOS 14.0, *)) {
        @try {
            @autoreleasepool {
                NSError* err = nil;
                SCShareableContent* content = FetchShareableContent(&err);
                if (content == nil) {
                    const bool permission = (err != nil && err.code == 4608);
                    [err release];
                    if (permission) {
                        m_impl->available = false;
                        m_impl->error = "screen recording permission not granted";
                        return false;
                    }
                    // shareable-content fetch timed out (macOS 26 SCK wedge):
                    // fall back to AppKit geometry + captureImageInRect: so
                    // the stream still works. Only possible on macOS 15.2+
                    // where captureImageInRect: exists.
                    if ([SCScreenshotManager respondsToSelector:
                             @selector(captureImageInRect:completionHandler:)]) {
                        NSArray<NSScreen*>* screens = [NSScreen screens];
                        if (screens.count > 0) {
                            NSRect u = NSZeroRect;
                            for (NSScreen* s in screens) {
                                u = NSUnionRect(u, s.frame);
                            }
                            m_impl->width = (int)u.size.width;
                            m_impl->height = (int)u.size.height;
                            m_impl->originX = (int)u.origin.x;
                            m_impl->originY = (int)u.origin.y;
                            m_impl->useInRect = true;
                            m_impl->available = true;
                            m_impl->started = true;
                            return true;
                        }
                    }
                    m_impl->available = false;
                    m_impl->error = "no capture source";
                    return false;
                }
                [err release];

                NSArray<SCDisplay*>* displays = SelectDisplays(content, extend);
                if (displays.count == 0) {
                    [content release];
                    m_impl->available = false;
                    m_impl->error = "no capture source";
                    return false;
                }

                // union of the selected displays (side-by-side layouts included)
                CGRect u = CGRectNull;
                for (SCDisplay* d in displays) {
                    u = CGRectUnion(u, d.frame);
                }
                m_impl->width = (int)u.size.width;
                m_impl->height = (int)u.size.height;
                m_impl->originX = (int)u.origin.x;
                m_impl->originY = (int)u.origin.y;

                // prefer SCStream continuous capture (up to display refresh
                // rate); mirror mode only - extend keeps the one-shot path
                if (!extend) {
                    TryStartStream(m_impl.get(), content);
                }

                [content release];
                m_impl->available = true;
                m_impl->started = true;
                return true;
            }
        } @catch (NSException* e) {
            m_impl->available = false;
            m_impl->error = "screen recording permission not granted";
            return false;
        }
    }
    m_impl->error = "requires macOS 14+";
    return false;
}

void Capturer::Stop()
{
    m_impl->started = false;
    if (m_impl->stream != nil) {
        [m_impl->stream stopCaptureWithCompletionHandler:nil];
        // drain the sample queue so no output callback is in flight
        if (m_impl->sampleQueue != nil) {
            dispatch_sync(m_impl->sampleQueue, ^{});
        }
        [m_impl->stream release];
        m_impl->stream = nil;
    }
    if (m_impl->output != nil) {
        [m_impl->output release];
        m_impl->output = nil;
    }
    if (m_impl->sampleQueue != nil) {
        dispatch_release(m_impl->sampleQueue);
        m_impl->sampleQueue = nil;
    }
    m_impl->streamMode = false;
}

bool Capturer::Capture(CaptureFrame& out)
{
    if (!m_impl->available || !m_impl->started) {
        return false;
    }
    if (m_impl->streamMode) {
        std::lock_guard<std::mutex> lock(m_impl->frameMutex);
        if (m_impl->frameSeq == 0) {
            return false;
        }
        if (CACurrentMediaTime() - m_impl->frameTime > 1.0) {
            return false; // stream stopped delivering; treat as stale
        }
        out.width = m_impl->frameW;
        out.height = m_impl->frameH;
        out.rgba.assign(m_impl->frameBuf.begin(), m_impl->frameBuf.end());
        return true;
    }
    __block bool ok = false;
    if (@available(macOS 14.0, *)) {
        @try {
            @autoreleasepool {
                if (m_impl->useInRect) {
                    // macOS 15.2+: one display-agnostic call for the union
                    // rect (mirror = main display, extend = all displays).
                    // No window exclusion - this path is only used when the
                    // filter path is wedged (macOS 26 SCStream hang).
                    CGRect rect = CGRectMake(m_impl->originX, m_impl->originY,
                                             (CGFloat)m_impl->width, (CGFloat)m_impl->height);
                    __block CGImageRef image = NULL;
                    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
                    [SCScreenshotManager captureImageInRect:rect
                                          completionHandler:^(CGImageRef _Nullable img, NSError* _Nullable e) {
                        if (img != NULL) {
                            image = CGImageRetain(img);
                        }
                        dispatch_semaphore_signal(sem);
                    }];
                    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
                    if (image == NULL) {
                        return false;
                    }
                    ok = ConvertToRGBA(image, out);
                    CGImageRelease(image);
                    return ok;
                }

                NSError* err = nil;
                SCShareableContent* content = FetchShareableContent(&err);
                if (content == nil) {
                    [err release];
                    return false;
                }
                [err release];

                NSArray<SCDisplay*>* displays = SelectDisplays(content, m_impl->extend);
                if (displays.count == 0) {
                    [content release];
                    return false;
                }
                // exclude every StarDesk window (see file comment: prevents
                // the infinite mirror feedback when a stream window is on
                // the captured screen)
                NSArray<SCWindow*>* excluded = OwnAppWindows(content);
                ok = CaptureDisplaysComposite(displays, excluded,
                                              m_impl->originX, m_impl->originY,
                                              m_impl->width, m_impl->height, out);
                [content release];

                if (ok) {
                    m_impl->filterFailCount = 0;
                }
                else if (++m_impl->filterFailCount >= 5) {
                    // filter path keeps timing out (macOS 26 SCStream wedge
                    // is system-wide until reboot): switch to the rect API
                    // for the rest of this process so the stream still works
                    if ([SCScreenshotManager respondsToSelector:
                             @selector(captureImageInRect:completionHandler:)]) {
                        m_impl->useInRect = true;
                        m_impl->error = "filter capture stuck; switched to captureImageInRect";
                    }
                    else {
                        m_impl->filterFailCount = 0; // rect API unavailable; keep retrying
                    }
                }
            }
        } @catch (NSException* e) {
            return false;
        }
    }
    return ok;
}

int Capturer::GetWidth() const { return m_impl->width; }
int Capturer::GetHeight() const { return m_impl->height; }
int Capturer::GetOriginX() const { return m_impl->originX; }
int Capturer::GetOriginY() const { return m_impl->originY; }
bool Capturer::CursorEmbedded() const { return false; } // ScreenCaptureKit: no pointer in the frame
void Capturer::SetHideCursor(bool) {} // ScreenCaptureKit: pointer not in the frame
bool Capturer::GetCursor(int&, int&, bool&) const { return false; } // InputInjector covers it
bool Capturer::IsAvailable() const { return m_impl->available; }
std::string Capturer::GetError() const { return m_impl->error; }

void Capturer::SetTargetResolution(int width, int height)
{
    if (!m_impl->streamMode || m_impl->stream == nil) {
        return;
    }
    if (m_impl->targetW == width && m_impl->targetH == height) {
        return;
    }
    m_impl->targetW = width;
    m_impl->targetH = height;

    // native pixel size (frameW/H are set as soon as frames flow)
    const int nativeW = m_impl->frameW > 0 ? m_impl->frameW : (int)(m_impl->width * 2);
    const int nativeH = m_impl->frameH > 0 ? m_impl->frameH : (int)(m_impl->height * 2);
    int tw = nativeW;
    int th = nativeH;
    if (width > 0 && height > 0) {
        // largest aspect-preserving size that fits the requested box
        // (e.g. a 16:10 screen at "1080p" becomes 1728x1080, no letterbox)
        const double s = MIN((double)width / nativeW, (double)height / nativeH);
        if (s < 1.0) {
            tw = MAX(1, (int)(nativeW * s));
            th = MAX(1, (int)(nativeH * s));
        }
    }
    SCStreamConfiguration* conf = [[SCStreamConfiguration alloc] init];
    conf.width = tw;
    conf.height = th;
    conf.pixelFormat = kCVPixelFormatType_32BGRA; // SCK only delivers BGRA; the output callback swizzles to RGBA
    // sRGB output: SCStream defaults to the display's P3 color space and the
    // pipeline is sRGB - P3 pixels would render noticeably yellow-green
    conf.colorSpaceName = kCGColorSpaceSRGB;
    conf.minimumFrameInterval = CMTimeMake(1, 60);
    conf.queueDepth = 4;
    conf.showsCursor = NO;
    [m_impl->stream updateConfiguration:conf completionHandler:nil];
    [conf release];
}

} // namespace sdk

@implementation StreamOutputImpl

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type != SCStreamOutputTypeScreen) {
        return;
    }
    CVImageBufferRef buf = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (buf == NULL) {
        return;
    }
    CVPixelBufferLockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);
    const int w = (int)CVPixelBufferGetWidth(buf);
    const int h = (int)CVPixelBufferGetHeight(buf);
    const int rb = (int)CVPixelBufferGetBytesPerRow(buf);
    const uint8_t* base = (const uint8_t*)CVPixelBufferGetBaseAddress(buf);
    if (base != NULL && self.impl != NULL) {
        sdk::Capturer::Impl* im = self.impl;
        std::lock_guard<std::mutex> lock(im->frameMutex);
        const size_t need = (size_t)w * h * 4;
        if (im->frameW != w || im->frameH != h || im->frameBuf.size() != need) {
            im->frameBuf.resize(need);
            im->frameW = w;
            im->frameH = h;
        }
        uint8_t* dst = im->frameBuf.data();
        // BGRA -> RGBA swizzle (SCK delivers BGRA; the pipeline is RGBA)
        for (int y = 0; y < h; ++y) {
            const uint8_t* srcRow = base + (size_t)y * rb;
            uint8_t* dstRow = dst + (size_t)y * w * 4;
            for (int x = 0; x < w; ++x) {
                dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
                dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
            }
        }
        im->frameSeq++;
        im->frameTime = CACurrentMediaTime();
    }
    CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
    (void)stream;
    (void)error;
    // the one-shot fallback paths are per-capture; a stopped stream simply
    // stops delivering frames and Capture() reports stale -> host degrades
}

@end
