#include "image_processor.h"
#include "logger.h"

CLIPImageProcessor::CLIPImageProcessor(const std::string& model_path, const std::string& processor_filename) {
    Ort::SessionOptions session_options;
    session_options.EnableOrtCustomOps();
    auto processor_path = model_path + "/" + processor_filename;
    TS_LOG(INFO) << "Loading image processor from " << processor_path;
    session_ = std::make_unique<Ort::Session>(env_, processor_path.c_str(), session_options);
}


Option<processed_image_t> CLIPImageProcessor::process_image(const std::string& image_encoded) {
    std::unique_lock<std::mutex> lock(mutex_);

    // Decode image
    auto image = StringUtils::base64_decode(image_encoded);

    // convert string to byte array
    std::vector<uint8_t> image_bytes(image.begin(), image.end());


    // Create input tensor
    int64_t input_tensor_size = image_bytes.size();
    std::vector<int64_t> input_shape = {input_tensor_size};
    std::vector<const char*> input_names = {"image"};
    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto input_tensor = Ort::Value::CreateTensor<uint8_t>(memory_info, image_bytes.data(), image_bytes.size(), input_shape.data(), input_shape.size());

    
    // Create output tensor
    std::vector<const char*> output_names = {"last_hidden_state"};

    // Run inference
    std::vector<Ort::Value> output_tensors;
    // TS_LOG(INFO) << "Running image processor";
    try {
        output_tensors = session_->Run(Ort::RunOptions{nullptr}, input_names.data(), &input_tensor, 1, output_names.data(), output_names.size());
    } catch (...) {
        return Option<processed_image_t>(400, "Error while processing image");
    }

    // Get output tensor
    auto output_tensor = output_tensors.front().GetTensorMutableData<float>();

    // Convert output tensor to processed_image_t
    auto output_shape = output_tensors.front().GetTensorTypeAndShapeInfo().GetShape();
    if (output_shape.size() != 4) {
        TS_LOG(INFO) << "Output tensor shape is not 4D";
        return Option<processed_image_t>(400, "Error while processing image");
    }
    processed_image_t output;

    const size_t batch_size = static_cast<size_t>(output_shape[0]);
    const size_t channels = static_cast<size_t>(output_shape[1]);
    const size_t height = static_cast<size_t>(output_shape[2]);
    const size_t width = static_cast<size_t>(output_shape[3]);
    for (size_t i = 0; i < batch_size; i++) {
        for (size_t j = 0; j < channels; j++) {
            for (size_t k = 0; k < height; k++) {
                for (size_t l = 0; l < width; l++) {
                    output.push_back(output_tensor[i * channels * height * width + j * height * width + k * width + l]);
                }
            }
        }
    }

    // TS_LOG(INFO) << "Image processed";

    return Option<processed_image_t>(std::move(output));
}
