// Copyright 2022 Xilinx, Inc.
// Copyright 2022 Advanced Micro Devices, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file
 * @brief Implements the gRPC server
 */

#include "amdinfer/servers/grpc_server.hpp"

#include <google/protobuf/repeated_ptr_field.h>  // for RepeatedPtrField
#include <grpc/support/log.h>                    // for GPR_ASSERT, GPR_UNL...
#include <grpcpp/grpcpp.h>                       // for ServerCompletionQueue

#include <cassert>        // for assert
#include <cstddef>        // for size_t, byte
#include <cstdint>        // for uint64_t, int16_t
#include <cstring>        // for memcpy
#include <exception>      // for exception
#include <memory>         // for unique_ptr, shared_ptr
#include <string>         // for allocator, string
#include <thread>         // for thread, yield
#include <unordered_set>  // for unordered_set
#include <utility>        // for move
#include <vector>         // for vector

#include "amdinfer/buffers/buffer.hpp"           // for Buffer
#include "amdinfer/build_options.hpp"            // for AMDINFER_ENABLE_LOG...
#include "amdinfer/clients/grpc_internal.hpp"    // for mapProtoToParameters
#include "amdinfer/core/data_types.hpp"          // for DataType, DataType:...
#include "amdinfer/core/exceptions.hpp"          // for invalid_argument
#include "amdinfer/core/inference_request.hpp"   // for InferenceRequest
#include "amdinfer/core/inference_response.hpp"  // for InferenceResponse
#include "amdinfer/core/parameters.hpp"          // for ParameterMap
#include "amdinfer/core/request_container.hpp"   // for RequestContainer
#include "amdinfer/core/shared_state.hpp"        // for SharedState
#include "amdinfer/declarations.hpp"             // for BufferRawPtrs, Infe...
#include "amdinfer/observation/observer.hpp"     // for Logger, Loggers
#include "amdinfer/util/containers.hpp"          // for containerProduct
#include "amdinfer/util/string.hpp"              // for toLower
#include "amdinfer/util/traits.hpp"              // IWYU pragma: keep
#include "inference.grpc.pb.h"                   // for GRPCInferenceServic...
#include "inference.pb.h"                        // for InferTensorContents

namespace amdinfer {
class CallDataModelInfer;
class CallDataModelMetadata;
class CallDataModelLoad;
class CallDataWorkerLoad;
class CallDataModelReady;
class CallDataModelUnload;
class CallDataWorkerUnload;
class CallDataServerLive;
class CallDataServerMetadata;
class CallDataServerReady;
class CallDataHasHardware;
class CallDataModelList;
}  // namespace amdinfer

// use aliases to prevent clashes between grpc:: and amdinfer::grpc::
using ServerBuilder = grpc::ServerBuilder;
using ServerCompletionQueue = grpc::ServerCompletionQueue;
template <typename T>
using ServerAsyncResponseWriter = grpc::ServerAsyncResponseWriter<T>;
using ServerContext = grpc::ServerContext;
using Server = grpc::Server;
using StatusCode = grpc::StatusCode;

// namespace inference {
// using StreamModelInferRequest = ModelInferRequest;
// using StreamModelInferResponse = ModelInferResponse;
// }

namespace amdinfer {

using AsyncService = inference::GRPCInferenceService::AsyncService;

class CallDataBase {
 public:
  virtual void proceed() = 0;
};

template <typename RequestType, typename ReplyType>
class CallData : public CallDataBase {
 public:
  // Take in the "service" instance (in this case representing an asynchronous
  // server) and the completion queue "cq" used for asynchronous communication
  // with the gRPC runtime.
  CallData(AsyncService* service, ::grpc::ServerCompletionQueue* cq)
    : service_(service), cq_(cq), status_(Create) {}

  virtual ~CallData() = default;

  void proceed() override {
    if (status_ == Create) {
      // Make this instance progress to the Process state.
      status_ = Process;

      waitForRequest();
    } else if (status_ == Process) {
      addNewCallData();

      // queue_->enqueue(this);
      // status_ = Wait;
      handleRequest();
      status_ = Wait;
    } else if (status_ == Wait) {
      std::this_thread::yield();
    } else {
      assert(status_ == Finish);
      // Once in the Finish state, deallocate ourselves (CallData).
      delete this;
    }
  }

