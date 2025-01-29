// SimpleVideo class implementation

#include <map>
#include <memory>
#include <vector>
#include <thread>
#include <chrono>

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

#include "h264_encoder.hpp"
#include "simple_video.hpp"
#include "file_output.hpp"
//#include "null_encoder.hpp"

using namespace libcamera;
using namespace std::placeholders;

using StreamRoles = std::vector<StreamRole>;

const std::vector<const char*> heapNames
{
    "/dev/dma_heap/vidbuf_cached",
    "/dev/dma_heap/linux,cma"
};

bool SimpleVideo::OpenCamera()
{
    // todo: log call here
    if (!camera_manager_) {
      camera_manager_ = std::make_unique<CameraManager>();
      if(0 != camera_manager_->start()) {
          std::cout << "camera manager failed to start!" << std::endl;
          camera_manager_.reset();
          return false;
      }
    }

    // get cameras in the system
    auto cameras = camera_manager_->cameras();
    // sort camera in the sequence ofe
    std::sort(cameras.begin(), cameras.end(), [](auto l, auto r) { return l->id() > r->id(); });
    if(cameras.size() == 0) {
        // todo: log no camera available
        return false;
    }

    std::string const& cam_id = cameras[0]->id();
    camera_ = camera_manager_->get(cam_id);
    if(!camera_) {
        // log error - todo use logger
        std::cout << "failed to find camera " + cam_id << std::endl;
        return false;
    }
    
    // acquire camera now
    if(0 != camera_->acquire()) {
        // todo: logger/exception
        std::cout << "failed to acquire camera " + cam_id << std::endl;
        return false;
    }
    
    // todo: set post processor call back
    // todo: if framerate set by user, set framerate field
 
    return true;
}

void SimpleVideo::CloseCamera()
{
    camera_->release();
    camera_.reset();
    camera_manager_.reset();
}

bool SimpleVideo::StartCamera()
{
    MakeRequest();

    auto default_crop = camera_->controls().at(&controls::ScalerCrop).def().get<Rectangle>();
	std::vector<Rectangle> crops;
    crops.push_back(default_crop);

    std::cout << "Using crop (main) " << crops.back().toString() << std::endl;
    // vc4 assumed to enabled
    controls_.set(controls::ScalerCrop, crops[0]);

    if(camera_->start(&controls_)) {
        std::cout << "failed to start camera!" << std::endl;
        return false;
    }

    // connect complete callback
    camera_->requestCompleted.connect(this, &SimpleVideo::requestCompleted);

    // queue requests
    for(std::unique_ptr<Request>& request: requests_) {
        if(camera_->queueRequest(request.get()) < 0) {
            std::cout << "failed to queue request" << std::endl;
            return false;
        }
    }

    return true;
}

void SimpleVideo::StopCamera()
{
    camera_->stop();
    camera_->requestCompleted.disconnect(this, &SimpleVideo::requestCompleted);

    requests_.clear();
    controls_.clear();
}

void SimpleVideo::PostMessage(MsgType t, MsgPayload p)
{
    msg_queue_.Post(Msg(t, p));
}

void SimpleVideo::requestCompleted(Request *request)
{
    if(request->status() == Request::RequestCancelled) {
        return;
    }

    struct dma_buf_sync dma_sync{};
    dma_sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    std::cout << "buffer size: " << request->buffers().size() << std::endl;;
    for(auto const& buffer_map : request->buffers()) {
        auto it = mapped_buffers_.find(buffer_map.second);
        if(it == mapped_buffers_.end()) {
            std::cout << "failed to identify request complete buffer" << std::endl;
            return;
        }
        int ret = ::ioctl(buffer_map.second->planes()[0].fd.get(), DMA_BUF_IOCTL_SYNC, &dma_sync);
        if(ret) {
            std::cout << "failed to sync dma buf on request complete" << std::endl;
            return;
        }
        std::cout << "buffer stream: " << buffer_map.first->configuration().toString() << std::endl;
    }
    // signal completed?
    std::cout << "request completed!!!" << std::endl;

    CompletedRequestPtr payload = std::make_shared<CompletedRequest>(sequence++, request);
    PostMessage(MsgType::RequestComplete, payload); 
}

