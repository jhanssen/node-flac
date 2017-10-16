#include <nan.h>
#include <uv.h>
#include <node.h>
#include <node_buffer.h>
#include <FLAC/stream_decoder.h>
#include <variant>
#include <cstring>

template<typename E>
constexpr auto to_underlying(E e) noexcept
{
    return static_cast<std::underlying_type_t<E>>(e);
}

struct Data
{
    Data();

    bool opened, stopped;
    Nan::Persistent<v8::Context> context;
    Nan::Persistent<v8::Function> callback;
    v8::Isolate* isolate;
    FLAC__StreamDecoder decoder;

    uv_async_t async;
    uv_thread_t thread;
    uv_mutex_t mutex;
    uv_cond_t cond;

    std::vector<std::string> inbuffers, inlocalBuffers;

    struct Format
    {
        uint32_t sampleRate;
        uint32_t channels;
        uint32_t bitsPerSample;
    };

    struct Metadata
    {
        std::vector<std::pair<std::string, std::string> > tags;
    };

    struct Message
    {
        enum class Type { Format, Metadata, Data };

        Type type;
        std::variant<Format, Metadata, std::string> data;
    };

    std::vector<Message> messages;

    Format currentFormat;

    bool formatChanged(const FLAC__Frame* frame) const;
    void pushFormat(const FLAC__Frame* frame);

    static FLAC__StreamDecoderReadStatus readCallback(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data);
    static FLAC__StreamDecoderWriteStatus writeCallback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *client_data);
    static void metadataCallback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data);
    static void errorCallback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data);

    static void flacThread(void* arg);
    static void asyncCallback(uv_async_t* handle);
} data;

Data::Data()
    : opened(false), stopped(false)
{
    memset(&currentFormat, '\0', sizeof(currentFormat));
}

inline bool Data::formatChanged(const FLAC__Frame* frame) const
{
    if (frame->header.sample_rate != data.currentFormat.sampleRate
        || frame->header.channels != data.currentFormat.channels
        || frame->header.bits_per_sample != data.currentFormat.bitsPerSample)
        return true;
    return false;
}

inline void Data::pushFormat(const FLAC__Frame* frame)
{
    currentFormat.sampleRate = frame->header.sample_rate;
    currentFormat.channels = frame->header.channels;
    currentFormat.bitsPerSample = frame->header.bits_per_sample;
    messages.push_back(Message{ Message::Type::Format, currentFormat });
}

FLAC__StreamDecoderReadStatus Data::readCallback(const FLAC__StreamDecoder */*decoder*/, FLAC__byte buffer[], size_t *bytes, void */*client_data*/)
{
    size_t rem = *bytes, where = 0;
    while (rem && !data.inlocalBuffers.empty()) {
        auto& front = data.inlocalBuffers.front();
        const size_t toread = std::min(front.size(), rem);
        memcpy(buffer + where, front.data(), toread);
        rem -= toread;
        where += toread;
        if (toread == front.size()) {
            data.inlocalBuffers.erase(data.inlocalBuffers.begin());
        }
    }
    if (where > 0) {
        *bytes = where;
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}

FLAC__StreamDecoderWriteStatus Data::writeCallback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *client_data)
{
    uv_mutex_lock(&data.mutex);
    if (data.formatChanged(frame)) {
        data.pushFormat(frame);
        uv_async_send(&data.async);
    }
    const uint32_t frameSamples = frame->header.blocksize * frame->header.channels * frame->header.bits_per_sample;

    std::string dt;
    dt.resize(frameSamples);
    unsigned char* ptr = reinterpret_cast<unsigned char*>(&dt[0]);

    for (unsigned i = 0; i < frame->header.blocksize; ++i) {
        for (unsigned int j = 0; j < frame->header.channels; ++j) {
            switch (frame->header.bits_per_sample) {
            case 8:
                *(ptr++) = buffer[j][i];
                break;
            case 16:
                memcpy(ptr, buffer[j] + i, 2);
                ptr += 2;
                break;
            case 24:
                memcpy(ptr, buffer[j] + i, 3);
                ptr += 3;
                break;
            case 32:
                memcpy(ptr, buffer[j] + i, 4);
                ptr += 4;
                break;
            }
        }
    }

    if (!dt.empty()) {
        data.messages.push_back(Message{ Message::Type::Data, dt });
        uv_async_send(&data.async);
    }

    uv_mutex_unlock(&data.mutex);

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void Data::metadataCallback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
    if (metadata->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
        const auto& vorbis = metadata->data.vorbis_comment;
        Metadata meta;
        auto split = [&meta](const FLAC__StreamMetadata_VorbisComment_Entry& entry) {
            const char* eq = static_cast<char*>(std::memchr(entry.entry, '=', entry.length));
            if (eq == nullptr)
                return;
            const char* estr = reinterpret_cast<char*>(entry.entry);
            meta.tags.push_back(std::make_pair(std::string(estr, eq),
                                               std::string(eq + 1, estr + entry.length)));
        };
        uv_mutex_lock(&data.mutex);
        split(vorbis.vendor_string);
        for (uint32_t i = 0; i < vorbis.num_comments; ++i) {
            split(vorbis.comments[i]);
        }
        data.messages.push_back(Message{ Message::Type::Metadata, std::move(meta) });
        uv_async_send(&data.async);
        uv_mutex_unlock(&data.mutex);
    }
}

