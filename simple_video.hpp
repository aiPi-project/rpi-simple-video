// Simple video - a simple video capture program example 

#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <variant>
#include <iostream>

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libcamera/base/span.h>
#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/control_ids.h>
#include <libcamera/controls.h>
#include <libcamera/formats.h>
#include <libcamera/libcamera.h>

#include "h264_encoder.hpp"

using namespace libcamera;
using MappedBuffers = std::map<FrameBuffer*, std::vector<Span<uint8_t>>>;

struct CompletedRequest
{
    using BufferMap = libcamera::Request::BufferMap;
    using Request = libcamera::Request;

    CompletedRequest(unsigned int seq, Request *r) 
        : sequence(seq), buffers(r->buffers()), request(r)
    {
        r->reuse();
    }

    unsigned int sequence;
    BufferMap buffers;
    Request *request;
};

typedef std::shared_ptr<CompletedRequest>  CompletedRequestPtr;
typedef std::variant<CompletedRequestPtr>  MsgPayload;

enum class MsgType
{
    RequestComplete,
    Timeout,
    Quit
};

struct Msg
{
    Msg(MsgType const &t): type(t) {}
    template <typename T>
    Msg(MsgType const &t, T p): type(t), payload(std::forward<T>(p))
    { }
    MsgType  type;
    MsgPayload payload;
};

class SimpleVideo
{
public:
    bool OpenCamera();
    void CloseCamera();

    bool StartCamera();
    void StopCamera();

    Stream* GetVideoStream() {
        return streams_["video"];
    }
    StreamInfo GetStreamInfo(Stream const *stream) {
        StreamConfiguration const &cfg = stream->configuration();
        StreamInfo info;
        info.width = cfg.size.width;
        info.height = cfg.size.height;
        info.stride = cfg.stride;
        info.pixel_format = cfg.pixelFormat;
        info.colour_space = cfg.colorSpace;
        
        return info;
    }

    template <class T>
    bool CreateEncoder() {
        StreamInfo info;

        auto it = streams_.find("video");
        Stream const *stream = it->second;
        StreamConfiguration const &cfg = stream->configuration();
        
        info.width = cfg.size.width;
        info.height = cfg.size.height;
        info.stride = cfg.stride;
        info.pixel_format = cfg.pixelFormat;
        info.colour_space = cfg.colorSpace;
        
        encoder_ = std::make_unique<T>(info);
        return true;
    };
    Encoder* GetEncoder() {
        return encoder_.get();
    }
    
    bool EncodeBuffer(CompletedRequestPtr &completed_request, Stream *stream);

    template <typename T>
    class MessageQueue
    {
    public:
        template<typename U>
        void Post(U &&msg) {
            std::unique_lock<std::mutex> lock(mutex_);
            queue_.push(std::forward<U>(msg));
            cond_.notify_one();
        }
        T Wait() {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this] { return !queue_.empty(); });
            T msg = std::move(queue_.front());
            queue_.pop();
            return msg;
        }
        void clear() {
            std::unique_lock<std::mutex> lock(mutex_);
            queue_ = {};
        }

    private:
        std::queue<T>  queue_;
        std::mutex  mutex_;
        std::condition_variable  cond_;
    };

    bool ConfigureVideo();
    Msg WaitMsg() {
        return msg_queue_.Wait();
    }
    void PostMessage(MsgType t, MsgPayload p);

    int Sequence() const { return sequence; }
    bool QueueRequest(CompletedRequestPtr completed_request);

protected:
    void requestCompleted(Request *request);
    
    void ConfigureDenoise(const::std::string& denoise_mode);
    
    bool SetupCapture();
    bool MakeRequest();

private:
    std::unique_ptr<CameraManager> camera_manager_;
    std::shared_ptr<Camera> camera_;
    ControlList controls_;
    std::unique_ptr<CameraConfiguration> configuration_;
    std::unique_ptr<Encoder> encoder_;
    std::map<std::string, Stream*> streams_;

    std::map<Stream*, std::vector<std::unique_ptr<FrameBuffer>>> frame_buffers_;
    std::map<FrameBuffer*, std::vector<libcamera::Span<uint8_t>>> mapped_buffers_;
    std::vector<std::unique_ptr<Request>> requests_;

    MessageQueue<Msg> msg_queue_;
    int sequence;

    libcamera::UniqueFD DmaHeapAlloc(const char* name, std::size_t size);
};

class BufferReadSync
{
public:
    BufferReadSync(const MappedBuffers&  mapped_buffers, FrameBuffer* fb)
    {
        auto it = mapped_buffers.find(fb);
        if(it == mapped_buffers.end()) {
            std::cout << "failed to find buffer in frame buffers" << std::endl;
            return;
        }
        planes_ = it->second;
    }
    ~BufferReadSync() {}

    const std::vector<Span<uint8_t>>& Get() const {
        return planes_;
    }

private:
    std::vector<Span<uint8_t>> planes_;
};