bool SimpleVideo::QueueRequest(CompletedRequestPtr completed_request)
{
    Request::BufferMap buffers(std::move(completed_request->buffers));

    Request *request = completed_request->request;

    for(auto const &p: buffers) {
        struct dma_buf_sync  dma_sync{};
        dma_sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;

        auto it = mapped_buffers_.find(p.second);
        if(it == mapped_buffers_.end()) {
            std::cout << "failed to identify queue request buffer" << std::endl;
            return false;
        }
        
        int ret = ::ioctl(p.second->planes()[0].fd.get(), DMA_BUF_IOCTL_SYNC, &dma_sync);
        if(ret) {
            std::cout << "failed to sync dma buf on queue request" << std::endl;
            return false;
        }

        // add buffer
        if(request->addBuffer(p.first, p.second) < 0) {
            std::cout << "failed to add buffer to request in QueueRequest" << std::endl;
            return false;
        }
    }

    if(camera_->queueRequest(request) < 0) {
        std::cout << "failed to queue message to camera!" << std::endl;
        return false;
    }
    return true;
}

void SimpleVideo::ConfigureDenoise(const::std::string& denoise_mode)
{
    using namespace libcamera::controls::draft;
    
    static const std::map<std::string, NoiseReductionModeEnum> denoise_table = {
        { "off", NoiseReductionModeOff },
        { "cdn_off", NoiseReductionModeMinimal },
        { "cdn_fast", NoiseReductionModeFast },
        { "cdn_hq", NoiseReductionModeHighQuality }
    };
    NoiseReductionModeEnum denoise;

    auto const mode = denoise_table.find(denoise_mode);
    if(mode != denoise_table.end()) {
        denoise = mode->second;
        controls_.set(NoiseReductionMode, denoise);
    }
    // log error for else?
}

bool SimpleVideo::ConfigureVideo()
{
    // todo: log call
    StreamRoles stream_roles = { StreamRole::VideoRecording };

    // assume to support lores stream
    // stream_roles.push_back(StreamRoles::Viewfinder);
    configuration_ = camera_->generateConfiguration(stream_roles);
    if(!configuration_) {
        std::cout << "failed to generate camera configuration" << std::endl;
        return false;
    }

    // now override default configurations
    StreamConfiguration &cfg = configuration_->at(0);
    cfg.pixelFormat = libcamera::formats::YUV420;
    cfg.bufferCount = 6; // better than default 4 buffers
    cfg.size.width = 640;
    cfg.size.height = 480;
    // todo: handle user's options

    // configure denoise, assume 'auto' now, todo: user provide option
    ConfigureDenoise("auto");
    // setup capture now
    if(!SetupCapture()) {
        std::cout << "failed to setup capture" << std::endl;
        return false;
    }

    streams_["video"] = configuration_->at(0).stream();
    std::cout << "Video setup complete" << std::endl;

    return true; 
}

libcamera::UniqueFD SimpleVideo::DmaHeapAlloc(const char* name, std::size_t size)
{
    libcamera::UniqueFD dmaHeapHandle;

    if(!name) return {};

    // find the dma heap handler
    for(const char* name : heapNames) {
        int ret = ::open(name, O_RDWR | O_CLOEXEC, 0);
        if(ret < 0) {
            std::cout << "failed to open " << name << ": " << ret << std::endl;
            continue;
        }
        dmaHeapHandle  = libcamera::UniqueFD(ret);
        break;
    }
    if(!dmaHeapHandle.isValid()) {
        std::cout << "could not open any dma heap devie" << std::endl;
        return {};
    }

    struct dma_heap_allocation_data alloc = {};

    alloc.len = size;
    alloc.fd_flags = O_CLOEXEC | O_RDWR;

    int ret = ::ioctl(dmaHeapHandle.get(), DMA_HEAP_IOCTL_ALLOC, &alloc);
    if(ret < 0) {
        std::cout << "dmaHeap allocation failed for " << name << std::endl;
        return {};
    }

    libcamera::UniqueFD allocFd(alloc.fd);
    ret = ::ioctl(allocFd.get(), DMA_BUF_SET_NAME, name);
    if(ret < 0) {
        std::cout << "dmaHeap naming failed for " << name << std::endl;
        return {};
    }
    
    return allocFd;
}