void Data::errorCallback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
}

void Data::flacThread(void* arg)
{
    uv_mutex_lock(&data.mutex);
    for (;;) {
        uv_cond_wait(&data.cond, &data.mutex);
        if (data.stopped) {
            uv_mutex_unlock(&data.mutex);
            return;
        }

        if (!data.inbuffers.empty()) {
            data.inlocalBuffers = std::move(data.inbuffers);
            uv_mutex_unlock(&data.mutex);
            if (!FLAC__stream_decoder_process_until_end_of_stream(&data.decoder)) {
                // badness, not sure what to do yet
            }

            if (!data.inlocalBuffers.empty()) {
                // prepend to buffers
                // if this happens often then it might be better to keep the data in localBuffers and append buffers instead
                uv_mutex_lock(&data.mutex);
                data.inbuffers.insert(data.inbuffers.begin(), data.inlocalBuffers.begin(), data.inlocalBuffers.end());
                data.inlocalBuffers.clear();
                continue;
            }
            uv_mutex_lock(&data.mutex);
        }
    }
}

void Data::asyncCallback(uv_async_t* handle)
{
    std::vector<Data::Message> localMessages;
    uv_mutex_lock(&data.mutex);
    localMessages = std::move(data.messages);
    uv_mutex_unlock(&data.mutex);

    Nan::HandleScope scope;
    v8::Local<v8::Context> context = v8::Local<v8::Context>::New(data.isolate, data.context);
    v8::Local<v8::Function> callback = v8::Local<v8::Function>::New(data.isolate, data.callback);

    for (const auto& message : localMessages) {
        switch (message.type) {
        case Data::Message::Type::Format: {
            const auto& format = std::get<Data::Format>(message.data);
            v8::Local<v8::Object> formatObj = v8::Object::New(data.isolate);
            formatObj->Set(v8::String::NewFromUtf8(data.isolate, "sampleRate"), v8::Integer::New(data.isolate, format.sampleRate));

            std::vector<v8::Local<v8::Value> > values;
            values.push_back(v8::Local<v8::Value>(v8::Integer::New(data.isolate, to_underlying(Data::Message::Type::Format))));
            values.push_back(v8::Local<v8::Value>(std::move(formatObj)));
            callback->Call(context, callback, values.size(), &values[0]);
            break; }
        case Data::Message::Type::Metadata: {
            const auto& meta = std::get<Data::Metadata>(message.data);
            v8::Local<v8::Object> metaObj = v8::Object::New(data.isolate);
            for (const auto& p : meta.tags) {
                metaObj->Set(v8::String::NewFromUtf8(data.isolate, p.first.c_str()),
                             v8::String::NewFromUtf8(data.isolate, p.second.c_str()));
            }

            std::vector<v8::Local<v8::Value> > values;
            values.push_back(v8::Local<v8::Value>(v8::Integer::New(data.isolate, to_underlying(Data::Message::Type::Metadata))));
            values.push_back(v8::Local<v8::Value>(std::move(metaObj)));
            callback->Call(context, callback, values.size(), &values[0]);
            break; }
        case Data::Message::Type::Data: {
            const auto& str = std::get<std::string>(message.data);
            v8::Local<v8::Object> bufferObj = Nan::NewBuffer(str.size()).ToLocalChecked();
            {
                char* data = node::Buffer::Data(bufferObj);
                memcpy(data, &str[0], str.size());
            }

            std::vector<v8::Local<v8::Value> > values;
            values.push_back(v8::Local<v8::Value>(v8::Integer::New(data.isolate, to_underlying(Data::Message::Type::Data))));
            values.push_back(v8::Local<v8::Value>(std::move(bufferObj)));
            callback->Call(context, callback, values.size(), &values[0]);
            break; }
        }
    }
}

