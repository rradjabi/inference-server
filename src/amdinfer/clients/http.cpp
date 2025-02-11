// Copyright 2021 Xilinx, Inc.
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
 * @brief Implements the methods for interacting with the server with HTTP/REST
 */

#include "amdinfer/clients/http.hpp"

#include <drogon/HttpClient.h>            // for HttpClient, HttpClientPtr
#include <drogon/HttpRequest.h>           // for HttpRequest, HttpReques...
#include <drogon/HttpResponse.h>          // for HttpResponse
#include <drogon/HttpTypes.h>             // for k200OK, Get, Post, ReqR...
#include <json/value.h>                   // for Value, arrayValue, obje...
#include <trantor/net/EventLoopThread.h>  // for EventLoopThread

#include <cassert>        // for assert
#include <future>         // for promise
#include <unordered_set>  // for unordered_set
#include <utility>        // for tuple_element<>::type
#include <vector>

#include "amdinfer/clients/http_internal.hpp"    // for mapParametersToJson
#include "amdinfer/core/exceptions.hpp"          // for bad_status
#include "amdinfer/core/inference_request.hpp"   // for InferenceRequest
#include "amdinfer/core/inference_response.hpp"  // for InferenceResponse

namespace amdinfer {

void addHeaders(const drogon::HttpRequestPtr& req, const StringMap& headers) {
  for (const auto& [field, value] : headers) {
    req->addHeader(field, value);
  }
}

class HttpClient::HttpClientImpl {
 public:
  explicit HttpClientImpl(const std::string& address, StringMap headers,
                          int parallelism)
    : headers_(std::move(headers)), num_clients_(parallelism) {
    // arbitrarily use ratio of 16:1 between HttpClients and EventLoops
    const auto client_thread_ratio = 16;
    const auto threads = (parallelism / client_thread_ratio) + 1;

    loops_.reserve(threads);
    clients_.reserve(num_clients_);
    for (auto i = 0; i < threads; ++i) {
      // need to use unique_ptr because EventLoopThreads are not moveable or
      // copyable and so incompatible with std::vectors
      const auto& loop =
        loops_.emplace_back(std::make_unique<trantor::EventLoopThread>());
      loop->run();
    }
    for (auto i = 0; i < num_clients_; ++i) {
      const auto& loop = loops_[i % threads];
      clients_.emplace_back(
        drogon::HttpClient::newHttpClient(address, loop->getLoop()));
    }
  }

  drogon::HttpClient* getClient() {
    const auto& client = clients_[counter_];
    counter_ = (counter_ + 1) % num_clients_;
    return client.get();
  }

  const StringMap& getHeaders() const { return headers_; }

  auto getClientNum() const { return num_clients_; }

 private:
  StringMap headers_;
  int counter_ = 0;
  int num_clients_;
  std::vector<std::unique_ptr<trantor::EventLoopThread>> loops_;
  std::vector<drogon::HttpClientPtr> clients_;
};

HttpClient::HttpClient(const std::string& address) {
  const auto parallelism = 32;
  this->impl_ = std::make_unique<HttpClient::HttpClientImpl>(
    address, StringMap{}, parallelism);
}

HttpClient::HttpClient(const std::string& address, const StringMap& headers,
                       int parallelism) {
  this->impl_ =
    std::make_unique<HttpClient::HttpClientImpl>(address, headers, parallelism);
}

// needed for HttpClientImpl forward declaration in WebSocket client
HttpClient::~HttpClient() = default;

void checkError(drogon::ReqResult result) {
  using drogon::ReqResult;

  std::string error_msg;
  switch (result) {
    case ReqResult::Ok:
      break;
    case ReqResult::BadResponse:
      throw bad_status{"Bad response"};
    case ReqResult::NetworkFailure:
      throw bad_status{"Network failure"};
    case ReqResult::BadServerAddress:
      throw connection_error{"Cannot connect to the server"};
    case ReqResult::Timeout:
      throw bad_status{"Timeout"};
    case ReqResult::HandshakeError:
      throw bad_status{"Handshake error"};
    case ReqResult::InvalidCertificate:
      throw bad_status{"Invalid certificate"};
    default:
      throw bad_status{"Request error code: " +
                       std::to_string(static_cast<int>(result))};
  }
}

drogon::HttpRequestPtr createGetRequest(const std::string& path,
                                        const StringMap& headers) {
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Get);
  req->setPath(path);
  addHeaders(req, headers);
  return req;
}

