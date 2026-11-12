// Desktop audio capture via PipeWire (see AudioCapture.h).
//
// Captures the default audio sink's MONITOR: find the sink node named by
// the core's "default.audio.sink" property (or the first Audio/Sink when the
// property is absent), then connect an input stream to that node — PipeWire
// routes input streams to the sink's monitor ports (same approach as
// wf-recorder). The stream negotiates F32 48kHz stereo and we accumulate
// samples into 20ms frames for the Opus encoder.
//
// Threading: ALL PipeWire work (context, registry, stream, connect AND the
// iterate pump) runs on ONE dedicated thread. Pumping the loop from a
// different thread than the one that created/connected the stream races and
// stalls the negotiation (the loop's iterate is not cross-thread safe here)
// — this was reproduced with a standalone probe.
#include "AudioCapture.h"

#ifdef STARDESK_HAS_AUDIO

#include "../../codec/OpusAudio.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>
#include <spa/pod/vararg.h>
#include <spa/utils/dict.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace sdk {

namespace {

const char* kAudioRole = "StarDesk Audio";

} // namespace

// ------------------------------------------------------------------ impl

struct AudioCapture::Impl {
    // pipewire objects (all owned by the capture thread)
    struct pw_loop* loop = nullptr; // pw_loop_new (pw_main_loop_new crashes
                                    // pw_context_new on pipewire 1.6.2)
    struct pw_context* ctx = nullptr;
    struct pw_core* core = nullptr;
    struct pw_registry* registry = nullptr;
    struct spa_hook registryListener;
    struct pw_stream* stream = nullptr;
    struct spa_hook streamListener;

    // negotiated format
    int rate = 48000;
    int channels = 2;
    int frameSamples = 960;

    // accumulator (interleaved F32, floats)
    std::vector<float> accum;

    OpusAudioEncoder enc;
    AudioCapture::PacketCallback onPacket;

    // registry bookkeeping
    std::string defaultSink;   // e.g. alsa_output.pci-...iec958-stereo
    uint32_t sinkNodeId = 0;
    bool haveSinkNode = false;

    std::string error;
};

namespace {

void OnRegistryGlobal(void* data, uint32_t id, uint32_t permissions,
                      const char* type, uint32_t version,
                      const struct spa_dict* props)
{
    AudioCapture::Impl* impl = static_cast<AudioCapture::Impl*>(data);
    if (impl->haveSinkNode || props == nullptr) {
        return;
    }
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
        return;
    }
    const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char* nodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (mediaClass == nullptr || strcmp(mediaClass, "Audio/Sink") != 0) {
        return;
    }
    // when the core publishes default.audio.sink, match that node; otherwise
    // accept the first audio sink (some setups never set the property)
    if (!impl->defaultSink.empty() &&
        (nodeName == nullptr || impl->defaultSink != nodeName)) {
        return;
    }
    impl->sinkNodeId = id;
    impl->haveSinkNode = true;
    fprintf(stderr, "[audio] PipeWire sink node %u (%s)\n", id,
            nodeName ? nodeName : "(unnamed)");
    fflush(stderr);
}

void OnRegistryRemove(void* data, uint32_t id)
{
    AudioCapture::Impl* impl = static_cast<AudioCapture::Impl*>(data);
    if (id == impl->sinkNodeId) {
        impl->haveSinkNode = false;
        impl->sinkNodeId = 0;
    }
}

const struct pw_registry_events s_registryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = OnRegistryGlobal,
    .global_remove = OnRegistryRemove,
};

void OnStreamStateChanged(void* data, enum pw_stream_state oldState,
                          enum pw_stream_state state, const char* error)
{
    AudioCapture::Impl* impl = static_cast<AudioCapture::Impl*>(data);
    fprintf(stderr, "[audio] PipeWire stream state: %s%s%s\n",
            pw_stream_state_as_string(state),
            error ? " (" : "",
            error ? error : "");
    if (state == PW_STREAM_STATE_ERROR) {
        impl->accum.clear();
    }
}

