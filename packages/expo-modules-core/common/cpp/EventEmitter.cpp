#include "JSIUtils.h"
#include "EventEmitter.h"
#include "LazyObject.h"

namespace expo::EventEmitter {

#pragma mark - Listeners

void Listeners::add(jsi::Runtime &runtime, std::string eventName, const jsi::Function &listener) noexcept {
  listenersMap[eventName].emplace_back(runtime, listener);
}

void Listeners::remove(jsi::Runtime &runtime, std::string eventName, const jsi::Function &listener) noexcept {
  if (!listenersMap.contains(eventName)) {
    return;
  }
  jsi::Value listenerValue(runtime, listener);

  listenersMap[eventName].remove_if([&](const jsi::Value &item) {
    return jsi::Value::strictEquals(runtime, listenerValue, item);
  });
}

void Listeners::removeAll(std::string eventName) noexcept {
  if (listenersMap.contains(eventName)) {
    listenersMap[eventName].clear();
  }
}

void Listeners::clear() noexcept {
  listenersMap.clear();
}

size_t Listeners::listenersCount(std::string eventName) noexcept {
  if (!listenersMap.contains(eventName)) {
    return 0;
  }
  return listenersMap[eventName].size();
}

void Listeners::call(jsi::Runtime &runtime, std::string eventName, const jsi::Object &thisObject, const jsi::Value *args, size_t count) noexcept {
  if (!listenersMap.contains(eventName)) {
    return;
  }
  ListenersList &listenersList = listenersMap[eventName];

  for (const jsi::Value &listener : listenersList) {
    listener
      .asObject(runtime)
      .asFunction(runtime)
      .callWithThis(runtime, thisObject, args, count);
  }
}

#pragma mark - NativeState

NativeState::NativeState() : jsi::NativeState() {}

NativeState::~NativeState() {
  listeners.clear();
}

NativeState::Shared NativeState::get(jsi::Runtime &runtime, const jsi::Object &object, bool createIfMissing) {
  if (object.hasNativeState<NativeState>(runtime)) {
    return object.getNativeState<NativeState>(runtime);
  }
  if (createIfMissing) {
    NativeState::Shared state = std::make_shared<NativeState>();
    object.setNativeState(runtime, state);
    return state;
  }
  return nullptr;
}

#pragma mark - Utils

void callObservingFunction(jsi::Runtime &runtime, const jsi::Object &object, const char* functionName, std::string eventName) {
  jsi::Value fnValue = object.getProperty(runtime, functionName);

  if (!fnValue.isObject()) {
    // Skip it if there is no observing function.
    return;
  }

  fnValue
    .getObject(runtime)
    .asFunction(runtime)
    .callWithThis(runtime, object, {
      jsi::Value(runtime, jsi::String::createFromUtf8(runtime, eventName))
    });
}

void addListener(jsi::Runtime &runtime, const jsi::Object &emitter, const std::string &eventName, const jsi::Function &listener) {
  if (NativeState::Shared state = NativeState::get(runtime, emitter, true)) {
    state->listeners.add(runtime, eventName, listener);

    if (state->listeners.listenersCount(eventName) == 1) {
      callObservingFunction(runtime, emitter, "startObserving", eventName);
    }
  }
}

void removeListener(jsi::Runtime &runtime, const jsi::Object &emitter, const std::string &eventName, const jsi::Function &listener) {
  if (NativeState::Shared state = NativeState::get(runtime, emitter, false)) {
    size_t listenersCountBefore = state->listeners.listenersCount(eventName);

    state->listeners.remove(runtime, eventName, listener);

    if (listenersCountBefore >= 1 && state->listeners.listenersCount(eventName) == 0) {
      callObservingFunction(runtime, emitter, "stopObserving", eventName);
    }
  }
}

void removeAllListeners(jsi::Runtime &runtime, const jsi::Object &emitter, const std::string &eventName) {
  if (NativeState::Shared state = NativeState::get(runtime, emitter, false)) {
    size_t listenersCountBefore = state->listeners.listenersCount(eventName);

    state->listeners.removeAll(eventName);

    if (listenersCountBefore >= 1) {
      callObservingFunction(runtime, emitter, "stopObserving", eventName);
    }
  }
}

void emitEvent(jsi::Runtime &runtime, const jsi::Object &emitter, const std::string &eventName, const jsi::Value *args, size_t count) {
  if (NativeState::Shared state = NativeState::get(runtime, emitter, false)) {
    state->listeners.call(runtime, eventName, emitter, args, count);
  }
}

jsi::Value createEventSubscription(jsi::Runtime &runtime, const std::string &eventName, const jsi::Object &emitter, const jsi::Function &listener) {
  jsi::Object subscription(runtime);
  jsi::PropNameID removeProp = jsi::PropNameID::forAscii(runtime, "remove", 6);
  std::shared_ptr<jsi::Value> emitterValue = std::make_shared<jsi::Value>(runtime, emitter);
  std::shared_ptr<jsi::Value> listenerValue = std::make_shared<jsi::Value>(runtime, listener);

  jsi::HostFunctionType removeSubscription = [eventName, emitterValue, listenerValue](jsi::Runtime &runtime, const jsi::Value &thisValue, const jsi::Value *args, size_t count) -> jsi::Value {
    jsi::Object emitter = emitterValue->getObject(runtime);
    jsi::Function listener = listenerValue->getObject(runtime).getFunction(runtime);

    removeListener(runtime, emitter, eventName, listener);
    return jsi::Value::undefined();
  };

  subscription.setProperty(runtime, removeProp, jsi::Function::createFromHostFunction(runtime, removeProp, 0, removeSubscription));

  return jsi::Value(runtime, subscription);
}

#pragma mark - Public API

void emitEvent(jsi::Runtime &runtime, jsi::Object &emitter, const std::string &eventName, const std::vector<jsi::Value> &arguments) {
  emitEvent(runtime, emitter, eventName, arguments.data(), arguments.size());
}

jsi::Function getClass(jsi::Runtime &runtime) {
  return common::getCoreObject(runtime)
    .getPropertyAsFunction(runtime, "EventEmitter");
}

void installClass(jsi::Runtime &runtime) {
  jsi::Function eventEmitterClass = common::createClass(runtime, "EventEmitter", [](jsi::Runtime &runtime, const jsi::Value &thisValue, const jsi::Value *args, size_t count) -> jsi::Value {
    // To provide backwards compatibility with the old EventEmitter where the native module object was passed as an argument.
    // We're checking if the argument is already an instance of the new emitter and if so, just return it without unnecessarily wrapping it.
    if (count > 0) {
      jsi::Object firstArg = args[0].asObject(runtime);
      jsi::Function constructor = thisValue.asObject(runtime).getPropertyAsFunction(runtime, "constructor");

      if (firstArg.instanceOf(runtime, constructor)) {
        return jsi::Value(runtime, args[0]);
      }
    }
    return jsi::Value(runtime, thisValue);
  });
  jsi::Object prototype = eventEmitterClass.getPropertyAsObject(runtime, "prototype");

  jsi::HostFunctionType addListenerHost = [](jsi::Runtime &runtime, const jsi::Value &thisValue, const jsi::Value *args, size_t count) -> jsi::Value {
    std::string eventName = args[0].asString(runtime).utf8(runtime);
    jsi::Function listener = args[1].asObject(runtime).asFunction(runtime);

    // `this` might be an object that is representing a host object, in which case it's not possible to get the native state.
    // For native modules we need to unwrap it to get the object used under the hood by `LazyObject` host object.
    const jsi::Object &thisObject = LazyObject::unwrapObjectIfNecessary(runtime, thisValue.getObject(runtime));

    addListener(runtime, thisObject, eventName, listener);
    return createEventSubscription(runtime, eventName, thisObject, listener);
  };

  jsi::HostFunctionType removeListenerHost = [](jsi::Runtime &runtime, const jsi::Value &thisValue, const jsi::Value *args, size_t count) -> jsi::Value {
    std::string eventName = args[0].asString(runtime).utf8(runtime);
    jsi::Function listener = args[1].asObject(runtime).asFunction(runtime);

    // Unwrap `this` object if it's a lazy object (e.g. native module).
    const jsi::Object &thisObject = LazyObject::unwrapObjectIfNecessary(runtime, thisValue.getObject(runtime));

    removeListener(runtime, thisObject, eventName, listener);
    return jsi::Value::undefined();
  };

  jsi::HostFunctionType removeAllListenersHost = [](jsi::Runtime &runtime, const jsi::Value &thisValue, const jsi::Value *args, size_t count) -> jsi::Value {
    std::string eventName = args[0].asString(runtime).utf8(runtime);

    // Unwrap `this` object if it's a lazy object (e.g. native module).
    const jsi::Object &thisObject = LazyObject::unwrapObjectIfNecessary(runtime, thisValue.getObject(runtime));

    removeAllListeners(runtime, thisObject, eventName);
    return jsi::Value::undefined();
  };

  jsi::HostFunctionType emit = [](jsi::Runtime &runtime, const jsi::Value &thisValue, const jsi::Value *args, size_t count) -> jsi::Value {
    std::string eventName = args[0].asString(runtime).utf8(runtime);

    // Unwrap `this` object if it's a lazy object (e.g. native module).
    const jsi::Object &thisObject = thisValue.getObject(runtime);

    // Make a new pointer that skips the first argument which is the event name.
    const jsi::Value *eventArgs = count > 1 ? &args[1] : nullptr;

    emitEvent(runtime, thisObject, eventName, eventArgs, count - 1);
    return jsi::Value::undefined();
  };

  jsi::PropNameID addListenerProp = jsi::PropNameID::forAscii(runtime, "addListener", 11);
  jsi::PropNameID removeListenerProp = jsi::PropNameID::forAscii(runtime, "removeListener", 14);
  jsi::PropNameID removeAllListenersProp = jsi::PropNameID::forAscii(runtime, "removeAllListeners", 18);
  jsi::PropNameID emitProp = jsi::PropNameID::forAscii(runtime, "emit", 4);

  prototype.setProperty(runtime, addListenerProp, jsi::Function::createFromHostFunction(runtime, addListenerProp, 2, addListenerHost));
  prototype.setProperty(runtime, removeListenerProp, jsi::Function::createFromHostFunction(runtime, removeListenerProp, 2, removeListenerHost));
  prototype.setProperty(runtime, removeAllListenersProp, jsi::Function::createFromHostFunction(runtime, removeAllListenersProp, 1, removeAllListenersHost));
  prototype.setProperty(runtime, emitProp, jsi::Function::createFromHostFunction(runtime, emitProp, 2, emit));

  common::getCoreObject(runtime)
    .setProperty(runtime, "EventEmitter", eventEmitterClass);
}

} // namespace expo::EventEmitter
