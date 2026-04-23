#include "poseidon_addon.h"
#include <poseidon/poseidon_client.h>

// ============================================================================
// Async workers: run C client functions off the JS thread, resolve/reject
// ============================================================================

class PoseidonWorker : public Napi::AsyncWorker {
public:
  using WorkFn = std::function<int()>;
  using ResultFn = std::function<void(Napi::Promise::Deferred&)>;

  PoseidonWorker(Napi::Promise::Deferred deferred, WorkFn work, ResultFn on_ok)
    : Napi::AsyncWorker(deferred.Env(), "PoseidonWorker"),
      deferred_(std::move(deferred)),
      work_(std::move(work)),
      on_ok_(std::move(on_ok)) {}

  void Execute() override {
    result_ = work_();
  }

  void OnOK() override {
    if (result_ == 0) {
      on_ok_(deferred_);
    } else {
      deferred_.Reject(Napi::Error::New(Env(), "Operation failed").Value());
    }
  }

  void OnError(const Napi::Error& err) override {
    deferred_.Reject(err.Value());
  }

private:
  Napi::Promise::Deferred deferred_;
  WorkFn work_;
  ResultFn on_ok_;
  int result_ = -1;
};

class PoseidonStringWorker : public Napi::AsyncWorker {
public:
  using WorkFn = std::function<int(std::string&)>;

  PoseidonStringWorker(Napi::Promise::Deferred deferred, WorkFn work)
    : Napi::AsyncWorker(deferred.Env(), "PoseidonStringWorker"),
      deferred_(std::move(deferred)),
      work_(std::move(work)) {}

  void Execute() override {
    result_ = work_(result_data_);
  }

  void OnOK() override {
    if (result_ == 0) {
      deferred_.Resolve(Napi::String::New(Env(), result_data_));
    } else {
      deferred_.Reject(Napi::Error::New(Env(), "Operation failed").Value());
    }
  }

  void OnError(const Napi::Error& err) override {
    deferred_.Reject(err.Value());
  }

private:
  Napi::Promise::Deferred deferred_;
  WorkFn work_;
  std::string result_data_;
  int result_ = -1;
};

// ============================================================================
// Init / Constructor / Destructor
// ============================================================================

Napi::Object PoseidonClientNative::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(env, "PoseidonClientNative", {
    InstanceMethod("connect", &PoseidonClientNative::Connect),
    InstanceMethod("disconnect", &PoseidonClientNative::Disconnect),
    InstanceMethod("createChannel", &PoseidonClientNative::CreateChannel),
    InstanceMethod("joinChannel", &PoseidonClientNative::JoinChannel),
    InstanceMethod("leaveChannel", &PoseidonClientNative::LeaveChannel),
    InstanceMethod("destroyChannel", &PoseidonClientNative::DestroyChannel),
    InstanceMethod("modifyChannel", &PoseidonClientNative::ModifyChannel),
    InstanceMethod("subscribe", &PoseidonClientNative::Subscribe),
    InstanceMethod("unsubscribe", &PoseidonClientNative::Unsubscribe),
    InstanceMethod("publish", &PoseidonClientNative::Publish),
    InstanceMethod("registerAlias", &PoseidonClientNative::RegisterAlias),
    InstanceMethod("unregisterAlias", &PoseidonClientNative::UnregisterAlias),
    InstanceMethod("onDelivery", &PoseidonClientNative::OnDelivery),
    InstanceMethod("onEvent", &PoseidonClientNative::OnEvent),
  });
  exports.Set("PoseidonClientNative", func);
  return exports;
}

PoseidonClientNative::PoseidonClientNative(const Napi::CallbackInfo& info)
  : Napi::ObjectWrap<PoseidonClientNative>(info) {}

PoseidonClientNative::~PoseidonClientNative() {
  if (client_) {
    poseidon_client_destroy(client_);
    client_ = nullptr;
  }
  if (tsfn_delivery_) tsfn_delivery_.Release();
  if (tsfn_event_) tsfn_event_.Release();
}

// ============================================================================
// Connect / Disconnect
// ============================================================================

Napi::Value PoseidonClientNative::Connect(const Napi::CallbackInfo& info) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(info.Env());

  if (info.Length() < 1 || !info[0].IsString()) {
    deferred.Reject(Napi::TypeError::New(info.Env(), "url string required").Value());
    return deferred.Promise();
  }
  if (client_) {
    deferred.Reject(Napi::Error::New(info.Env(), "Already connected").Value());
    return deferred.Promise();
  }

  std::string url = info[0].As<Napi::String>().Utf8Value();
  auto* self = this;
  Napi::Promise promise = deferred.Promise();

  auto worker = new PoseidonWorker(std::move(deferred),
    [self, url]() -> int {
      self->client_ = poseidon_client_create(url.c_str());
      if (!self->client_) return -1;
      return poseidon_client_connect(self->client_);
    },
    [](Napi::Promise::Deferred& d) { d.Resolve(d.Env().Undefined()); }
  );
  worker->Queue();
  return promise;
}

