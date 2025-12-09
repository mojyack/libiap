#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/names.h>

#include "macros/assert.hpp"
#include "macros/autoptr.hpp"
#include "pw.hpp"
#include "util/cleaner.hpp"

namespace pw {
namespace {
declare_autoptr(PWThreadLoop, pw_thread_loop, pw_thread_loop_destroy);
declare_autoptr(PWStream, pw_stream, pw_stream_destroy);
declare_autoptr(PWProperties, pw_properties, pw_properties_clear);
} // namespace

struct Context {
    AutoPWThreadLoop thread_loop;
    AutoPWStream     capture_stream;
    AutoPWStream     playback_stream;
    spa_audio_info   capture_format;
};

namespace {
constexpr auto playback_channels = 2;

auto capture_on_process(void* const userdata) -> void {
    auto& context = *std::bit_cast<Context*>(userdata);

    const auto pw_buffer = pw_stream_dequeue_buffer(context.capture_stream.get());
    ensure(pw_buffer != NULL);
    auto cleaner = Cleaner{[&] { pw_stream_queue_buffer(context.capture_stream.get(), pw_buffer); }};

    const auto buffer  = pw_buffer->buffer;
    const auto samples = std::bit_cast<int16_t*>(buffer->datas[0].data);
    ensure(samples != NULL);

    const auto num_channels = context.capture_format.info.raw.channels;
    const auto num_samples  = buffer->datas[0].chunk->size / sizeof(int16_t);

    on_capture(samples, num_samples, num_channels);
}

auto capture_on_stream_param_changed(void* const userdata, const uint32_t id, const spa_pod* const param) -> void {
    auto& context = *std::bit_cast<Context*>(userdata);

    if(param == NULL || id != SPA_PARAM_Format) {
        return;
    }

    if(spa_format_parse(param, &context.capture_format.media_type, &context.capture_format.media_subtype) < 0) {
        return;
    }

    if(context.capture_format.media_type != SPA_MEDIA_TYPE_audio || context.capture_format.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }

    ensure(spa_format_audio_raw_parse(param, &context.capture_format.info.raw) >= 0);
}

auto playback_on_process(void* const userdata) -> void {
    auto& context = *std::bit_cast<Context*>(userdata);

    const auto pw_buffer = pw_stream_dequeue_buffer(context.playback_stream.get());
    ensure(pw_buffer != NULL);
    auto cleaner = Cleaner{[&] { pw_stream_queue_buffer(context.playback_stream.get(), pw_buffer); }};

    const auto buffer  = pw_buffer->buffer;
    const auto samples = std::bit_cast<int16_t*>(buffer->datas[0].data);
    ensure(samples != NULL);

    constexpr auto stride = sizeof(int16_t) * playback_channels;

    auto num_frames = buffer->datas[0].maxsize / stride;
    if(pw_buffer->requested != 0) {
        num_frames = std::min(pw_buffer->requested, num_frames);
    }

    const auto copied = on_playback(samples, num_frames * playback_channels);

    const auto chunk = buffer->datas[0].chunk;
    chunk->offset    = 0;
    chunk->stride    = stride;
    chunk->size      = copied * sizeof(int16_t);
}

const auto capture_stream_events = pw_stream_events{
    .version       = PW_VERSION_STREAM_EVENTS,
    .param_changed = capture_on_stream_param_changed,
    .process       = capture_on_process,
};

const auto playback_stream_events = pw_stream_events{
    .version = PW_VERSION_STREAM_EVENTS,
    .process = playback_on_process,
};
} // namespace

auto init(const size_t capture_rate, const size_t playback_rate) -> Context* {
    pw_init(NULL, NULL);

    auto context = std::unique_ptr<Context>(new Context());

    context->thread_loop.reset(pw_thread_loop_new("pw", NULL));
    ensure(context->thread_loop.get() != NULL);
    const auto loop = pw_thread_loop_get_loop(context->thread_loop.get());

    const auto setup_stream = [loop, context = context.get()](const AutoPWProperties props, const char* const name, const pw_stream_events& events, const spa_audio_info_raw format, const spa_direction direction) -> AutoPWStream {
        constexpr auto error_value = nullptr;
        ensure_v(props.get() != NULL);

        auto stream = AutoPWStream(pw_stream_new_simple(loop, name, props.get(), &events, context));
        ensure_v(stream.get() != NULL);

        // "The POD start is always aligned to 8 bytes."
        alignas(8) auto pod_builder_buffer = std::array<std::byte, 1024>();
        auto            pod_builder        = spa_pod_builder{.data = pod_builder_buffer.data(), .size = pod_builder_buffer.size()};

        const auto params = std::array{
            spa_format_audio_raw_build(&pod_builder, SPA_PARAM_EnumFormat, &format),
        };

        ensure_v(pw_stream_connect(stream.get(),
                                   direction,
                                   PW_ID_ANY,
                                   pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT |
                                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                                   PW_STREAM_FLAG_RT_PROCESS),
                                   (const spa_pod**)params.data(), params.size()) == 0);

        return stream;
    };

    if(capture_rate > 0) {
        context->capture_stream = setup_stream(
            AutoPWProperties(pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio",
                PW_KEY_MEDIA_CATEGORY, "Capture",
                PW_KEY_MEDIA_ROLE, "Music",
                PW_KEY_NODE_NICK, "Rockbox media player",
                NULL)),
            "audio-capture",
            capture_stream_events,
            {.format = SPA_AUDIO_FORMAT_S16, .rate = uint32_t(capture_rate)},
            SPA_DIRECTION_INPUT);
        ensure(context->capture_stream.get() != NULL);
    }

    if(playback_rate > 0) {
        context->playback_stream = setup_stream(
            AutoPWProperties(pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio",
                PW_KEY_MEDIA_CATEGORY, "Playback",
                PW_KEY_MEDIA_ROLE, "Music",
                NULL)),
            "audio-playback",
            playback_stream_events,
            {.format = SPA_AUDIO_FORMAT_S16, .rate = uint32_t(playback_rate), .channels = playback_channels},
            SPA_DIRECTION_OUTPUT);
        ensure(context->playback_stream.get() != NULL);
    }

    return context.release();
}

auto run(Context* const context) -> void {
    pw_thread_loop_lock(context->thread_loop.get());
    pw_thread_loop_start(context->thread_loop.get());
    pw_thread_loop_unlock(context->thread_loop.get());
}

auto finish(Context* const context) -> void {
    pw_thread_loop_stop(context->thread_loop.get());
    context->capture_stream.reset();
    context->playback_stream.reset();
    delete context;
    pw_deinit();
}
} // namespace pw