NAN_METHOD(Open) {
    if (data.opened) {
        Nan::ThrowError("Already open");
        return;
    }
    data.isolate = info.GetIsolate();
    data.stopped = false;
    if (!info[0]->IsFunction()) {
        Nan::ThrowError("Argument must be a function");
        return;
    }
    data.context.Reset(Nan::GetCurrentContext());
    data.callback.Reset(v8::Local<v8::Function>::Cast(info[0]));

    if (FLAC__stream_decoder_init_stream(&data.decoder,
                                         data.readCallback,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         data.writeCallback,
                                         data.metadataCallback,
                                         data.errorCallback,
                                         &data) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        Nan::ThrowError("Failed to initialize flac stream");
        return;
    }

    if (uv_async_init(uv_default_loop(), &data.async, data.asyncCallback) < 0) {
        FLAC__stream_decoder_finish(&data.decoder);
        FLAC__stream_decoder_delete(&data.decoder);
        Nan::ThrowError("Failed to init async handle");
        return;
    }

    if (uv_mutex_init(&data.mutex) < 0) {
        FLAC__stream_decoder_finish(&data.decoder);
        FLAC__stream_decoder_delete(&data.decoder);
        Nan::ThrowError("Failed to init mutex");
        return;
    }

    if (uv_cond_init(&data.cond) < 0) {
        FLAC__stream_decoder_finish(&data.decoder);
        FLAC__stream_decoder_delete(&data.decoder);
        uv_mutex_destroy(&data.mutex);
        Nan::ThrowError("Failed to init mutex");
        return;
    }

    if (uv_thread_create(&data.thread, data.flacThread, &data) < 0) {
        FLAC__stream_decoder_finish(&data.decoder);
        FLAC__stream_decoder_delete(&data.decoder);
        uv_cond_destroy(&data.cond);
        uv_mutex_destroy(&data.mutex);
        Nan::ThrowError("Failed to init thread");
        return;
    }
    data.opened = true;
}

NAN_METHOD(Close) {
    if (!data.opened) {
        Nan::ThrowError("Not open");
        return;
    }
    if (!data.stopped) {
        uv_mutex_lock(&data.mutex);
        data.stopped = true;
        uv_cond_signal(&data.cond);
        uv_mutex_unlock(&data.mutex);
        uv_thread_join(&data.thread);
    }

    data.context.Reset();
    data.callback.Reset();

    FLAC__stream_decoder_finish(&data.decoder);
    FLAC__stream_decoder_delete(&data.decoder);
    uv_cond_destroy(&data.cond);
    uv_mutex_destroy(&data.mutex);
    data.opened = false;
}

NAN_METHOD(Feed) {
    if (!node::Buffer::HasInstance(info[0])) {
        Nan::ThrowError("Feed needs a Buffer argument");
        const char* dt = node::Buffer::Data(info[0]);
        const size_t size = node::Buffer::Length(info[0]);

        uv_mutex_lock(&data.mutex);
        data.inbuffers.push_back(std::string(dt, size));
        uv_cond_signal(&data.cond);
        uv_mutex_unlock(&data.mutex);
    }
}

NAN_MODULE_INIT(Initialize) {
    NAN_EXPORT(target, Open);
    NAN_EXPORT(target, Feed);
    NAN_EXPORT(target, Close);
}

NODE_MODULE(flac, Initialize)
