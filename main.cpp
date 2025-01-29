// SimpleVideo main function

#include "simple_video.hpp"
#include "file_output.hpp"

using namespace std::placeholders;

int main()
{
    //FileOutput file_output("video_output.raw");
    //NullEncoder encoder;
    SimpleVideo app;

    if(!app.OpenCamera()) {
        std::cout << "Open camera failed!" << std::endl;
        return 1;
    }

    if(!app.ConfigureVideo()) {
        std::cout << "Configure video failed!" << std::endl;
        return 1;
    }

    FileOutput  output("video_output.h264");
    /* fp_ = fopen("video_output.raw", "w");
    if(!fp_) {
        std::cout << "failed to create video output file!" << std::endl;
    } */

    if(!app.CreateEncoder<H264Encoder>()) {
        std::cout << "failed to create h264 encoder!" << std::endl;
        return 0;
    }
    app.GetEncoder()->SetOutputReadyCallback(std::bind(&FileOutput::OutputReady, output, _1, _2, _3, _4));
    std::function<void(void*)> f = [](void*) {};
    app.GetEncoder()->SetInputDoneCallback(f);

    app.StartCamera();

    for(std::size_t i = 0; i < 150; i++) {
        //auto msg = msg_queue_.Wait();
        auto msg = app.WaitMsg();
        std::cout << "request " << i << " completed" << std::endl;

        //ProcessBuffer(std::get<CompletedRequestPtr>(msg.payload), streams_["video"]);
        app.EncodeBuffer(std::get<CompletedRequestPtr>(msg.payload), app.GetVideoStream());

        if(!app.QueueRequest(std::get<CompletedRequestPtr>(msg.payload))) {
            std::cout << "failed to requeue request!!" << std::endl;
            break;
        }
    }

    //std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    app.StopCamera();
    //if(fp_) {
    //    fclose(fp_);
    //}

    app.CloseCamera();
    std::cout << "Successfully open camera and configure video!" << std::endl;
    return 0;
}