  virtual void finish(const ::grpc::Status& status) = 0;

 protected:
  // When we handle a request of this type, we need to tell
  // the completion queue to wait for new requests of the same type.
  virtual void addNewCallData() = 0;

  virtual void waitForRequest() = 0;
  virtual void handleRequest() noexcept = 0;

  // The means of communication with the gRPC runtime for an asynchronous
  // server.
  AsyncService* service_;
  // The producer-consumer queue where for asynchronous server notifications.
  ::grpc::ServerCompletionQueue* cq_;
  // Context for the rpc, allowing to tweak aspects of it such as the use
  // of compression, authentication, as well as to send metadata back to the
  // client.
  ::grpc::ServerContext ctx_;

  // What we get from the client.
  RequestType request_;
  // What we send back to the client.
  ReplyType reply_;

  // Let's implement a tiny state machine with the following states.
  enum CallStatus { Create, Process, Wait, Finish };
  CallStatus status_;  // The current serving state.
};

template <typename RequestType, typename ReplyType>
class CallDataUnary : public CallData<RequestType, ReplyType> {
 public:
  // Take in the "service" instance (in this case representing an asynchronous
  // server) and the completion queue "cq" used for asynchronous communication
  // with the gRPC runtime.
  CallDataUnary(AsyncService* service, ::grpc::ServerCompletionQueue* cq)
    : CallData<RequestType, ReplyType>(service, cq), responder_(&this->ctx_) {}

  void finish(const ::grpc::Status& status) override {
    // And we are done! Let the gRPC runtime know we've finished, using the
    // memory address of this instance as the uniquely identifying tag for
    // the event.
    this->status_ = this->Finish;
    responder_.Finish(this->reply_, status, this);
  }

 protected:
  // The means to get back to the client.
  ::grpc::ServerAsyncResponseWriter<ReplyType> responder_;
};

template <typename RequestType, typename ReplyType>
class CallDataServerStream : public CallData<RequestType, ReplyType> {
 public:
  // Take in the "service" instance (in this case representing an asynchronous
  // server) and the completion queue "cq" used for asynchronous communication
  // with the gRPC runtime.
  CallDataServerStream(AsyncService* service, ::grpc::ServerCompletionQueue* cq)
    : CallData<RequestType, ReplyType>(service, cq), responder_(&this->ctx_) {}

  void write(const ReplyType& response) { responder_->Write(response, this); }

  void finish(const ::grpc::Status& status) override {
    // And we are done! Let the gRPC runtime know we've finished, using the
    // memory address of this instance as the uniquely identifying tag for
    // the event.
    this->status_ = this->Finish;
    responder_.Finish(this->reply_, status, this);
  }

 protected:
  // The means to get back to the client.
  ::grpc::ServerAsyncWriter<ReplyType> responder_;
};

struct WriteData {
  template <typename T, typename Tensor>
  void operator()(Buffer* buffer, Tensor* tensor, size_t offset, size_t size,
                  [[maybe_unused]] const Observer& observer) const {
    auto* contents = getTensorContents<T>(tensor);
    if constexpr (util::is_any_v<T, bool, uint32_t, uint64_t, int32_t, int64_t,
                                 float, double, char>) {
      auto* dest = static_cast<std::byte*>(buffer->data(offset));
      std::memcpy(dest, contents, size * sizeof(T));
    } else if constexpr (util::is_any_v<T, uint8_t, uint16_t, int8_t, int16_t,
                                        fp16>) {
      for (size_t i = 0; i < size; i++) {
#ifdef AMDINFER_ENABLE_LOGGING
        if (const auto min_size = size > kNumTraceData ? kNumTraceData : size;
            i < min_size) {
          AMDINFER_LOG_TRACE(observer.logger,
                             "Writing data to buffer: " +
                               std::to_string(static_cast<T>(contents[i])));
        }
#endif
        offset = buffer->write(static_cast<T>(contents[i]), offset);
      }
    } else {
      static_assert(!sizeof(T), "Invalid type to WriteData");
    }
  }
};

#ifdef AMDINFER_ENABLE_LOGGING
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CALLDATA_IMPL(endpoint, type)                                      \
  class CallData##endpoint                                                 \
    : public CallData##type<inference::endpoint##Request,                  \
                            inference::endpoint##Response> {               \
   public:                                                                 \
    CallData##endpoint(AsyncService* service, ServerCompletionQueue* cq,   \
                       SharedState* state)                                 \
      : CallData##type(service, cq), state_(state) {                       \
      proceed();                                                           \
    }                                                                      \
                                                                           \
   private:                                                                \
    Logger logger_{Loggers::Server};                                       \
    SharedState* state_;                                                   \
                                                                           \
   protected:                                                              \
    void addNewCallData() override {                                       \
      new CallData##endpoint(service_, cq_, state_);                       \
    }                                                                      \
    void waitForRequest() override {                                       \
      service_->Request##endpoint(&ctx_, &request_, &responder_, cq_, cq_, \
                                  this);                                   \
    }                                                                      \
    void handleRequest() noexcept override