Napi::Value PoseidonClientNative::Disconnect(const Napi::CallbackInfo& info) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(info.Env());

  if (!client_) {
    deferred.Resolve(info.Env().Undefined());
    return deferred.Promise();
  }

  auto* self = this;
  Napi::Promise promise = deferred.Promise();

  auto worker = new PoseidonWorker(std::move(deferred),
    [self]() -> int {
      poseidon_client_destroy(self->client_);
      self->client_ = nullptr;
      return 0;
    },
    [](Napi::Promise::Deferred& d) { d.Resolve(d.Env().Undefined()); }
  );
  worker->Queue();
  return promise;
}

// ============================================================================
// Request methods
// ============================================================================

Napi::Value PoseidonClientNative::CreateChannel(const Napi::CallbackInfo& info) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(info.Env());
  if (!client_) {
    deferred.Reject(Napi::Error::New(info.Env(), "Not connected").Value());
    return deferred.Promise();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    deferred.Reject(Napi::TypeError::New(info.Env(), "name string required").Value());
    return deferred.Promise();
  }

  std::string name = info[0].As<Napi::String>().Utf8Value();
  auto* self = this;
  Napi::Promise promise = deferred.Promise();

  auto worker = new PoseidonStringWorker(std::move(deferred),
    [self, name](std::string& result) -> int {
      char buf[1024] = {};
      int rc = poseidon_client_create_channel(self->client_, name.c_str(), buf, sizeof(buf));
      if (rc == 0) result = buf;
      return rc;
    }
  );
  worker->Queue();
  return promise;
}

Napi::Value PoseidonClientNative::JoinChannel(const Napi::CallbackInfo& info) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(info.Env());
  if (!client_) {
    deferred.Reject(Napi::Error::New(info.Env(), "Not connected").Value());
    return deferred.Promise();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    deferred.Reject(Napi::TypeError::New(info.Env(), "topicOrAlias string required").Value());
    return deferred.Promise();
  }

  std::string topic = info[0].As<Napi::String>().Utf8Value();
  auto* self = this;
  Napi::Promise promise = deferred.Promise();

  auto worker = new PoseidonStringWorker(std::move(deferred),
    [self, topic](std::string& result) -> int {
      char buf[1024] = {};
      int rc = poseidon_client_join_channel(self->client_, topic.c_str(), buf, sizeof(buf));
      if (rc == 0) result = buf;
      return rc;
    }
  );
  worker->Queue();
  return promise;
}

Napi::Value PoseidonClientNative::LeaveChannel(const Napi::CallbackInfo& info) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(info.Env());
  if (!client_) {
    deferred.Reject(Napi::Error::New(info.Env(), "Not connected").Value());
    return deferred.Promise();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    deferred.Reject(Napi::TypeError::New(info.Env(), "topicId string required").Value());
    return deferred.Promise();
  }

  std::string topic_id = info[0].As<Napi::String>().Utf8Value();
  auto* self = this;
  Napi::Promise promise = deferred.Promise();

  auto worker = new PoseidonWorker(std::move(deferred),
    [self, topic_id]() -> int {
      return poseidon_client_leave_channel(self->client_, topic_id.c_str());
    },
    [](Napi::Promise::Deferred& d) { d.Resolve(d.Env().Undefined()); }
  );
  worker->Queue();
  return promise;
}

Napi::Value PoseidonClientNative::DestroyChannel(const Napi::CallbackInfo& info) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(info.Env());
  if (!client_) {
    deferred.Reject(Napi::Error::New(info.Env(), "Not connected").Value());
    return deferred.Promise();
  }
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
    deferred.Reject(Napi::TypeError::New(info.Env(), "topicId and ownerKeyPem strings required").Value());
    return deferred.Promise();
  }

  std::string topic_id = info[0].As<Napi::String>().Utf8Value();
  std::string pem = info[1].As<Napi::String>().Utf8Value();
  auto* self = this;
  Napi::Promise promise = deferred.Promise();

  auto worker = new PoseidonWorker(std::move(deferred),
    [self, topic_id, pem]() -> int {
      poseidon_key_pair_t* key = poseidon_key_pair_load_from_pem(pem.c_str());
      if (!key) return -1;
      int rc = poseidon_client_destroy_channel(self->client_, topic_id.c_str(), key);
      poseidon_key_pair_destroy(key);
      return rc;
    },
    [](Napi::Promise::Deferred& d) { d.Resolve(d.Env().Undefined()); }
  );
  worker->Queue();
  return promise;
}