drogon::HttpRequestPtr createPostRequest(const Json::Value& json,
                                         const std::string& path,
                                         const StringMap& headers) {
  auto req = drogon::HttpRequest::newHttpJsonRequest(json);
  req->setMethod(drogon::Post);
  req->setPath(path);
  addHeaders(req, headers);
  return req;
}

ServerMetadata HttpClient::serverMetadata() const {
  auto* client = this->impl_->getClient();
  auto req = createGetRequest("/v2", impl_->getHeaders());

  auto [result, response] = client->sendRequest(req);
  checkError(result);
  if (response->statusCode() != drogon::k200OK) {
    throw bad_status(response->getJsonError());
  }
  ServerMetadata metadata;
  auto json = response->getJsonObject();
  metadata.name = json->get("name", "").asString();
  metadata.version = json->get("version", "").asString();
  auto extensions = json->get("extensions", Json::arrayValue);
  for (auto const& extension : extensions) {
    metadata.extensions.insert(extension.asString());
  }
  return metadata;
}

bool HttpClient::serverLive() const {
  auto* client = this->impl_->getClient();
  auto req = createGetRequest("/v2/health/live", impl_->getHeaders());

  // arbitrarily setting a 10 second timeout
  const auto timeout_s = 10.0;
  auto [result, response] = client->sendRequest(req, timeout_s);
  if (result != drogon::ReqResult::Ok) {
    return false;
  }
  return response->statusCode() == drogon::k200OK;
}

bool HttpClient::serverReady() const {
  auto* client = this->impl_->getClient();
  auto req = createGetRequest("/v2/health/ready", impl_->getHeaders());

  auto [result, response] = client->sendRequest(req);
  checkError(result);
  return response->statusCode() == drogon::k200OK;
}

bool HttpClient::modelReady(const std::string& model) const {
  auto* client = this->impl_->getClient();
  auto req =
    createGetRequest("/v2/models/" + model + "/ready", impl_->getHeaders());

  auto [result, response] = client->sendRequest(req);
  checkError(result);
  return response->statusCode() == drogon::k200OK;
}

ModelMetadata HttpClient::modelMetadata(const std::string& model) const {
  auto* client = this->impl_->getClient();
  auto req = createGetRequest("/v2/models/" + model, impl_->getHeaders());

  auto [result, response] = client->sendRequest(req);
  checkError(result);
  auto resp = response->jsonObject();
  return mapJsonToModelMetadata(resp.get());
}

void HttpClient::modelLoad(const std::string& model,
                           const ParameterMap& parameters) const {
  auto* client = this->impl_->getClient();

  Json::Value json = Json::objectValue;
  json = mapParametersToJson(parameters);

  auto req = createPostRequest(json, "/v2/repository/models/" + model + "/load",
                               impl_->getHeaders());

  auto [result, response] = client->sendRequest(req);
  checkError(result);
  if (response->statusCode() != drogon::k200OK) {
    throw bad_status(std::string(response->body()));
  }
}

void HttpClient::modelUnload(const std::string& model) const {
  auto* client = this->impl_->getClient();

  Json::Value json;
  auto req = createPostRequest(
    json, "/v2/repository/models/" + model + "/unload", impl_->getHeaders());

  auto [result, response] = client->sendRequest(req);
  checkError(result);
  auto status = response->statusCode();
  if (status != drogon::k200OK) {
    throw bad_status(std::string(response->body()));
  }
}

