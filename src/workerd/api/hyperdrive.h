// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "streams.h"
#include <capnp/compat/json.h>
#include <cstdint>
#include <kj/common.h>
#include <kj/async-io.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/http-util.h>

namespace workerd::api {

// A Hyperdrive resource for development integrations.
//
// Provides the same interface as Hyperdrive while sending connection
// traffic directly to postgres
class Hyperdrive : public jsg::Object {
public:
  // `clientIndex` is what to pass to IoContext::getHttpClient() to get an HttpClient
  // representing this namespace.
  explicit Hyperdrive(uint clientIndex, kj::String database,
                      kj::String user, kj::String password);
  jsg::Ref<Socket> connect(jsg::Lock& js);
  kj::StringPtr getDatabase();
  kj::StringPtr getUser();
  kj::StringPtr getPassword();

  kj::StringPtr getHost();
  uint16_t getPort();

  kj::String getConnectionString();

  JSG_RESOURCE_TYPE(Hyperdrive, CompatibilityFlags::Reader flags) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(database, getDatabase);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(user, getUser);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(password, getPassword);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(host, getHost);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(port, getPort);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(connectionString, getConnectionString);

    JSG_METHOD(connect);
  }

private:
  uint clientIndex;
  kj::String randomHost;
  kj::String database;
  kj::String user;
  kj::String password;
  bool registeredConnectOverride = false;
  kj::Promise<kj::Own<kj::AsyncIoStream>> connectToDb(); 
};
#define EW_HYPERDRIVE_ISOLATE_TYPES \
  api::Hyperdrive
} // namespace workerd::api
