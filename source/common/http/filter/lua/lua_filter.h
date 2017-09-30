#pragma once

#include "envoy/http/filter.h"
#include "envoy/upstream/cluster_manager.h"

#include "common/http/filter/lua/wrappers.h"
#include "common/lua/wrappers.h"

namespace Envoy {
namespace Http {
namespace Filter {
namespace Lua {

/**
 * Callbacks used by a a strem handler to access the filter.
 */
class FilterCallbacks {
public:
  virtual ~FilterCallbacks() {}

  /**
   * Add data to the connection manager buffer.
   * @param data supplies the data to add.
   */
  virtual void addData(Buffer::Instance& data) PURE;

  /**
   * @return const Buffer::Instance* the currently buffered body.
   */
  virtual const Buffer::Instance* bufferedBody() PURE;

  /**
   * Continue filter iteration if iteration has been paused due to an async call.
   */
  virtual void continueIteration() PURE;

  /**
   * Perform an immediate response.
   * @param headers supplies the response headers.
   * @param body supplies the optional response body.
   * @param state supplies the active Lua state.
   */
  virtual void respond(HeaderMapPtr&& headers, Buffer::Instance* body, lua_State* state) PURE;
};

class Filter;

/**
 * A wrapper for a currently running request/response. This is the primary handle passed to Lua.
 * The script interacts with Envoy entirely through this handle.
 */
class StreamHandleWrapper : public Envoy::Lua::BaseLuaObject<StreamHandleWrapper>,
                            public AsyncClient::Callbacks {
public:
  enum class State { Running, WaitForBodyChunk, WaitForBody, WaitForTrailers, HttpCall, Responded };

  StreamHandleWrapper(Envoy::Lua::CoroutinePtr&& coroutine, HeaderMap& headers, bool end_stream,
                      Filter& filter, FilterCallbacks& callbacks);

  FilterHeadersStatus start(int function_ref);
  FilterDataStatus onData(Buffer::Instance& data, bool end_stream);
  FilterTrailersStatus onTrailers(HeaderMap& trailers);

  void onReset() {
    if (http_request_) {
      http_request_->cancel();
      http_request_ = nullptr;
    }
  }

  static ExportedFunctions exportedFunctions() {
    return {{"headers", static_luaHeaders},
            {"body", static_luaBody},
            {"bodyChunks", static_luaBodyChunks},
            {"trailers", static_luaTrailers},
            {"log", static_luaLog},
            {"httpCall", static_luaHttpCall},
            {"respond", static_luaRespond}};
  }

private:
  /**
   * Perform an HTTP call to an upstream host.
   * @param 1 (string): The name of the upstream cluster to call. This cluster must be configured.
   * @param 2 (table): A table of HTTP headers. :method, :path, and :authority must be defined.
   * @param 3 (string): Body. Can be nil.
   * @param 4 (int): Timeout in milliseconds for the call.
   * @return headers (table), body (string/nil)
   */
  DECLARE_LUA_FUNCTION(StreamHandleWrapper, luaHttpCall);

  /**
   * Perform an inline response. This call is currently only valid on the request path. Further
   * filter iteration will stop. No further script code will run after this call.
   * @param 1 (table): A table of HTTP headers. :status must be defined.
   * @param 2 (string): Body. Can be nil.
   */
  DECLARE_LUA_FUNCTION(StreamHandleWrapper, luaRespond);

  /**
   * @return a handle to the headers.
   */
  DECLARE_LUA_FUNCTION(StreamHandleWrapper, luaHeaders);

  /**
   * @return a handle to the full body or nil if there is no body. This call will cause the script
   *         to yield until the entire body is received (or if there is no body will return nil
   *         right away).
   *         NOTE: This call causes Envoy to buffer the body. The max buffer size is configured
   *         based on the currently active flow control settings.
   */
  DECLARE_LUA_FUNCTION(StreamHandleWrapper, luaBody);

  /**
   * @return an iterator that allows the script to iterate through all body chunks as they are
   *         received. The iterator will yield between body chunks. Envoy *will not* buffer
   *         the body chunks in this case, but the script can look at them as they go by.
   */
  DECLARE_LUA_FUNCTION(StreamHandleWrapper, luaBodyChunks);

  /**
   * @return a handle to the trailers or nil if there are no trailers. This call will cause the
   *         script to yield of Envoy does not yet know if there are trailers or not.
   */
  DECLARE_LUA_FUNCTION(StreamHandleWrapper, luaTrailers);

  /**
   * Log a message to the Envoy log.
   * @param 1 (int): The log level.
   * @param 2 (string): The log message.
   */
  DECLARE_LUA_FUNCTION(StreamHandleWrapper, luaLog);

  /**
   * This is the closure/iterator returned by luaBodyChunks() above.
   */
  DECLARE_LUA_CLOSURE(StreamHandleWrapper, luaBodyIterator);

  static HeaderMapPtr buildHeadersFromTable(lua_State* state, int table_index);

  // Envoy::Lua::BaseLuaObject
  void onMarkDead() override {
    headers_wrapper_.markDead();
    body_wrapper_.markDead();
    trailers_wrapper_.markDead();
  }

  void onMarkLive() override {
    headers_wrapper_.markLive();
    body_wrapper_.markLive();
    trailers_wrapper_.markLive();
  }

  // Http::AsyncClient::Callbacks
  void onSuccess(MessagePtr&&) override;
  void onFailure(AsyncClient::FailureReason) override;

  Envoy::Lua::CoroutinePtr coroutine_;
  HeaderMap& headers_;
  bool end_stream_;
  bool headers_continued_{};
  bool buffered_body_{};
  bool saw_body_{};
  Filter& filter_;
  FilterCallbacks& callbacks_;
  HeaderMap* trailers_{};
  Envoy::Lua::LuaDeathRef<HeaderMapWrapper> headers_wrapper_;
  Envoy::Lua::LuaDeathRef<Envoy::Lua::BufferWrapper> body_wrapper_;
  Envoy::Lua::LuaDeathRef<HeaderMapWrapper> trailers_wrapper_;
  State state_{State::Running};
  std::function<void()> yield_callback_;
  AsyncClient::Request* http_request_{};
};

/**
 * Global configuration for the filter.
 */
class FilterConfig : Logger::Loggable<Logger::Id::lua> {
public:
  FilterConfig(const std::string& lua_code, ThreadLocal::SlotAllocator& tls,
               Upstream::ClusterManager& cluster_manager);
  Envoy::Lua::CoroutinePtr createCoroutine() { return lua_state_.createCoroutine(); }
  int requestFunctionRef() { return lua_state_.getGlobalRef(request_function_slot_); }
  int responseFunctionRef() { return lua_state_.getGlobalRef(response_function_slot_); }