Napi::Value PoseidonClientNative::ModifyChannel(const Napi::CallbackInfo& info) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(info.Env());
  if (!client_) {
    deferred.Reject(Napi::Error::New(info.Env(), "Not connected").Value());
    return deferred.Promise();
  }
  if (info.Length() < 3 || !info[0].IsString() || !info[2].IsString()) {
    deferred.Reject(Napi::TypeError::New(info.Env(), "topicId, config, and ownerKeyPem required").Value());
    return deferred.Promise();
  }

  std::string topic_id = info[0].As<Napi::String>().Utf8Value();
  std::vector<uint8_t> config_data;
  if (info[1].IsBuffer()) {
    auto buf = info[1].As<Napi::Buffer<uint8_t>>();
    config_data.assign(buf.Data(), buf.Data() + buf.Length());
  }
  std::string pem = info[2].As<Napi::String>().Utf8Value();
  auto* self = this;
  Napi::Promise promise = deferred.Promise();

  auto worker = new PoseidonWorker(std::move(deferred),
    [self, topic_id, config_data, pem]() -> int {
      poseidon_key_pair_t* key = poseidon_key_pair_load_from_pem(pem.c_str());
      if (!key) return -1;
      int rc = poseidon_client_modify_channel(self->client_, topic_id.c_str(),
        config_data.empty() ? nullptr : config_data.data(),
        config_data.size(), key);
      poseidon_key_pair_destroy(key);
      return rc;
    },
    [](Napi::Promise::Deferred& d) { d.Resolve(d.Env().Undefined()); }
  );
  worker->Queue();
  return promise;
}

Napi::Value PoseidonClientNative::Subscribe(const Napi::CallbackInfo& info) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(info.Env());
  if (!client_) {
    deferred.Reject(Napi::Error::New(info.Env(), "Not connected").Value());
    return deferred.Promise();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    deferred.Reject(Napi::TypeError::New(info.Env(), "topicPath string required").Value());
    return deferred.Promise();
  }

  std::string path = info[0].As<Napi::String>().Utf8Value();
  auto* self = this;
  Napi::Promise promise = deferred.Promise();

  auto worker = new PoseidonWorker(std::move(deferred),
    [self, path]() -> int {
      return poseidon_client_subscribe(self->client_, path.c_str());
    },
    [](Napi::Promise::Deferred& d) { d.Resolve(d.Env().Undefined()); }
  );
  worker->Queue();
  return promise;
}

Napi::Value PoseidonClientNative::Unsubscribe(const Napi::CallbackInfo& info) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(info.Env());
  if (!client_) {
    deferred.Reject(Napi::Error::New(info.Env(), "Not connected").Value());
    return deferred.Promise();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    deferred.Reject(Napi::TypeError::New(info.Env(), "topicPath string required").Value());
    return deferred.Promise();
  }

  std::string path = info[0].As<Napi::String>().Utf8Value();
  auto* self = this;
  Napi::Promise promise = deferred.Promise();

  auto worker = new PoseidonWorker(std::move(deferred),
    [self, path]() -> int {
      return poseidon_client_unsubscribe(self->client_, path.c_str());
    },
    [](Napi::Promise::Deferred& d) { d.Resolve(d.Env().Undefined()); }
  );
  worker->Queue();
  return promise;
}

Napi::Value PoseidonClientNative::Publish(const Napi::CallbackInfo& info) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(info.Env());
  if (!client_) {
    deferred.Reject(Napi::Error::New(info.Env(), "Not connected").Value());
    return deferred.Promise();
  }
  if (info.Length() < 2 || !info[0].IsString()) {
    deferred.Reject(Napi::TypeError::New(info.Env(), "topicPath and data required").Value());
    return deferred.Promise();
  }

  std::string path = info[0].As<Napi::String>().Utf8Value();
  std::vector<uint8_t> data;
  if (info[1].IsBuffer()) {
    auto buf = info[1].As<Napi::Buffer<uint8_t>>();
    data.assign(buf.Data(), buf.Data() + buf.Length());
  }
  auto* self = this;
  Napi::Promise promise = deferred.Promise();

  auto worker = new PoseidonWorker(std::move(deferred),
    [self, path, data]() -> int {
      return poseidon_client_publish(self->client_, path.c_str(),
        data.empty() ? nullptr : data.data(), data.size());
    },
    [](Napi::Promise::Deferred& d) { d.Resolve(d.Env().Undefined()); }
  );
  worker->Queue();
  return promise;
}

