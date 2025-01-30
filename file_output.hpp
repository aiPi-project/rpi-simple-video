#include <libcamera/libcamera.h>

#include <iostream>
#include <string>

class FileOutput {
   public:
    FileOutput(const std::string &filename)
        : fp_(nullptr), filename_(filename) {}
    ~FileOutput() { closeFile(); }

    void OutputReady(void *mem, size_t size, int64_t timestamp_us,
                     bool keyframe) {
        uint32_t flags = keyframe ? FLAG_KEYFRAME : FLAG_NONE;
        outputBuffer(mem, size, timestamp_us, flags);
    }

    void MetadataReady(libcamera::ControlList &metadata){/* do nothing */};

   private:
    enum Flag { FLAG_NONE = 0, FLAG_KEYFRAME = 1, FLAG_RESTART = 2 };

    void openFile() {
        fp_ = fopen(filename_.c_str(), "w");
        if (!fp_) {
            std::cout << "failed to open file for writing" << std::endl;
        }
    }

    void closeFile() {
        if (fp_) {
            fflush(fp_);
            fclose(fp_);
            fp_ = nullptr;
        }
    }

    void outputBuffer(void *mem, size_t size, int64_t timestamp_us,
                      uint32_t flags) {
        if (nullptr == fp_) {
            openFile();
        }

        if (fp_ && size > 0) {
            if (fwrite(mem, size, 1, fp_) != 1) {
                std::cout << "failed to write output" << std::endl;
                return;
            }
        }
    }

    FILE *fp_;
    const std::string filename_;
};