#else
#define CALLDATA_IMPL(endpoint, type)                                      \
  class CallData##endpoint                                                 \
    : public CallData##type<inference::endpoint##Request,                  \
                            inference::endpoint##Response> {               \
   public:                                                                 \
    CallData##endpoint(AsyncService* service, ServerCompletionQueue* cq,   \
                       SharedState* state)                                 \
      : CallData##type(service, cq), state_(state) {                       \
      proceed();                                                           \
    }                                                                      \
                                                                           \
   private:                                                                \
    SharedState* state_;                                                   \
                                                                           \
   protected:                                                              \
    void addNewCallData() override {                                       \
      new CallData##endpoint(service_, cq_, state_);                       \
    }                                                                      \
    void waitForRequest() override {                                       \
      service_->Request##endpoint(&ctx_, &request_, &responder_, cq_, cq_, \
                                  this);                                   \
    }                                                                      \
    void handleRequest() noexcept override
#endif

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CALLDATA_IMPL_END \
  }                       \
  ;  // NOLINT

CALLDATA_IMPL(ModelInfer, Unary);

public:
const inference::ModelInferRequest& getRequest() const {
  return this->request_;
}

inference::ModelInferResponse& getReply() { return this->reply_; }
CALLDATA_IMPL_END

InferenceRequestInput getInput(
  const inference::ModelInferRequest_InferInputTensor& req,
  const MemoryPool* pool) {
  Observer observer;
  AMDINFER_IF_LOGGING(observer.logger = Logger{Loggers::Server});

  AMDINFER_LOG_TRACE(observer.logger,
                     "Creating InferenceRequestInput from proto tensor");

  InferenceRequestInput input;
  input.setName(req.name());

  std::vector<uint64_t> shape_vector;
  shape_vector.reserve(req.shape_size());
  for (const auto& index : req.shape()) {
    shape_vector.push_back(static_cast<size_t>(index));
  }
  input.setShape(shape_vector);
  input.setDatatype(DataType(req.datatype().c_str()));

  input.setParameters(mapProtoToParameters(req.parameters()));

  auto size = input.getSize();
  auto buffer = pool->get({MemoryAllocators::Cpu}, input, 1);
  input.setData(buffer->data(0));
  // auto* dest = static_cast<std::byte*>(input_buffer->data(offset));
  AMDINFER_LOG_TRACE(observer.logger, "Writing " + std::to_string(size) +
                                        " elements of type " +
                                        input.getDatatype().str() + " to " +
                                        util::addressToString(buffer->data(0)));

  switchOverTypes(WriteData(), input.getDatatype(), buffer.get(), &req, 0, size,
                  observer);

  return input;
}

InferenceRequestOutput getOutput(
  const inference::ModelInferRequest_InferRequestedOutputTensor& proto) {
  InferenceRequestOutput output;
  output.setData(nullptr);
  output.setName(proto.name());
  output.setParameters(mapProtoToParameters(proto.parameters()));
  return output;
}

