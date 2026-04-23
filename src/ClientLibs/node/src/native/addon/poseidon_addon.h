#ifndef POSEIDON_ADDON_H
#define POSEIDON_ADDON_H

#include <napi.h>

struct poseidon_client;

class PoseidonClientNative : public Napi::ObjectWrap<PoseidonClientNative> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  PoseidonClientNative(const Napi::CallbackInfo& info);
  ~PoseidonClientNative();

private:
  Napi::Value Connect(const Napi::CallbackInfo& info);
  Napi::Value Disconnect(const Napi::CallbackInfo& info);
  Napi::Value CreateChannel(const Napi::CallbackInfo& info);
  Napi::Value JoinChannel(const Napi::CallbackInfo& info);
  Napi::Value LeaveChannel(const Napi::CallbackInfo& info);
  Napi::Value DestroyChannel(const Napi::CallbackInfo& info);
  Napi::Value ModifyChannel(const Napi::CallbackInfo& info);
  Napi::Value Subscribe(const Napi::CallbackInfo& info);
  Napi::Value Unsubscribe(const Napi::CallbackInfo& info);
  Napi::Value Publish(const Napi::CallbackInfo& info);
  Napi::Value RegisterAlias(const Napi::CallbackInfo& info);
  Napi::Value UnregisterAlias(const Napi::CallbackInfo& info);
  Napi::Value OnDelivery(const Napi::CallbackInfo& info);
  Napi::Value OnEvent(const Napi::CallbackInfo& info);

  poseidon_client* client_ = nullptr;
  Napi::ThreadSafeFunction tsfn_delivery_;
  Napi::ThreadSafeFunction tsfn_event_;
};

#endif