Napi::Value PoseidonClientNative::RegisterAlias(const Napi::CallbackInfo& info) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(info.Env());
  if (!client_) {
    deferred.Reject(Napi::Error::New(info.Env(), "Not connected").Value());
    return deferred.Promise();
  }
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
    deferred.Reject(Napi::TypeError::New(info.Env(), "name and topicId strings required").Value());
    return deferred.Promise();
  }

  std::string name = info[0].As<Napi::String>().Utf8Value();
  std::string topic_id = info[1].As<Napi::String>().Utf8Value();
  auto* self = this;
  Napi::Promise promise = deferred.Promise();

  auto worker = new PoseidonWorker(std::move(deferred),
    [self, name, topic_id]() -> int {
      return poseidon_client_register_alias(self->client_, name.c_str(), topic_id.c_str());
    },
    [](Napi::Promise::Deferred& d) { d.Resolve(d.Env().Undefined()); }
  );
  worker->Queue();
  return promise;
}

Napi::Value PoseidonClientNative::UnregisterAlias(const Napi::CallbackInfo& info) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(info.Env());
  if (!client_) {
    deferred.Reject(Napi::Error::New(info.Env(), "Not connected").Value());
    return deferred.Promise();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    deferred.Reject(Napi::TypeError::New(info.Env(), "name string required").Value());
    return deferred.Promise();
  }

  std::string name = info[0].As<Napi::String>().Utf8Value();
  auto* self = this;
  Napi::Promise promise = deferred.Promise();

  auto worker = new PoseidonWorker(std::move(deferred),
    [self, name]() -> int {
      return poseidon_client_unregister_alias(self->client_, name.c_str());
    },
    [](Napi::Promise::Deferred& d) { d.Resolve(d.Env().Undefined()); }
  );
  worker->Queue();
  return promise;
}

// ============================================================================
// Callbacks (ThreadSafeFunction)
// ============================================================================

Napi::Value PoseidonClientNative::OnDelivery(const Napi::CallbackInfo& info) {
  if (info.Length() < 1 || !info[0].IsFunction()) {
    return info.Env().Undefined();
  }

  Napi::Function cb = info[0].As<Napi::Function>();
  tsfn_delivery_ = Napi::ThreadSafeFunction::New(
    info.Env(), cb, "poseidon_delivery", 0, 1);

  if (client_) {
    auto* tsfn_ptr = &tsfn_delivery_;
    poseidon_client_on_delivery(client_,
      [](void* ctx, const char* topic_id, const char* subtopic,
         const uint8_t* data, size_t len) {
        auto* tsfn = static_cast<Napi::ThreadSafeFunction*>(ctx);
        struct Payload {
          std::string topic;
          std::string sub;
          std::vector<uint8_t> data;
        };
        auto* p = new Payload{topic_id ? topic_id : "", subtopic ? subtopic : "",
                               std::vector<uint8_t>(data, data + len)};
        tsfn->NonBlockingCall(p, [](Napi::Env env, Napi::Function js_cb, Payload* p) {
          auto buf = Napi::Buffer<uint8_t>::Copy(env, p->data.data(), p->data.size());
          js_cb.Call({Napi::String::New(env, p->topic), Napi::String::New(env, p->sub), buf});
          delete p;
        });
      }, tsfn_ptr);
  }

  return info.Env().Undefined();
}

Napi::Value PoseidonClientNative::OnEvent(const Napi::CallbackInfo& info) {
  if (info.Length() < 1 || !info[0].IsFunction()) {
    return info.Env().Undefined();
  }

  Napi::Function cb = info[0].As<Napi::Function>();
  tsfn_event_ = Napi::ThreadSafeFunction::New(
    info.Env(), cb, "poseidon_event", 0, 1);

  if (client_) {
    auto* tsfn_ptr = &tsfn_event_;
    poseidon_client_on_event(client_,
      [](void* ctx, uint8_t event_type, const uint8_t* data, size_t len) {
        auto* tsfn = static_cast<Napi::ThreadSafeFunction*>(ctx);
        struct Payload {
          uint8_t type;
          std::vector<uint8_t> data;
        };
        auto* p = new Payload{event_type, std::vector<uint8_t>(data, data + len)};
        tsfn->NonBlockingCall(p, [](Napi::Env env, Napi::Function js_cb, Payload* p) {
          auto buf = Napi::Buffer<uint8_t>::Copy(env, p->data.data(), p->data.size());
          js_cb.Call({Napi::Number::New(env, p->type), buf});
          delete p;
        });
      }, tsfn_ptr);
  }

  return info.Env().Undefined();
}

// ============================================================================
// Module registration
// ============================================================================

Napi::Object InitModule(Napi::Env env, Napi::Object exports) {
  return PoseidonClientNative::Init(env, exports);
}

NODE_API_MODULE(poseidon_node_native, InitModule)