  Upstream::ClusterManager& cluster_manager_;

private:
  Envoy::Lua::ThreadLocalState lua_state_;
  uint64_t request_function_slot_;
  uint64_t response_function_slot_;
};

typedef std::shared_ptr<FilterConfig> FilterConfigConstSharedPtr;

// TODO(mattklein123): Filter stats.

/**
 * The HTTP Lua filter. Allows scripts to run in both the request an response flow.
 */
class Filter : public StreamFilter, Logger::Loggable<Logger::Id::lua> {
public:
  Filter(FilterConfigConstSharedPtr config) : config_(config) {}

  Upstream::ClusterManager& clusterManager() { return config_->cluster_manager_; }
  void scriptError(const Envoy::Lua::LuaException& e);
  virtual void scriptLog(int level, const char* message);

  // Http::StreamFilterBase
  void onDestroy() override;

  // Http::StreamDecoderFilter
  FilterHeadersStatus decodeHeaders(HeaderMap& headers, bool end_stream) override {
    return doHeaders(request_stream_wrapper_, decoder_callbacks_, config_->requestFunctionRef(),
                     headers, end_stream);
  }
  FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override {
    return doData(request_stream_wrapper_, data, end_stream);
  }
  FilterTrailersStatus decodeTrailers(HeaderMap& trailers) override {
    return doTrailers(request_stream_wrapper_, trailers);
  }
  void setDecoderFilterCallbacks(StreamDecoderFilterCallbacks& callbacks) override {
    decoder_callbacks_.callbacks_ = &callbacks;
  }

  // Http::StreamEncoderFilter
  FilterHeadersStatus encodeHeaders(HeaderMap& headers, bool end_stream) override {
    return doHeaders(response_stream_wrapper_, encoder_callbacks_, config_->responseFunctionRef(),
                     headers, end_stream);
  }
  FilterDataStatus encodeData(Buffer::Instance& data, bool end_stream) override {
    return doData(response_stream_wrapper_, data, end_stream);
  };
  FilterTrailersStatus encodeTrailers(HeaderMap& trailers) override {
    return doTrailers(response_stream_wrapper_, trailers);
  };
  void setEncoderFilterCallbacks(StreamEncoderFilterCallbacks& callbacks) override {
    encoder_callbacks_.callbacks_ = &callbacks;
  };

private:
  struct DecoderCallbacks : public FilterCallbacks {
    DecoderCallbacks(Filter& parent) : parent_(parent) {}

    // FilterCallbacks
    void addData(Buffer::Instance& data) override {
      return callbacks_->addDecodedData(data, false);
    }
    const Buffer::Instance* bufferedBody() override { return callbacks_->decodingBuffer(); }
    void continueIteration() override { return callbacks_->continueDecoding(); }
    void respond(HeaderMapPtr&& headers, Buffer::Instance* body, lua_State* state) override;

    Filter& parent_;
    StreamDecoderFilterCallbacks* callbacks_{};
  };

  struct EncoderCallbacks : public FilterCallbacks {
    EncoderCallbacks(Filter& parent) : parent_(parent) {}

    // FilterCallbacks
    void addData(Buffer::Instance& data) override {
      return callbacks_->addEncodedData(data, false);
    }
    const Buffer::Instance* bufferedBody() override { return callbacks_->encodingBuffer(); }
    void continueIteration() override { return callbacks_->continueEncoding(); }
    void respond(HeaderMapPtr&& headers, Buffer::Instance* body, lua_State* state) override;

    Filter& parent_;
    StreamEncoderFilterCallbacks* callbacks_{};
  };

  typedef Envoy::Lua::LuaDeathRef<StreamHandleWrapper> StreamHandleRef;

  FilterHeadersStatus doHeaders(StreamHandleRef& handle, FilterCallbacks& callbacks,
                                int function_ref, HeaderMap& headers, bool end_stream);
  FilterDataStatus doData(StreamHandleRef& handle, Buffer::Instance& data, bool end_stream);
  FilterTrailersStatus doTrailers(StreamHandleRef& handle, HeaderMap& trailers);

  FilterConfigConstSharedPtr config_;
  DecoderCallbacks decoder_callbacks_{*this};
  EncoderCallbacks encoder_callbacks_{*this};
  StreamHandleRef request_stream_wrapper_;
  StreamHandleRef response_stream_wrapper_;
  bool destroyed_{};
};

} // namespace Lua
} // namespace Filter
} // namespace Http
} // namespace Envoy
