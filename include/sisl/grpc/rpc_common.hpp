/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#pragma once

namespace sisl {
class GrpcServer;
class GenericRpcData;
enum class AuthVerifyStatus : uint8_t;

using generic_rpc_handler_cb_t = std::function< bool(boost::intrusive_ptr< GenericRpcData >&) >;
using generic_rpc_completed_cb_t = std::function< void(boost::intrusive_ptr< GenericRpcData >&) >;

struct RPCHelper {
    static bool has_server_shutdown(const GrpcServer* server);
    static bool run_generic_handler_cb(GrpcServer* server, const std::string& method,
                                       boost::intrusive_ptr< GenericRpcData >& rpc_data);
    static grpc::Status do_authorization(const GrpcServer* server, const grpc::ServerContext* srv_ctx);
    static grpc::StatusCode to_grpc_statuscode(const sisl::AuthVerifyStatus status);
};
} // namespace sisl::grpc
