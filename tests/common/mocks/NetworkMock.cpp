/*
 * Copyright (C) 2019-2020 HERE Europe B.V.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 * License-Filename: LICENSE
 */

#include "NetworkMock.h"

#include <thread>

namespace http = olp::http;

NetworkMock::NetworkMock() = default;

NetworkMock::~NetworkMock() = default;

std::tuple<http::RequestId, NetworkCallback, CancelCallback>
GenerateNetworkMockActions(
    const std::shared_ptr<std::promise<void>>& pre_signal,
    const std::shared_ptr<std::promise<void>>& wait_for_signal,
    const MockedResponseInformation& response_information,
    const std::shared_ptr<std::promise<void>>& post_signal) {
  static std::atomic<http::RequestId> s_request_id{
      static_cast<http::RequestId>(http::RequestIdConstants::RequestIdMin)};

  http::RequestId request_id = s_request_id.fetch_add(1);

  auto completed = std::make_shared<std::atomic_bool>(false);

  // callback is generated when the send method is executed, in order to receive
  // the cancel callback, we need to pass it to store it somewhere and share
  // with cancel mock.
  auto callback_placeholder = std::make_shared<http::Network::Callback>();

  auto mocked_send = [=](const http::NetworkRequest&,
                         const http::Network::Payload& payload,
                         const http::Network::Callback& callback,
                         const http::Network::HeaderCallback& header_callback,
                         const http::Network::DataCallback& data_callback) {
    *callback_placeholder = callback;

    auto mocked_network_block = [=]() {
      // emulate a small response delay
      std::this_thread::sleep_for(std::chrono::milliseconds(50));

      // notify waiting thread that we reached the network code
      pre_signal->set_value();

      // wait until test cancel request during execution
      wait_for_signal->get_future().get();

      // in the case request was not canceled return the expected payload
      if (!completed->exchange(true)) {
        const auto data_len = strlen(response_information.data);
        if (payload) {
          payload->write(response_information.data,
                         static_cast<std::streamsize>(data_len));
        } else if (data_callback) {
          data_callback(
              reinterpret_cast<const std::uint8_t*>(response_information.data),
              0, data_len);
        }

        for (const auto& header : response_information.headers) {
          header_callback(header.first, header.second);
        }

        callback(http::NetworkResponse()
                     .WithStatus(response_information.status)
                     .WithRequestId(request_id));
      }

      // notify that request finished
      post_signal->set_value();
    };

    // simulate that network code is actually running in the background.
    std::thread(std::move(mocked_network_block)).detach();

    return http::SendOutcome(request_id);
  };

  auto mocked_cancel = [=](http::RequestId id) {
    if (!completed->exchange(true)) {
      // simulate that network code is actually running in the background.
      std::thread([=]() {
        auto cancel_code = static_cast<int>(http::ErrorCode::CANCELLED_ERROR);
        (*callback_placeholder)(http::NetworkResponse()
                                    .WithError("Cancelled")
                                    .WithStatus(cancel_code)
                                    .WithRequestId(id));
      })
          .detach();
    }
  };

  return std::make_tuple(request_id, std::move(mocked_send),
                         std::move(mocked_cancel));
}

///
/// NetworkMock Actions
///
NetworkCallback ReturnHttpResponse(http::NetworkResponse response,
                                   const std::string& response_body,
                                   const http::Headers& headers,
                                   std::chrono::nanoseconds delay,
                                   http::RequestId request_id,
                                   std::uint64_t offset) {
  response.WithRequestId(request_id);
  return [=](const http::NetworkRequest&, const http::Network::Payload& payload,
             const http::Network::Callback& callback,
             const http::Network::HeaderCallback& header_callback,
             const http::Network::DataCallback& data_callback) mutable {
    std::thread([=]() {
      std::this_thread::sleep_for(delay);

      for (const auto& header : headers) {
        header_callback(header.first, header.second);
      }

      if (data_callback) {
        data_callback(
            reinterpret_cast<const unsigned char*>(response_body.c_str()),
            offset, response_body.length());
      } else {
        payload->seekp(0, std::ios_base::end);
        payload->write(response_body.c_str(), response_body.size());
        payload->seekp(0);
      }

      callback(response);
    })
        .detach();

    return http::SendOutcome(request_id);
  };
}