void setCallback(InferenceRequest* request, CallDataModelInfer* calldata) {
  Callback callback = [calldata](const InferenceResponse& response) {
    if (response.isError()) {
      calldata->finish(
        ::grpc::Status(StatusCode::UNKNOWN, response.getError()));
      return;
    }
    try {
      mapResponseToProto(response, calldata->getReply());
    } catch (const invalid_argument& e) {
      calldata->finish(::grpc::Status(StatusCode::UNKNOWN, e.what()));
      return;
    }

    // #ifdef AMDINFER_ENABLE_TRACING
    //   const auto &context = response.getContext();
    //   propagate(resp.get(), context);
    // #endif
    calldata->finish(::grpc::Status::OK);
  };
  request->setCallback(std::move(callback));
}

InferenceRequestPtr getRequest(const inference::ModelInferRequest& grpc_request,
                               const MemoryPool* pool) {
  [[maybe_unused]] Observer observer;
  AMDINFER_IF_LOGGING(observer.logger = Logger{Loggers::Server});

  AMDINFER_LOG_TRACE(observer.logger,
                     "Creating InferenceRequest from proto tensor");

  auto request = std::make_shared<InferenceRequest>();

  request->setID(grpc_request.id());

  request->setParameters(mapProtoToParameters(grpc_request.parameters()));

  request->setCallback(nullptr);

  for (const auto& input : grpc_request.inputs()) {
    request->addInputTensor(getInput(input, pool));
  }

  if (grpc_request.outputs_size() != 0) {
    for (const auto& output : grpc_request.outputs()) {
      request->addOutputTensor(getOutput(output));
    }
  }

  return request;
}

// CALLDATA_IMPL(StreamModelInfer, ServerStream);

//  public:
//   const inference::ModelInferRequest& getRequest() const {
//     return this->request_;
//   }
// CALLDATA_IMPL_END

void grpcUnaryCallback(CallDataModelInfer* calldata,
                       const InferenceResponse& response) {
  if (response.isError()) {
    calldata->finish(::grpc::Status(StatusCode::UNKNOWN, response.getError()));
    return;
  }
  try {
    mapResponseToProto(response, calldata->getReply());
  } catch (const invalid_argument& e) {
    calldata->finish(::grpc::Status(StatusCode::UNKNOWN, e.what()));
    return;
  }

  // #ifdef AMDINFER_ENABLE_TRACING
  //   const auto &context = response.getContext();
  //   propagate(resp.get(), context);
  // #endif
  calldata->finish(::grpc::Status::OK);
}

CALLDATA_IMPL(ServerLive, Unary) {
  reply_.set_live(true);
  finish(::grpc::Status::OK);
}
CALLDATA_IMPL_END

CALLDATA_IMPL(ServerReady, Unary) {
  reply_.set_ready(true);
  finish(::grpc::Status::OK);
}
CALLDATA_IMPL_END

CALLDATA_IMPL(ModelReady, Unary) {
  const auto& model = request_.name();
  try {
    reply_.set_ready(state_->modelReady(model));
    finish(::grpc::Status::OK);
  } catch (const invalid_argument& e) {
    reply_.set_ready(false);
    finish(::grpc::Status(StatusCode::NOT_FOUND, e.what()));
  } catch (const std::exception& e) {
    reply_.set_ready(false);
    finish(::grpc::Status(StatusCode::UNKNOWN, e.what()));
  }
}
CALLDATA_IMPL_END

CALLDATA_IMPL(ModelMetadata, Unary) {
  const auto& model = request_.name();
  try {
    auto metadata = state_->modelMetadata(model);
    mapModelMetadataToProto(metadata, reply_);
    finish(::grpc::Status::OK);
  } catch (const invalid_argument& e) {
    finish(::grpc::Status(StatusCode::NOT_FOUND, e.what()));
  } catch (const std::exception& e) {
    finish(::grpc::Status(StatusCode::UNKNOWN, e.what()));
  }
}
CALLDATA_IMPL_END