std::string HttpClient::workerLoad(const std::string& worker,
                                   const ParameterMap& parameters) const {
  auto* client = this->impl_->getClient();

  Json::Value json = Json::objectValue;
  json = mapParametersToJson(parameters);

  auto req = createPostRequest(json, "/v2/workers/" + worker + "/load",
                               impl_->getHeaders());

  auto [result, response] = client->sendRequest(req);
  checkError(result);
  if (response->statusCode() != drogon::k200OK) {
    throw bad_status(std::string(response->body()));
  }
  return std::string(response->body());
}

void HttpClient::workerUnload(const std::string& worker) const {
  auto* client = this->impl_->getClient();

  Json::Value json;
  auto req = createPostRequest(json, "/v2/workers/" + worker + "/unload",
                               impl_->getHeaders());

  auto [result, response] = client->sendRequest(req);
  checkError(result);
  auto status = response->statusCode();
  if (status != drogon::k200OK) {
    throw bad_status(std::string(response->body()));
  }
}

auto createInferenceRequest(const std::string& model,
                            const InferenceRequest& request,
                            const StringMap& headers) {
  if (request.getInputs().empty()) {
    throw invalid_argument("The request's inputs cannot be empty");
  }

  auto json = mapRequestToJson(request);
  return createPostRequest(json, "/v2/models/" + model + "/infer", headers);
}

InferenceResponseFuture HttpClient::modelInferAsync(
  const std::string& model, const InferenceRequest& request) const {
  auto req = createInferenceRequest(model, request, impl_->getHeaders());
  auto prom = std::make_shared<std::promise<amdinfer::InferenceResponse>>();
  auto fut = prom->get_future();

  auto* client = this->impl_->getClient();
  client->sendRequest(req, [prom](drogon::ReqResult result,
                                  const drogon::HttpResponsePtr& response) {
    // throwing exceptions asynchronously makes them difficult to process so
    // just return an error object. Unfortunately, there's no way to know which
    // request just errored out since response is likely nullptr if Drogon
    // failed
    std::string error;
    try {
      checkError(result);
      if (response->statusCode() != drogon::k200OK) {
        throw bad_status(std::string(response->body()));
      }
    } catch (const bad_status& e) {
      error = e.what();
    }
    if (error.empty()) {
      auto resp = response->jsonObject();
      prom->set_value(mapJsonToResponse(resp.get()));
    } else {
      prom->set_value(InferenceResponse(error));
    }
  });

  return fut;
}

InferenceResponse HttpClient::modelInfer(
  const std::string& model, const InferenceRequest& request) const {
  auto req = createInferenceRequest(model, request, impl_->getHeaders());

  auto* client = this->impl_->getClient();
  auto [result, response] = client->sendRequest(req);
  checkError(result);
  if (response->statusCode() != drogon::k200OK) {
    throw bad_status(std::string{response->body()});
  }

  auto resp = response->jsonObject();
  return mapJsonToResponse(resp.get());
}

std::vector<std::string> HttpClient::modelList() const {
  auto* client = this->impl_->getClient();
  auto req = createGetRequest("/v2/models", impl_->getHeaders());

  auto [result, response] = client->sendRequest(req);
  checkError(result);
  if (response->statusCode() != drogon::k200OK) {
    throw bad_status(response->getJsonError());
  }
  auto json = response->jsonObject();

  auto json_models = json->get("models", Json::arrayValue);
  std::vector<std::string> models;
  models.reserve(json_models.size());
  for (const auto& model : json_models) {
    models.push_back(model.asString());
  }
  return models;
}

bool HttpClient::hasHardware(const std::string& name, int num) const {
  auto* client = this->impl_->getClient();

  Json::Value json;
  json["name"] = name;
  json["num"] = num;
  auto req = createPostRequest(json, "/v2/hardware", impl_->getHeaders());

  auto [result, response] = client->sendRequest(req);
  checkError(result);
  return response->statusCode() == drogon::k200OK;
}

}  // namespace amdinfer