void OnStreamProcess(void* data)
{
    AudioCapture::Impl* impl = static_cast<AudioCapture::Impl*>(data);
    struct pw_buffer* b = pw_stream_dequeue_buffer(impl->stream);
    if (b == nullptr) {
        return;
    }
    struct spa_buffer* sb = b->buffer;
    if (sb->datas[0].data != nullptr && sb->datas[0].chunk != nullptr &&
        sb->datas[0].chunk->size > 0) {
        const float* samples =
            static_cast<const float*>(sb->datas[0].data);
        const uint32_t n =
            sb->datas[0].chunk->size / sizeof(float); // interleaved values
        impl->accum.insert(impl->accum.end(), samples, samples + n);
        const size_t frameVals = (size_t)impl->frameSamples * impl->channels;
        while (impl->accum.size() >= frameVals) {
            std::vector<uint8_t> packet;
            packet.reserve(512);
            if (impl->enc.EncodeFloat(impl->accum.data(), impl->frameSamples,
                                      packet)) {
                if (impl->onPacket) {
                    impl->onPacket(packet.data(), packet.size());
                }
            }
            impl->accum.erase(impl->accum.begin(),
                              impl->accum.begin() + frameVals);
        }
        // cap the accumulator (if the consumer stalls, drop audio rather
        // than grow without bound)
        if (impl->accum.size() > frameVals * 8) {
            impl->accum.erase(impl->accum.begin(),
                              impl->accum.begin() + (impl->accum.size() - frameVals));
        }
    }
    pw_stream_queue_buffer(impl->stream, b);
}

const struct pw_stream_events s_streamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = OnStreamStateChanged,
    .process = OnStreamProcess,
};

} // namespace

// ------------------------------------------------------------------ impl

AudioCapture::AudioCapture() = default;

AudioCapture::~AudioCapture()
{
    Stop();
}

bool AudioCapture::Start(int sampleRate, int channels, int frameSamples,
                         PacketCallback cb)
{
    if (m_running.load()) {
        return true;
    }
    if (sampleRate <= 0 || channels <= 0 || frameSamples <= 0 || !cb) {
        return false;
    }

    m_impl = std::make_unique<Impl>();
    m_impl->rate = sampleRate;
    m_impl->channels = channels;
    m_impl->frameSamples = frameSamples;
    m_impl->onPacket = std::move(cb);

    m_ready = false;
    m_running.store(true);
    m_thread = std::thread(&AudioCapture::LoopMain, this);

    // wait (bounded) for the capture thread to finish the PipeWire setup
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_readyCv.wait_for(lock, std::chrono::seconds(8),
                                [this] { return m_ready; })) {
            fprintf(stderr, "[audio] setup timed out\n");
            m_running.store(false);
            if (m_thread.joinable()) {
                m_thread.join();
            }
            m_impl.reset();
            return false;
        }
    }
    if (!m_ready || !m_impl) {
        return false; // error string logged by the capture thread
    }
    return true;
}