CALLDATA_IMPL(ServerMetadata, Unary) {
  auto metadata = SharedState::serverMetadata();
  reply_.set_name(metadata.name);
  reply_.set_version(metadata.version);
  for (const auto& extension : metadata.extensions) {
    reply_.add_extensions(extension);
  }
  finish(::grpc::Status::OK);
}
CALLDATA_IMPL_END

CALLDATA_IMPL(ModelList, Unary) {
  auto models = state_->modelList();
  for (const auto& model : models) {
    reply_.add_models(model);
  }
  finish(::grpc::Status::OK);
}
CALLDATA_IMPL_END

CALLDATA_IMPL(ModelLoad, Unary) {
  auto parameters = mapProtoToParameters(request_.parameters());

  auto* model = request_.mutable_name();
  util::toLower(model);
  try {
    state_->modelLoad(*model, parameters);
  } catch (const runtime_error& e) {
    AMDINFER_LOG_ERROR(logger_, e.what());
    finish(::grpc::Status(StatusCode::NOT_FOUND, e.what()));
    return;
  } catch (const std::exception& e) {
    finish(::grpc::Status(StatusCode::UNKNOWN, e.what()));
    return;
  }

  finish(::grpc::Status::OK);
}
CALLDATA_IMPL_END

CALLDATA_IMPL(ModelUnload, Unary) {
  auto* model = request_.mutable_name();
  util::toLower(model);
  state_->modelUnload(*model);
  finish(::grpc::Status::OK);
}
CALLDATA_IMPL_END

CALLDATA_IMPL(WorkerLoad, Unary) {
  auto parameters = mapProtoToParameters(request_.parameters());

  auto* model = request_.mutable_name();
  util::toLower(model);

  try {
    auto endpoint = state_->workerLoad(*model, parameters);
    reply_.set_endpoint(endpoint);
    finish(::grpc::Status::OK);
  } catch (const runtime_error& e) {
    AMDINFER_LOG_ERROR(logger_, e.what());
    finish(::grpc::Status(StatusCode::NOT_FOUND, e.what()));
  } catch (const std::exception& e) {
    AMDINFER_LOG_ERROR(logger_, e.what());
    finish(::grpc::Status(StatusCode::UNKNOWN, e.what()));
  }
}
CALLDATA_IMPL_END

CALLDATA_IMPL(WorkerUnload, Unary) {
  auto* worker = request_.mutable_name();
  util::toLower(worker);
  state_->workerUnload(*worker);
  finish(::grpc::Status::OK);
}
CALLDATA_IMPL_END

CALLDATA_IMPL(HasHardware, Unary) {
  auto found = SharedState::hasHardware(request_.name(), request_.num());
  reply_.set_found(found);
  finish(::grpc::Status::OK);
}
CALLDATA_IMPL_END

void CallDataModelInfer::handleRequest() noexcept {
  const auto& model = request_.model_name();
#ifdef AMDINFER_ENABLE_TRACING
  auto trace = startTrace(&(__func__[0]));
  trace->setAttribute("model", model);
  trace->startSpan("request_handler");
#endif

  try {
    auto request = amdinfer::getRequest(request_, state_->getPool());
    setCallback(request.get(), this);
    auto request_container = std::make_unique<RequestContainer>();
    request_container->request = request;
#ifdef AMDINFER_ENABLE_TRACING
    trace->endSpan();
    request_container->trace = std::move(trace);
#endif
    state_->modelInfer(model, std::move(request_container));
  } catch (const invalid_argument& e) {
    AMDINFER_LOG_INFO(logger_, e.what());
    finish(::grpc::Status(StatusCode::NOT_FOUND, e.what()));
  } catch (const std::exception& e) {
    AMDINFER_LOG_ERROR(logger_, e.what());
    finish(::grpc::Status(StatusCode::UNKNOWN, e.what()));
  }
}

class GrpcServer final {
 public:
  /// Get the singleton GrpcServer instance
  static GrpcServer& getInstance() { return create("", -1, nullptr); }

  // using this singleton approach here because the start() method is state-
  // independent. The HTTP server is already global like this
  static GrpcServer& create(const std::string& address, const int cq_count,
                            SharedState* state) {
    static GrpcServer server(address, cq_count, state);
    return server;
  }

