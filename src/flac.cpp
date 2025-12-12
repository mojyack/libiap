#include <FLAC++/decoder.h>
#include <stdio.h>
#include <string.h>

#include "flac.hpp"
#include "macros/assert.hpp"
#include "macros/autoptr.hpp"

declare_autoptr(File, FILE, fclose);

class Decoder : public FLAC::Decoder::File {
  private:
    AudioFile* file;

    auto metadata_callback(const FLAC__StreamMetadata* const metadata) -> void override {
        switch(metadata->type) {
        case FLAC__METADATA_TYPE_STREAMINFO: {
            const auto& info    = metadata->data.stream_info;
            file->total_samples = info.total_samples;
            file->sample_rate   = info.sample_rate;
            file->channels      = info.channels;
            if(info.bits_per_sample != 16) {
                WARN("bps({}) != 16", info.bits_per_sample);
            }
        } break;
        case FLAC__METADATA_TYPE_VORBIS_COMMENT: {
            const auto& info = metadata->data.vorbis_comment;
            for(auto i = 0u; i < info.num_comments; i += 1) {
                auto& comment = info.comments[i];
                auto& str     = file->comments.emplace_back();
                str.resize(comment.length);
                memcpy(str.data(), comment.entry, comment.length);
            }
        } break;
        case FLAC__METADATA_TYPE_PICTURE: {
            const auto& info = metadata->data.picture;
            if(info.type == FLAC__STREAM_METADATA_PICTURE_TYPE_FRONT_COVER) {
                file->cover.resize(info.data_length);
                memcpy(file->cover.data(), info.data, info.data_length);
            }
        } break;
        default:
            break;
        }
    }

    auto write_callback(const FLAC__Frame* const frame, const FLAC__int32* const buffer[]) -> FLAC__StreamDecoderWriteStatus override {
        for(auto i = 0u; i < frame->header.blocksize; i += 1) {
            for(auto c = 0u; c < file->channels; c += 1) {
                file->data.push_back(buffer[c][i]);
            }
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    auto error_callback(const FLAC__StreamDecoderErrorStatus /*status*/) -> void override {
    }

  public:
    Decoder(AudioFile& file) : file(&file) {
    }
};

auto decode_flac(const char* path, bool metadata) -> std::optional<AudioFile> {
    auto file = AutoFile(fopen(path, "rb"));
    ensure(file.get() != NULL);

    auto ret     = AudioFile();
    auto decoder = Decoder(ret);
    ensure(decoder.init(file.get()) == FLAC__STREAM_DECODER_INIT_STATUS_OK);
    if(metadata) {
        ensure(decoder.set_metadata_respond(FLAC__METADATA_TYPE_PICTURE));
        ensure(decoder.set_metadata_respond(FLAC__METADATA_TYPE_VORBIS_COMMENT));
    }
    [[maybe_unused]] auto _ = file.release(); // moved to decoder
    if(metadata) {
        ensure(decoder.process_until_end_of_metadata());
    } else {
        ensure(decoder.process_until_end_of_stream());
    }
    return ret;
}