// setup capture stream configuration
bool SimpleVideo::SetupCapture()
{
    for(auto& config: *configuration_) {
        config.stride = 0;
    }
    auto validation = configuration_->validate();
    if(CameraConfiguration::Invalid == validation) {
        std::cout << "failed to validate stream configuration" << std::endl;
        return false;
    }

    if(camera_->configure(configuration_.get()) < 0) {
        std::cout << "failed to configure streams!" << std::endl;
        return false;
    }

    // log available controls
    for(auto const &[id, info]: camera_->controls()) {
        std::cout << id->name() << " : " << info.toString() << std::endl;
    }

    // allocate all the buffers, mmap and store them on a free list
    for(StreamConfiguration& config : *configuration_) {
        Stream *stream = config.stream();
        std::vector<std::unique_ptr<FrameBuffer>> frame_bufs;

        // allocate memory from dam and map
        for(size_t i = 0; i < config.bufferCount; i++) {
            std::string name("aiPi-cam" + std::to_string(i));
            libcamera::UniqueFD fd = DmaHeapAlloc(name.c_str(), config.frameSize);
            if(!fd.isValid()) {
                std::cout << "failed to allocate capture buffer for stream" << std::endl;
                return false;
            }

            std::vector<FrameBuffer::Plane> plane(1);
            plane[0].fd = libcamera::SharedFD(std::move(fd));
            plane[0].offset = 0;
            plane[0].length = config.frameSize;

            frame_bufs.push_back(std::make_unique<FrameBuffer>(plane));

            // memory map the dma heap
            void *memory = mmap(NULL, config.frameSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                                plane[0].fd.get(), 0);
            mapped_buffers_[frame_bufs.back().get()].push_back(
                    libcamera::Span<uint8_t>(static_cast<uint8_t*>(memory), config.frameSize));
        }
        frame_buffers_[stream] = std::move(frame_bufs);
    }
    // todo: start preview
    return true;
}

bool SimpleVideo::MakeRequest()
{
    std::map<Stream *, std::queue<FrameBuffer *>> free_buffers;

    for(auto &kv: frame_buffers_) {
        free_buffers[kv.first] = {};
        for(auto &b: kv.second) {
            free_buffers[kv.first].push(b.get());
        }
    }

    // todo: rework this loop
    while(true) {
		for (StreamConfiguration &config : *configuration_)
		{
			Stream *stream = config.stream();
			if (stream == configuration_->at(0).stream())
			{
				if (free_buffers[stream].empty())
				{
					std::cout << "Requests created" << std::endl;
					return true;
				}
				std::unique_ptr<Request> request = camera_->createRequest();
				if (!request) {
					std::cout << "failed to make request" << std::endl;
                    return false;
                }
				requests_.push_back(std::move(request));
			}
			else if (free_buffers[stream].empty()) {
				std::cout << "concurrent streams need matching numbers of buffers" << std::endl;
                return false;
            }
			FrameBuffer *buffer = free_buffers[stream].front();
			free_buffers[stream].pop();
			if (requests_.back()->addBuffer(stream, buffer) < 0) {
				std::cout << "failed to add buffer to request" << std::endl;
                return false;
            }
		}

    }
    return false;
}

bool SimpleVideo::EncodeBuffer(CompletedRequestPtr &completed_request, Stream *stream)
{
    StreamInfo info = GetStreamInfo(stream);
    FrameBuffer *buffer = completed_request->buffers[stream];
    BufferReadSync r(mapped_buffers_, buffer);
    libcamera::Span span = r.Get()[0];
    void *mem = span.data();
    if(!mem) {
        std::cout << "no buffer to process" << std::endl;
        return false;
    }
    int64_t timestamp_ns = buffer->metadata().timestamp;
    encoder_->EncodeBuffer(buffer->planes()[0].fd.get(), span.size(), mem, info, timestamp_ns / 1000); 
    return true;
}

# if 0
void ProcessBuffer(CompletedRequestPtr &completed_request, Stream *stream)
{
    StreamInfo info = GetStreamInfo(stream);
    if(completed_request->buffers.find(stream) == completed_request->buffers.end()) {
        std::cout << "can not find stream buffer!! " << completed_request->buffers.size()
            << std::endl;
        return;
    }
    FrameBuffer *buffer = completed_request->buffers[stream];
    BufferReadSync r(mapped_buffers_, buffer);
    libcamera::Span span = r.Get()[0];
    void *mem = span.data();
    if(!mem) {
        std::cout << "no buffer to process" << std::endl;
        return;
    }
    if(span.size() > 0) {
        fwrite(mem, span.size(), 1, fp_);
    }
    std::cout << "buffer mem size: " << span.size() << std::endl;
    // save the buffer to file
}
#endif
