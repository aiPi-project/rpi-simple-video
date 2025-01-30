#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

#include <jpeglib.h>
#include <libcamera/libcamera.h>
#include <sys/mman.h>

using namespace libcamera;
using namespace std::chrono_literals;

static std::shared_ptr<Camera> camera;

static void requestComplete(Request *request) {
    if (request->status() == Request::RequestCancelled) return;

    // request completed successfully, app can access the
    // completed buffers now
    const ControlList &requestMetadata = request->metadata();
    const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();
    for (auto bufferPair : buffers) {
        FrameBuffer *buffer = bufferPair.second;
        const FrameMetadata &metadata = buffer->metadata();
        // print information about the completed buffer
        std::cout << " seq: " << std::setw(6) << std::setfill('0')
                  << metadata.sequence << " timestamp: " << metadata.timestamp
                  << " byteused: ";

        unsigned int nplane = 0;
        for (const FrameMetadata::Plane &plane : metadata.planes()) {
            std::cout << plane.bytesused;
            if (++nplane < metadata.planes().size()) std::cout << "/";
        }
        std::cout << std::endl;
        // image data can be accessed here, but the FrameBuffer
        // must be mapped by the application
    }
    // re-queue the Request to the camera
    request->reuse(Request::ReuseBuffers);
    camera->queueRequest(request);
}

// Function to save raw RGB data as JPEG using libjpeg
bool saveAsJPEG(const std::string &fileName, uint8_t *data, int width,
                int height, int stride) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *outfile;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    if ((outfile = fopen(fileName.c_str(), "wb")) == nullptr) {
        std::cerr << "Error opening output JPEG file" << std::endl;
        return false;
    }

    jpeg_stdio_dest(&cinfo, outfile);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;  // RGB has 3 components
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    // Write the RGB data line by line
    std::vector<uint8_t> row_rgb(width * 3);
    JSAMPROW row_pointer;
    while (cinfo.next_scanline < cinfo.image_height) {
        uint8_t *row_bgr = &data[cinfo.next_scanline * stride];
        // convert each pixel from BGR to RGB
        for (int x = 0; x < width; ++x) {
            row_rgb[x * 3 + 0] = row_bgr[x * 3 + 2];  // Red
            row_rgb[x * 3 + 1] = row_bgr[x * 3 + 1];  // Green
            row_rgb[x * 3 + 2] = row_bgr[x * 3 + 0];  // Green
        }
        row_pointer = row_rgb.data();
        // row_pointer = (JSAMPROW)&data[cinfo.next_scanline * stride];
        jpeg_write_scanlines(&cinfo, &row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    fclose(outfile);
    jpeg_destroy_compress(&cinfo);

    return true;
}

int simply_capture(std::shared_ptr<Camera> camera) {
    // acquire an exclusive lock of the camera
    camera->acquire();
    // create a new configuration
    std::unique_ptr<CameraConfiguration> config =
        camera->generateConfiguration({StreamRole::StillCapture});
    // log the first stream configuration item
    StreamConfiguration &streamConfig = config->at(0);
    std::cout << "Default Viewfinder configuration is: "
              << streamConfig.toString() << std::endl;

    // customize stream configuration
    config->at(0).pixelFormat = formats::RGB888;
    config->at(0).size.width = 1296;
    config->at(0).size.height = 972;

    // if any parameter changed, we must validate the config
    config->validate();

    // update the camera configuration
    camera->configure(config.get());

    // create a FrameBufferAllocator for the camera and use it to allocate
    // buffers for streams of a CameraConfiguration with the allocate() function
    FrameBufferAllocator *allocator = new FrameBufferAllocator(camera);

    for (StreamConfiguration &cfg : *config) {
        int ret = allocator->allocate(cfg.stream());
        if (ret < 0) {
            std::cerr << "Can't allocate buffers" << std::endl;
            return -1;
        }
        size_t allocated = allocator->buffers(cfg.stream()).size();
    }

    // Frame capture
    Stream *stream = streamConfig.stream();
    const std::vector<std::unique_ptr<FrameBuffer>> &buffers =
        allocator->buffers(stream);
    std::vector<std::unique_ptr<Request>> requests;
    for (size_t i = 0; i < buffers.size(); i++) {
        std::unique_ptr<Request> request = camera->createRequest();
        if (!request) {
            std::cerr << "Can't create request" << std::endl;
            return -1;
        }
        const std::unique_ptr<FrameBuffer> &buffer = buffers[i];
        int rc = request->addBuffer(stream, buffer.get());
        if (rc < 0) {
            std::cerr << "Can't set buffer for request" << std::endl;
            return -1;
        }

        requests.push_back(std::move(request));
    }

    camera->requestCompleted.connect(requestComplete);

    camera->start();
    for (std::unique_ptr<Request> &request : requests) {
        camera->queueRequest(request.get());
    }

    std::this_thread::sleep_for(3000ms);
    // Save image to file
    const FrameBuffer::Plane &plane = buffers[0]->planes()[0];
    void *memory = mmap(NULL, plane.length, PROT_READ, MAP_SHARED,
                        plane.fd.get(), plane.offset);
    if (memory == MAP_FAILED) {
        std::cerr << "Failed to map memory" << std::endl;
    }

    // find stride
    int plane_stride = stream->configuration().stride;
    std::cout << "save jpeg file, width = " << config->at(0).size.width
              << " height = " << config->at(0).size.height
              << " stride = " << plane_stride << std::endl;
    if (!saveAsJPEG("capture.jpg", static_cast<uint8_t *>(memory),
                    config->at(0).size.width, config->at(0).size.height,
                    plane_stride)) {
        std::cerr << "Failed to save as jpeg file" << std::endl;
    }

    camera->stop();
    allocator->free(stream);
    delete allocator;
    camera->release();

    return 0;
}

std::string cameraName(Camera *camera) {
    const ControlList &props = camera->properties();
    std::string name;

    const auto &location = props.get(properties::Location);
    if (location) {
        switch (*location) {
            case properties::CameraLocationFront:
                name = "Internal front camera";
                break;
            case properties::CameraLocationBack:
                name = "Internal back camera";
                break;
            case properties::CameraLocationExternal:
                name = "External camera";
                const auto &model = props.get(properties::Model);
                if (model) name = "'" + *model + "'";
                break;
        }
    }
    name += " {" + camera->id() + "}";
    return name;
}

int main() {
    std::unique_ptr<CameraManager> cm = std::make_unique<CameraManager>();
    cm->start();
    if (cm->cameras().empty()) {
        std::cout << "No camera available in the system, exit" << std::endl;
        return 0;
    }

    // print out the cameras available
    for (auto const &camera : cm->cameras()) {
        std::cout << " - " << cameraName(camera.get()) << std::endl;
    }

    auto cameraId = cm->cameras()[0]->id();
    camera = cm->get(cameraId);

    // call capture function to capture a picture
    simply_capture(camera);

    camera.reset();
    cm->stop();

    return 0;
}
