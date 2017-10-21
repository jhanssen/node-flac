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
    ~Data();

    static uint32_t openCount;
    static Nan::Persistent<v8::Private> extName;

    bool stopped, needsDone;
    Nan::Persistent<v8::Context> context;
    Nan::Persistent<v8::Function> callback;
    Nan::Persistent<v8::Object> weak;
    v8::Isolate* isolate;
    FLAC__StreamDecoder* decoder;

    uv_async_t async;
    uv_thread_t thread;
    uv_mutex_t mutex;
    uv_cond_t cond;

    struct BufferData
    {
        size_t where;
        std::string buffer;
    };
    std::vector<BufferData> inbuffers;

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
        enum class Type { Format, Metadata, Data, Done, End };

        Type type;
        std::variant<Format, Metadata, std::string> data;
    };

    std::vector<Message> messages;

    Format currentFormat;

    bool formatChanged(const FLAC__Frame* frame) const;
    void pushFormat(const FLAC__Frame* frame);

    void close();

    static FLAC__StreamDecoderReadStatus readCallback(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data);
    static FLAC__StreamDecoderWriteStatus writeCallback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *client_data);
    static void metadataCallback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data);
    static void errorCallback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data);

    static void flacThread(void* arg);
    static void asyncCallback(uv_async_t* handle);

    static void weakCallback(const Nan::WeakCallbackInfo<Data> &data);
};

uint32_t Data::openCount = 0;
Nan::Persistent<v8::Private> Data::extName;

Data::Data()
    : stopped(false), needsDone(false), decoder(nullptr)
{
    memset(&currentFormat, '\0', sizeof(currentFormat));
    memset(&async, '\0', sizeof(async));

    uv_mutex_init(&mutex);
    uv_cond_init(&cond);
}

Data::~Data()
{
    uv_cond_destroy(&cond);
    uv_mutex_destroy(&mutex);
}

inline bool Data::formatChanged(const FLAC__Frame* frame) const
{
    uint32_t bps = frame->header.bits_per_sample;
    if (bps == 24)
        bps = 32;
    if (frame->header.sample_rate != currentFormat.sampleRate
        || frame->header.channels != currentFormat.channels
        || bps != currentFormat.bitsPerSample)
        return true;
    return false;
}

inline void Data::pushFormat(const FLAC__Frame* frame)
{
    currentFormat.sampleRate = frame->header.sample_rate;
    currentFormat.channels = frame->header.channels;
    uint32_t bps = frame->header.bits_per_sample;
    if (bps == 24)
        bps = 32;
    currentFormat.bitsPerSample = bps;
    messages.push_back(Message{ Message::Type::Format, currentFormat });
}

