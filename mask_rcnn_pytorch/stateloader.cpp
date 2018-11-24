#include "stateloader.h"

#include <rapidjson/error/en.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/reader.h>

#include <iostream>
#include <stack>

namespace {

enum class ReadState {
  None,
  DictObject,
  ParamName,
  SizeTensorPair,
  TensorSize,
  SizeTensorPairDelim,
  TensorValue,
  List
};

struct DictHandler
    : public rapidjson::BaseReaderHandler<rapidjson::UTF8<>, DictHandler> {
  DictHandler() {}

  bool Double(double d) {
    std::cout << "double " << d << std::endl;
    if (current_state_.top() == ReadState::List ||
        current_state_.top() == ReadState::TensorValue) {
      blob_.push_back(static_cast<float>(d));
      ++index_;
    } else {
      throw std::logic_error("Double parsing error");
    }
    return true;
  }

  bool Uint(unsigned u) {
    std::cout << "uint " << u << std::endl;
    if (current_state_.top() == ReadState::List ||
        current_state_.top() == ReadState::TensorValue) {
      blob_.push_back(static_cast<float>(u));
      ++index_;
    } else if (current_state_.top() == ReadState::TensorSize) {
      size_.push_back(static_cast<int64_t>(u));
    } else {
      throw std::logic_error("UInt parsing error");
    }
    return true;
  }

  bool Key(const char* str, rapidjson::SizeType length, bool /*copy*/) {
    key_.assign(str, length);
    std::cout << "key " << key_ << std::endl;
    if (current_state_.top() == ReadState::DictObject) {
      current_state_.push(ReadState::ParamName);
    } else {
      throw std::logic_error("Key parsing error");
    }
    return true;
  }

  bool StartObject() {
    std::cout << "start object" << std::endl;
    if (current_state_.top() == ReadState::None) {
      current_state_.pop();
      current_state_.push(ReadState::DictObject);
    } else {
      throw std::logic_error("Start object parsing error");
    }
    return true;
  }

  bool EndObject(rapidjson::SizeType /*memberCount*/) {
    std::cout << "end object" << std::endl;
    if (current_state_.top() != ReadState::DictObject) {
      throw std::logic_error("End object parsing error");
    }
    return true;
  }

  void StartData() {
    current_state_.push(ReadState::TensorValue);
    auto total_length = std::accumulate(size_.begin(), size_.end(), 1,
                                        std::multiplies<int64_t>());
    blob_.resize(static_cast<size_t>(total_length));
    blob_.clear();
    index_ = 0;
  }

  bool StartArray() {
    std::cout << "start array" << std::endl;
    if (current_state_.top() == ReadState::List) {
      current_state_.push(ReadState::List);
    } else if (current_state_.top() == ReadState::ParamName) {
      current_state_.push(ReadState::SizeTensorPair);
    } else if (current_state_.top() == ReadState::SizeTensorPair) {
      current_state_.push(ReadState::TensorSize);
      size_.clear();
    } else if (current_state_.top() == ReadState::SizeTensorPairDelim) {
      current_state_.pop();
      StartData();
    } else if (current_state_.top() == ReadState::TensorValue) {
      current_state_.push(ReadState::List);
    } else {
      throw std::logic_error("Start array parsing error");
    }
    return true;
  }

  bool EndArray(rapidjson::SizeType elementCount) {
    std::cout << "end array" << std::endl;
    if (current_state_.top() == ReadState::List) {
      current_state_.pop();
    } else if (current_state_.top() == ReadState::SizeTensorPair) {
      current_state_.pop();
      assert(current_state_.top() == ReadState::ParamName);
      current_state_.pop();
      std::cout << "Add new param" << std::endl;
      dict.insert(key_, tensor_);
    } else if (current_state_.top() == ReadState::TensorSize) {
      current_state_.pop();
      if (elementCount == 0) {
        size_.push_back(1);
        StartData();
      } else {
        current_state_.push(ReadState::SizeTensorPairDelim);
      }
    } else if (current_state_.top() == ReadState::TensorValue) {
      current_state_.pop();
      assert(index_ == static_cast<int64_t>(blob_.size()));
      at::Tensor tensor_image = torch::from_blob(
          blob_.data(), at::IntList(size_), at::CPU(at::kFloat));
      if (blob_.size() == 1) {
        assert(current_state_.top() == ReadState::SizeTensorPair);
        current_state_.pop();
        assert(current_state_.top() == ReadState::ParamName);
        current_state_.pop();
        std::cout << "Add new param" << std::endl;
        dict.insert(key_, tensor_);
      }
    } else {
      throw std::logic_error("End array parsing error");
    }
    return true;
  }

  std::string key_;
  std::vector<int64_t> size_;
  torch::Tensor tensor_;
  std::vector<float> blob_;
  int64_t index_{0};

  std::stack<ReadState> current_state_{{ReadState::None}};

  torch::OrderedDict<std::string, torch::Tensor> dict;
};
}  // namespace

torch::OrderedDict<std::string, torch::Tensor> LoadStateDict(
    const std::string& file_name) {
  auto* file = std::fopen(file_name.c_str(), "r");
  if (file) {
    char readBuffer[65536];
    rapidjson::FileReadStream is(file, readBuffer, sizeof(readBuffer));
    rapidjson::Reader reader;
    DictHandler handler;
    auto res = reader.Parse(is, handler);
    std::fclose(file);

    if (!res) {
      throw std::runtime_error(rapidjson::GetParseError_En(res.Code()));
    }

    return handler.dict;
  }
  return torch::OrderedDict<std::string, torch::Tensor>();
}