  GrpcServer(GrpcServer const&) = delete;  ///< Copy constructor
  GrpcServer& operator=(const GrpcServer&) =
    delete;                                 ///< Copy assignment constructor
  GrpcServer(GrpcServer&& other) = delete;  ///< Move constructor
  GrpcServer& operator=(GrpcServer&& other) =
    delete;  ///< Move assignment constructor

  ~GrpcServer() {
    server_->Shutdown();
    // Always shutdown the completion queues after the server.
    for (const auto& cq : cq_) {
      cq->Shutdown();
      void* tag = nullptr;
      bool ok = false;
      while (cq->Next(&tag, &ok)) {
        // drain the completion queue to prevent assertion errors in grpc
      }
    }
    for (auto& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

 private:
  GrpcServer(const std::string& address, const int cq_count, SharedState* state)
    : state_(state) {
    ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(kMaxGrpcMessageSize);
    builder.SetMaxSendMessageSize(kMaxGrpcMessageSize);
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(address, ::grpc::InsecureServerCredentials());
    // Register "service_" as the instance through which we'll communicate
    // with clients. In this case it corresponds to an *asynchronous* service.
    builder.RegisterService(&service_);
    // Get hold of the completion queue used for the asynchronous
    // communication with the gRPC runtime.
    for (auto i = 0; i < cq_count; i++) {
      cq_.push_back(builder.AddCompletionQueue());
    }
    // Finally assemble the server.
    server_ = builder.BuildAndStart();

    // Start threads to handle incoming RPCs
    for (auto i = 0; i < cq_count; i++) {
      threads_.emplace_back(&GrpcServer::handleRpcs, this, i);
    }
  }

  // This can be run in multiple threads if needed.
  void handleRpcs(int index) {
    const auto& my_cq = cq_.at(index);

    // Spawn a new CallData instance to serve new clients.
    new CallDataServerLive(&service_, my_cq.get(), state_);
    new CallDataServerMetadata(&service_, my_cq.get(), state_);
    new CallDataModelMetadata(&service_, my_cq.get(), state_);
    new CallDataServerReady(&service_, my_cq.get(), state_);
    new CallDataModelList(&service_, my_cq.get(), state_);
    new CallDataModelReady(&service_, my_cq.get(), state_);
    new CallDataModelLoad(&service_, my_cq.get(), state_);
    new CallDataModelUnload(&service_, my_cq.get(), state_);
    new CallDataWorkerLoad(&service_, my_cq.get(), state_);
    new CallDataWorkerUnload(&service_, my_cq.get(), state_);
    new CallDataModelInfer(&service_, my_cq.get(), state_);
    new CallDataHasHardware(&service_, my_cq.get(), state_);
    // new CallDataStreamModelInfer(&service_, my_cq.get());
    void* tag = nullptr;  // uniquely identifies a request.
    bool ok = false;
    while (true) {
      // the gRPC is shutting down in this case
      if (my_cq == nullptr) {
        return;
      }

      // Block waiting to read the next event from the completion queue. The
      // event is uniquely identified by its tag, which in this case is the
      // memory address of a CallDataBase instance.
      // The return value of Next should always be checked. This return value
      // tells us whether there is any kind of event or cq_ is shutting down.
      auto event_received = my_cq->Next(&tag, &ok);
      if (GPR_UNLIKELY(!(ok && event_received))) {
        break;
      }
      static_cast<CallDataBase*>(tag)->proceed();
    }
  }

  std::vector<std::unique_ptr<::grpc::ServerCompletionQueue>> cq_;
  inference::GRPCInferenceService::AsyncService service_;
  std::unique_ptr<::grpc::Server> server_;
  std::vector<std::thread> threads_;
  SharedState* state_;
};

namespace grpc {

void start(SharedState* state, int port) {
  const std::string address = "0.0.0.0:" + std::to_string(port);
  GrpcServer::create(address, 1, state);
}

void stop() {
  // the GrpcServer's destructor is called automatically
  // auto& foo = GrpcServer::getInstance();
  // foo.~GrpcServer();
}

}  // namespace grpc

}  // namespace amdinfer