FLAC__StreamDecoderReadStatus Data::readCallback(const FLAC__StreamDecoder */*decoder*/, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
    Data* data = static_cast<Data*>(client_data);
    while (!data->stopped && data->inbuffers.empty()) {
        // if we need more data, wait
        if (data->needsDone) {
            data->needsDone = false;
            data->messages.push_back(Message{ Message::Type::Done, std::string() });
            uv_async_send(&data->async);
        }
        uv_cond_wait(&data->cond, &data->mutex);
    }
    if (data->stopped) {
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    data->needsDone = true;
    size_t rem = *bytes, where = 0;
    while (rem && !data->inbuffers.empty()) {
        auto& front = data->inbuffers.front();

        const size_t toread = std::min(front.buffer.size() - front.where, rem);
        memcpy(buffer + where, front.buffer.data() + front.where, toread);
        rem -= toread;
        where += toread;
        if (toread == front.buffer.size() - front.where) {
            data->inbuffers.erase(data->inbuffers.begin());
        } else {
            front.where += toread;
        }
    }

    *bytes = where;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderWriteStatus Data::writeCallback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *client_data)
{
    Data* data = static_cast<Data*>(client_data);
    if (data->formatChanged(frame)) {
        data->pushFormat(frame);
        uv_async_send(&data->async);
    }
    uint32_t bps = frame->header.bits_per_sample;
    if (bps == 24)
        bps = 32;
    const uint32_t frameSamples = frame->header.blocksize * frame->header.channels * (bps / 8);

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
                *(ptr++) = buffer[j][i];
                *(ptr++) = buffer[j][i] >> 8;
                break;
            case 24:
                *(ptr++) = 0;
                *(ptr++) = buffer[j][i];
                *(ptr++) = buffer[j][i] >> 8;
                *(ptr++) = buffer[j][i] >> 16;
                break;
            case 32:
                *(ptr++) = buffer[j][i];
                *(ptr++) = buffer[j][i] >> 8;
                *(ptr++) = buffer[j][i] >> 16;
                *(ptr++) = buffer[j][i] >> 24;
                break;
            }
        }
    }

    // printf("wrote %u (%u)\n", frameSamples, ptr - reinterpret_cast<unsigned char*>(&dt[0]));

    if (!dt.empty()) {
        data->messages.push_back(Message{ Message::Type::Data, std::move(dt) });
        uv_async_send(&data->async);
    }

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void Data::metadataCallback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
    // printf("!!meta %d\n", metadata->type);
    Data* data = static_cast<Data*>(client_data);
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
        uv_mutex_lock(&data->mutex);
        split(vorbis.vendor_string);
        for (uint32_t i = 0; i < vorbis.num_comments; ++i) {
            split(vorbis.comments[i]);
        }
        data->messages.push_back(Message{ Message::Type::Metadata, std::move(meta) });
        uv_async_send(&data->async);
        uv_mutex_unlock(&data->mutex);
    }
}

void Data::errorCallback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
}

void Data::flacThread(void* arg)
{
    Data* data = static_cast<Data*>(arg);

    uv_mutex_lock(&data->mutex);
    for (;;) {
        if (!FLAC__stream_decoder_process_single(data->decoder)) {
            // badness, not sure what to do yet
        }

        if (data->stopped)
            break;
        if (FLAC__stream_decoder_get_state(data->decoder) == FLAC__STREAM_DECODER_END_OF_STREAM) {
            // end of stream, send a done if we haven't and close the decoder
            if (data->needsDone) {
                data->needsDone = false;
                data->messages.push_back(Message{ Message::Type::Done, std::string() });
            }
            data->messages.push_back(Message{ Message::Type::End, std::string() });
            uv_async_send(&data->async);
            break;
        }
    }
    uv_mutex_unlock(&data->mutex);
}

void Data::close()
{
    if (!decoder)
        return;

    if (!--Data::openCount) {
        Data::extName.Reset();
    }

    if (!stopped) {
        uv_mutex_lock(&mutex);
        stopped = true;
        uv_cond_signal(&cond);
        uv_mutex_unlock(&mutex);
        uv_thread_join(&thread);
    }

    context.Reset();
    callback.Reset();

    FLAC__stream_decoder_finish(decoder);
    FLAC__stream_decoder_delete(decoder);

    decoder = nullptr;
}

void Data::weakCallback(const Nan::WeakCallbackInfo<Data> &data)
{
    Data* param = data.GetParameter();
    param->close();
    if (param->async.data) {
        uv_close(reinterpret_cast<uv_handle_t*>(&param->async),
                 [](uv_handle_t* handle) { delete static_cast<Data*>(handle->data); });
    } else {
        delete param;
    }
}