void AudioCapture::LoopMain()
{
    // ALL pipewire work runs on this thread (single-threaded pump, like the
    // working standalone probe): loop, context, registry discovery, stream
    // creation + connect, then the iterate pump.
    auto fail = [this](const char* why) {
        m_impl->error = why;
        fprintf(stderr, "[audio] %s\n", why);
        fflush(stderr);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ready = true; // signaled with m_impl->error set; Start() checks
        }
        m_readyCv.notify_all();
    };

    pw_init(nullptr, nullptr);
    m_impl->loop = pw_loop_new(nullptr);
    if (m_impl->loop == nullptr) {
        fail("pw_loop_new failed");
        return;
    }
    m_impl->ctx = pw_context_new(m_impl->loop, nullptr, 0);
    if (m_impl->ctx == nullptr) {
        fail("pw_context_new failed (is pipewire running?)");
        return;
    }
    m_impl->core = pw_context_connect(m_impl->ctx, nullptr, 0);
    if (m_impl->core == nullptr) {
        fail("pw_context_connect failed (is pipewire running?)");
        return;
    }

    // find an audio sink node (its monitor carries the desktop audio):
    // prefer the core's default.audio.sink name when present, otherwise the
    // first Audio/Sink (see OnRegistryGlobal)
    const struct pw_properties* coreProps = pw_core_get_properties(m_impl->core);
    const char* defSink =
        coreProps ? pw_properties_get(coreProps, "default.audio.sink") : nullptr;
    if (defSink != nullptr) {
        m_impl->defaultSink = defSink;
        fprintf(stderr, "[audio] default sink: %s\n", defSink);
    } else {
        fprintf(stderr,
                "[audio] no default.audio.sink property — using the first audio sink\n");
    }
    m_impl->registry = pw_core_get_registry(m_impl->core,
                                            PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(m_impl->registry, &m_impl->registryListener,
                             &s_registryEvents, m_impl.get());
    // pump until the sink node is found (bounded)
    for (int i = 0; i < 200 && !m_impl->haveSinkNode; ++i) {
        pw_loop_iterate(m_impl->loop, 20);
    }
    if (!m_impl->haveSinkNode) {
        fail("no audio sink node found — no desktop audio");
        return;
    }

    // Opus encoder
    if (!m_impl->enc.Init(m_impl->rate, m_impl->channels, m_impl->frameSamples)) {
        fail("opus encoder init failed");
        return;
    }

    // the audio stream: input direction, targeted at the sink node (PipeWire
    // routes it to the sink's monitor ports)
    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Communication",
        nullptr);
    m_impl->stream = pw_stream_new(m_impl->core, kAudioRole, props);
    if (m_impl->stream == nullptr) {
        fail("pw_stream_new failed");
        return;
    }
    pw_stream_add_listener(m_impl->stream, &m_impl->streamListener,
                           &s_streamEvents, m_impl.get());

    uint8_t podBuf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podBuf, sizeof(podBuf));
    uint32_t audioPos[2] = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR};
    const struct spa_pod* params[1];
    params[0] = (const struct spa_pod*)spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_F32_LE),
        SPA_FORMAT_AUDIO_rate, SPA_POD_Int(m_impl->rate),
        SPA_FORMAT_AUDIO_channels, SPA_POD_Int(m_impl->channels),
        SPA_FORMAT_AUDIO_position,
        SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Id, 2, audioPos));

    const enum pw_stream_flags flags =
        (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                               PW_STREAM_FLAG_MAP_BUFFERS);
    if (pw_stream_connect(m_impl->stream, PW_DIRECTION_INPUT,
                          m_impl->sinkNodeId, flags,
                          params, 1) < 0) {
        fail("pw_stream_connect failed");
        return;
    }

    // success: tell Start() we are up, then pump
    fprintf(stderr, "[audio] capture started (%dHz %dch %d-samples/frame)\n",
            m_impl->rate, m_impl->channels, m_impl->frameSamples);
    fflush(stderr);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ready = true;
    }
    m_readyCv.notify_all();

    while (m_running.load() && m_impl) {
        pw_loop_iterate(m_impl->loop, 10);
    }
    // final drain
    if (m_impl) {
        pw_loop_iterate(m_impl->loop, 0);
    }
}

void AudioCapture::Stop()
{
    m_running.store(false);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_impl) {
        if (m_impl->stream != nullptr) {
            pw_stream_disconnect(m_impl->stream);
            pw_stream_destroy(m_impl->stream);
        }
        if (m_impl->registry != nullptr) {
            pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(m_impl->registry));
        }
        if (m_impl->core != nullptr) {
            pw_core_disconnect(m_impl->core);
        }
        if (m_impl->ctx != nullptr) {
            pw_context_destroy(m_impl->ctx);
        }
        if (m_impl->loop != nullptr) {
            pw_loop_destroy(m_impl->loop);
        }
        m_impl.reset();
    }
}

} // namespace sdk

#endif // STARDESK_HAS_AUDIO