void Data::asyncCallback(uv_async_t* handle)
{
    Data* data = static_cast<Data*>(handle->data);
    std::vector<Data::Message> localMessages;
    uv_mutex_lock(&data->mutex);
    localMessages = std::move(data->messages);
    uv_mutex_unlock(&data->mutex);

    Nan::HandleScope scope;
    v8::Local<v8::Context> context = v8::Local<v8::Context>::New(data->isolate, data->context);
    v8::Local<v8::Function> callback = v8::Local<v8::Function>::New(data->isolate, data->callback);

    for (const auto& message : localMessages) {
        switch (message.type) {
        case Data::Message::Type::Format: {
            const auto& format = std::get<Data::Format>(message.data);
            v8::Local<v8::Object> formatObj = v8::Object::New(data->isolate);
            formatObj->Set(v8::String::NewFromUtf8(data->isolate, "sampleRate"), v8::Integer::New(data->isolate, format.sampleRate));
            formatObj->Set(v8::String::NewFromUtf8(data->isolate, "channels"), v8::Integer::New(data->isolate, format.channels));
            formatObj->Set(v8::String::NewFromUtf8(data->isolate, "bitDepth"), v8::Integer::New(data->isolate, format.bitsPerSample));

            std::vector<v8::Local<v8::Value> > values;
            values.push_back(v8::Local<v8::Value>(v8::Integer::New(data->isolate, to_underlying(Data::Message::Type::Format))));
            values.push_back(v8::Local<v8::Value>(std::move(formatObj)));
            if (callback->Call(context, callback, values.size(), &values[0]).IsEmpty()) {
                Nan::ThrowError("Failed to call");
            }
            break; }
        case Data::Message::Type::Metadata: {
            const auto& meta = std::get<Data::Metadata>(message.data);
            v8::Local<v8::Object> metaObj = v8::Object::New(data->isolate);
            for (const auto& p : meta.tags) {
                metaObj->Set(v8::String::NewFromUtf8(data->isolate, p.first.c_str()),
                             v8::String::NewFromUtf8(data->isolate, p.second.c_str()));
            }

            std::vector<v8::Local<v8::Value> > values;
            values.push_back(v8::Local<v8::Value>(v8::Integer::New(data->isolate, to_underlying(Data::Message::Type::Metadata))));
            values.push_back(v8::Local<v8::Value>(std::move(metaObj)));
            if (callback->Call(context, callback, values.size(), &values[0]).IsEmpty()) {
                Nan::ThrowError("Failed to call");
            }
            break; }
        case Data::Message::Type::Data: {
            const auto& str = std::get<std::string>(message.data);
            v8::Local<v8::Object> bufferObj = Nan::NewBuffer(str.size()).ToLocalChecked();
            {
                char* data = node::Buffer::Data(bufferObj);
                memcpy(data, &str[0], str.size());
            }

            std::vector<v8::Local<v8::Value> > values;
            values.push_back(v8::Local<v8::Value>(v8::Integer::New(data->isolate, to_underlying(Data::Message::Type::Data))));
            values.push_back(v8::Local<v8::Value>(std::move(bufferObj)));
            if (callback->Call(context, callback, values.size(), &values[0]).IsEmpty()) {
                Nan::ThrowError("Failed to call");
            }
            break; }
        case Data::Message::Type::Done: {
            v8::Local<v8::Value> done = v8::Integer::New(data->isolate, to_underlying(message.type));
            if (callback->Call(context, callback, 1, &done).IsEmpty()) {
                Nan::ThrowError("Failed to call");
            }
            break; }
        case Data::Message::Type::End: {
            // decoder end and thread dead. join and stuff.
            data->close();

            v8::Local<v8::Value> end = v8::Integer::New(data->isolate, to_underlying(message.type));
            if (callback->Call(context, callback, 1, &end).IsEmpty()) {
                Nan::ThrowError("Failed to call");
            }
            break; }
        }
    }
}

NAN_METHOD(Open) {
    Data* data = new Data;
    data->isolate = info.GetIsolate();
    if (!info[0]->IsFunction()) {
        Nan::ThrowError("Argument must be a function");
        return;
    }
    data->context.Reset(Nan::GetCurrentContext());
    data->callback.Reset(v8::Local<v8::Function>::Cast(info[0]));

    data->decoder = FLAC__stream_decoder_new();
    if (data->decoder == nullptr) {
        Nan::ThrowError("Unable to create decoder");
        return;
    }

    if (FLAC__stream_decoder_init_stream(data->decoder,
                                         data->readCallback,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         data->writeCallback,
                                         data->metadataCallback,
                                         data->errorCallback,
                                         data) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        Nan::ThrowError("Failed to initialize flac stream");
        return;
    }

    if (uv_async_init(uv_default_loop(), &data->async, data->asyncCallback) < 0) {
        FLAC__stream_decoder_finish(data->decoder);
        FLAC__stream_decoder_delete(data->decoder);
        Nan::ThrowError("Failed to init async handle");
        return;
    }
    data->async.data = data;

    if (uv_thread_create(&data->thread, data->flacThread, data) < 0) {
        FLAC__stream_decoder_finish(data->decoder);
        FLAC__stream_decoder_delete(data->decoder);
        uv_cond_destroy(&data->cond);
        uv_mutex_destroy(&data->mutex);
        Nan::ThrowError("Failed to init thread");
        return;
    }

    auto ctx = Nan::GetCurrentContext();
    v8::Local<v8::External> ext = v8::External::New(data->isolate, data);
    v8::Local<v8::Object> weak = v8::Object::New(data->isolate);
    v8::Local<v8::Private> extName;
    if (Data::extName.IsEmpty()) {
        extName = v8::Private::New(data->isolate, v8::String::NewFromUtf8(data->isolate, "ext"));
        Data::extName.Reset(extName);
    } else {
        extName = v8::Local<v8::Private>::New(data->isolate, Data::extName);
    }
    ++Data::openCount;
    weak->SetPrivate(ctx, extName, ext);
    data->weak.Reset(weak);
    data->weak.SetWeak(data, Data::weakCallback, Nan::WeakCallbackType::kParameter);
    info.GetReturnValue().Set(weak);
}

NAN_METHOD(Close) {
    if (!info[0]->IsObject()) {
        Nan::ThrowError("Argument must be an object");
        return;
    }

    auto iso = info.GetIsolate();
    auto ctx = Nan::GetCurrentContext();
    v8::Local<v8::Object> obj = v8::Local<v8::Object>::Cast(info[0]);
    v8::Local<v8::Private> extName = v8::Local<v8::Private>::New(iso, Data::extName);
    if (!obj->HasPrivate(ctx, extName).ToChecked()) {
        Nan::ThrowError("Argument must have an external");
        return;
    }
    v8::Local<v8::Value> extValue = obj->GetPrivate(ctx, extName).ToLocalChecked();
    Data* data = static_cast<Data*>(v8::Local<v8::External>::Cast(extValue)->Value());
    data->close();
}

NAN_METHOD(Feed) {
    if (!info[0]->IsObject()) {
        Nan::ThrowError("Argument must be an object");
        return;
    }

    auto iso = info.GetIsolate();
    auto ctx = Nan::GetCurrentContext();
    v8::Local<v8::Object> obj = v8::Local<v8::Object>::Cast(info[0]);
    v8::Local<v8::Private> extName = v8::Local<v8::Private>::New(iso, Data::extName);
    if (!obj->HasPrivate(ctx, extName).ToChecked()) {
        Nan::ThrowError("Argument must have an external");
        return;
    }
    v8::Local<v8::Value> extValue = obj->GetPrivate(ctx, extName).ToLocalChecked();
    Data* data = static_cast<Data*>(v8::Local<v8::External>::Cast(extValue)->Value());
    if (!data->decoder) {
        Nan::ThrowError("Decoder not open");
        return;
    }

    if (!node::Buffer::HasInstance(info[1])) {
        Nan::ThrowError("Feed needs a Buffer argument");
        return;
    }

    const char* dt = node::Buffer::Data(info[1]);
    const size_t size = node::Buffer::Length(info[1]);

    if (!size)
        return;

    // printf("feeding %zu\n", size);

    uv_mutex_lock(&data->mutex);
    data->inbuffers.push_back(Data::BufferData{ 0, std::string(dt, size) });
    uv_cond_signal(&data->cond);
    uv_mutex_unlock(&data->mutex);

    // printf("fed\n");
}

NAN_MODULE_INIT(Initialize) {
    NAN_EXPORT(target, Open);
    NAN_EXPORT(target, Feed);
    NAN_EXPORT(target, Close);
}

NODE_MODULE(flac, Initialize)
