// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "common.h"
#include "internal.h"
#include "queue.h"
#include <workerd/jsg/function.h>
#include <workerd/jsg/buffersource.h>

namespace workerd::api {

// =======================================================================================
// jscontroller, ReadableStreamJsController, WritableStreamJsController, and the rest in
// this section define the implementation of JavaScript-backed ReadableStream and WritableStreams.
//
// A JavaScript-backed ReadableStream is backed by a ReadableStreamJsController that is either
// Closed, Errored, or in a Readable state. When readable, the controller owns either a
// ReadableStreamDefaultController or ReadableByteStreamController object that corresponds
// to the identically named interfaces in the streams spec. These objects are responsible
// for the bulk of the implementation detail, with the ReadableStreamJsController serving
// only as a bridge between it and the ReadableStream object itself.
//
//  * ReadableStream -> ReadableStreamJsController -> jsg::Ref<ReadableStreamDefaultController>
//  * ReadableStream -> ReadableStreamJsController -> jsg::Ref<ReadableByteStreamController>
//
// Contrast this with the implementation of internal streams using the
// ReadableStreamInternalController:
//
//  * ReadableStream -> ReadableStreamInternalController -> IoOwn<ReadableStreamSource>
//
// When user-code creates a JavaScript-backed ReadableStream using the `ReadableStream`
// object constructor, they pass along an object called an "underlying source" that provides
// JavaScript functions the ReadableStream will call to either initialize, close, or source
// the data for the stream:
//
//   const readable = new ReadableStream({
//     async start(controller) {
//       // Initialize the stream
//     },
//     async pull(controller) {
//       // Provide the stream data
//     },
//     async cancel(reason) {
//       // Cancel and de-initialize the stream
//     }
//   });
//
// By default, a JavaScript-backed ReadableStream is value-oriented -- that is, any JavaScript
// type can be passed through the stream. It is not limited to bytes only. The implementation
// of the pull method on the underlying source can push strings, booleans, numbers, even undefined
// as values that can be read from the stream. In such streams, the `controller` used internally
// (and owned by the ReadableStreamJsController) is the `ReadableStreamDefaultController`.
//
// To create a byte-oriented stream -- one that is capable only of working with bytes in the
// form of ArrayBufferViews (e.g. `Uint8Array`, `Uint16Array`, `DataView`, etc), the underlying
// source object passed into the `ReadableStream` constructor must have a property
// `'type' = 'bytes'`.
//
//   const readable = new ReadableStream({
//     type: 'bytes',
//     async start(controller) {
//       // Initialize the stream
//     },
//     async pull(controller) {
//       // Provide the stream data
//     },
//     async cancel(reason) {
//       // Cancel and de-initialize the stream
//     }
//   });
//
// From here on, we'll refer to these as either value streams or byte streams. And we'll refer to
// ReadableStreamDefaultController as simply "DefaultController", and ReadableByteStreamController
// as simply "ByobController".
//
// The DefaultController and ByobController each maintain an internal queue. When a read request
// is received, if there is enough data in the internal queue to fulfill the read request, then
// we do so. Otherwise, the controller will call the underlying source's pull method to ask it
// to provide data to fulfill the read request.
//
// A critical aspect of the implementation here is that for JavaScript-backed streams, the entire
// implementation never leaves the isolate lock, and we use JavaScript promises (via jsg::Promise)
// instead of kj::Promise's to keep the implementation from having to bounce back and forth between
// the two spaces. This means that with a JavaScript-backed ReadableStream, it is possible to read
// and fully consume the stream entirely from within JavaScript without ever engaging the kj event
// loop.
//
// When you tee() a JavaScript-backed ReadableStream, the stream is put into a locked state and
// the data is funneled out through two separate "branches" (two new `ReadableStream`s).
//
// When anything reads from a tee branch, the underlying controller is asked to read from the
// underlying source. When the underlying source responds to that read request, the
// data is forwarded to all of the known branches.
//
// The story for JavaScript-backed writable streams is similar. User code passes what the
// spec calls an "underlying sink" to the `WritableStream` object constructor. This provides
// functions that are used to receive stream data.
//
// const writable = new WritableStream({
//   async start(controller) {
//     // initialize
//   },
//   async write(chunk, controller) {
//     // process the written chunk
//   },
//   async abort(reason) {},
//   async close(reason) {},
// });
//
// It is important to note that JavaScript-backed WritableStream's are *always* value
// oriented. It is up to the implementation of the underlying sink to determine if it is
// capable of doing anything with whatever type of chunk it is given.
//
// JavaScript-backed WritableStreams are backed by the WritableStreamJsController and
// WritableStreamDefaultController objects:
//
//  WritableStream -> WritableStreamJsController -> jsg::Ref<WritableStreamDefaultController>
//
// The WritableStreamJsController implements both the WritableStreamController interface
// (same API that is implemented by WritableStreamInternalController) and the
// jscontroller::WriterOwner API.
//
// All write operations on a JavaScript-backed WritableStream are processed within the
// isolate lock using JavaScript promises instead of kj::Promises.

class ReadableStreamJsController;
class WritableStreamJsController;

namespace jscontroller {
// The jscontroller namespace defines declarations that are common to all of the the
// JavaScript-backed ReadableStream and WritableStream variants.

// =======================================================================================
// The Unlocked, Locked, ReaderLocked, and WriterLocked structs
// are used to track the current lock status of JavaScript-backed streams.
// All readable and writable streams begin in the Unlocked state. When a
// reader or writer are attached, the streams will transition into the
// ReaderLocked or WriterLocked state. When the reader is released, those
// will transition back to Unlocked.
//
// When a readable is piped to a writable, both will enter the PipeLocked state.
// (PipeLocked is defined within the ReadableLockImpl and WritableLockImpl classes
// below) When the pipe completes, both will transition back to Unlocked.
//
// When a ReadableStreamJsController is tee()'d, it will enter the locked state.

template <typename Controller>
class ReadableLockImpl {
  // A utility class used by ReadableStreamJsController
  // for implementing the reader lock in a consistent way (without duplicating any code).
public:
  using PipeController = ReadableStreamController::PipeController;
  using Reader = ReadableStreamController::Reader;

  bool isLockedToReader() const { return !state.template is<Unlocked>(); }

  bool lockReader(jsg::Lock& js, Controller& self, Reader& reader);

  void releaseReader(Controller& self, Reader& reader, kj::Maybe<jsg::Lock&> maybeJs);
  // See the comment for releaseReader in common.h for details on the use of maybeJs

  bool lock();

  void onClose();
  void onError(jsg::Lock& js, v8::Local<v8::Value> reason);

  kj::Maybe<PipeController&> tryPipeLock(
        Controller& self,
        jsg::Ref<WritableStream> destination);

  void visitForGc(jsg::GcVisitor& visitor);

private:
  class PipeLocked: public PipeController {
  public:
    explicit PipeLocked(Controller& inner, jsg::Ref<WritableStream> ref)
        : inner(inner), writableStreamRef(kj::mv(ref)) {}

    bool isClosed() override { return inner.state.template is<StreamStates::Closed>(); }

    kj::Maybe<v8::Local<v8::Value>> tryGetErrored(jsg::Lock& js) override {
      KJ_IF_MAYBE(errored, inner.state.template tryGet<StreamStates::Errored>()) {
        return errored->getHandle(js);
      }
      return nullptr;
    }

    void cancel(jsg::Lock& js, v8::Local<v8::Value> reason) override {
      // Cancel here returns a Promise but we do not need to propagate it.
      // We can safely drop it on the floor here.
      auto promise KJ_UNUSED = inner.cancel(js, reason);
    }

    void close() override {
      inner.doClose();
    }

    void error(jsg::Lock& js, v8::Local<v8::Value> reason) override {
      inner.doError(js, reason);
    }

    void release(jsg::Lock& js,
                 kj::Maybe<v8::Local<v8::Value>> maybeError = nullptr) override {
      KJ_IF_MAYBE(error, maybeError) {
        cancel(js, *error);
      }
      inner.lock.state.template init<Unlocked>();
    }

    kj::Maybe<kj::Promise<void>> tryPumpTo(WritableStreamSink& sink, bool end) override;

    jsg::Promise<ReadResult> read(jsg::Lock& js) override;

    void visitForGc(jsg::GcVisitor& visitor) ;

  private:
    Controller& inner;
    jsg::Ref<WritableStream> writableStreamRef;

    friend Controller;
  };

  kj::OneOf<Locked, PipeLocked, ReaderLocked, Unlocked> state = Unlocked();
  friend Controller;
};

template <typename Controller>
class WritableLockImpl {
  // A utility class used by WritableStreamJsController to implement the writer lock
  // mechanism. Extracted for consistency with ReadableStreamJsController and to
  // eventually allow it to be shared also with WritableStreamInternalController.
public:
  using Writer = WritableStreamController::Writer;

  bool isLockedToWriter() const;

  bool lockWriter(jsg::Lock& js, Controller& self, Writer& writer);

  void releaseWriter(Controller& self, Writer& writer, kj::Maybe<jsg::Lock&> maybeJs);
  // See the comment for releaseWriter in common.h for details on the use of maybeJs

  void visitForGc(jsg::GcVisitor& visitor);

  bool pipeLock(WritableStream& owner,
                jsg::Ref<ReadableStream> source,
                PipeToOptions& options);
  void releasePipeLock();

private:
  struct PipeLocked {
    ReadableStreamController::PipeController& source;
    jsg::Ref<ReadableStream> readableStreamRef;
    bool preventAbort;
    bool preventCancel;
    bool preventClose;
    bool pipeThrough;
    kj::Maybe<jsg::Ref<AbortSignal>> maybeSignal;

    kj::Maybe<jsg::Promise<void>> checkSignal(
        jsg::Lock& js,
        Controller& self);
  };
  kj::OneOf<Unlocked, Locked, WriterLocked, PipeLocked> state = Unlocked();

  inline kj::Maybe<PipeLocked&> tryGetPipe() {
    KJ_IF_MAYBE(locked, state.template tryGet<PipeLocked>()) {
      return *locked;
    }
    return nullptr;
  }

  friend Controller;
};

// =======================================================================================
class WriterOwner {
  // The WriterOwner is the current owner of a WritableStreamDefaultcontroller.
  // Currently, this can only be a WritableStreamJsController.
  // The WriterOwner interface allows the underlying controller to communicate
  // status updates up to the current owner without caring about what kind of thing
  // the owner currently is.
public:
  virtual void doClose() = 0;
  // Communicate to the owner that the stream has been closed. The owner should release
  // ownership of the underlying controller and allow it to be garbage collected as soon
  // as possible.

  virtual void doError(jsg::Lock& js, v8::Local<v8::Value> reason) = 0;
  // Communicate to the owner that the stream has been errored. The owner should remember
  // the error reason, and release ownership of the underlying controller and allow it to
  // be garbage collected as soon as possible.

  virtual bool isLocked() const = 0;

  virtual void updateBackpressure(jsg::Lock& js, bool backpressure) = 0;
  virtual void maybeResolveReadyPromise() = 0;
  virtual void maybeRejectReadyPromise(jsg::Lock& js, v8::Local<v8::Value> reason) = 0;
};

// =======================================================================================
template <class Self>
class ReadableImpl {
  // The ReadableImpl provides implementation that is common to both the
  // ReadableStreamDefaultController and the ReadableByteStreamController.
public:
  using Consumer = typename Self::QueueType::Consumer;
  using Entry = typename Self::QueueType::Entry;
  using StateListener = typename Self::QueueType::ConsumerImpl::StateListener;

  ReadableImpl(UnderlyingSource underlyingSource,
               StreamQueuingStrategy queuingStrategy);

  void start(jsg::Lock& js, jsg::Ref<Self> self);

  jsg::Promise<void> cancel(jsg::Lock& js,
                             jsg::Ref<Self> self,
                             v8::Local<v8::Value> maybeReason);

  bool canCloseOrEnqueue();

  void doCancel(jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason);

  void close(jsg::Lock& js);

  void enqueue(jsg::Lock& js, kj::Own<Entry> entry, jsg::Ref<Self> self);

  void doClose(jsg::Lock& js);

  void doError(jsg::Lock& js, jsg::Value reason);

  kj::Maybe<int> getDesiredSize();

  void pullIfNeeded(jsg::Lock& js, jsg::Ref<Self> self);

  bool hasPendingReadRequests();

  bool shouldCallPull();

  kj::Own<Consumer> getConsumer(kj::Maybe<StateListener&> listener);

  void visitForGc(jsg::GcVisitor& visitor);

  size_t consumerCount();

private:
  struct Algorithms {
    kj::Maybe<jsg::Promise<void>> starting;
    kj::Maybe<jsg::Promise<void>> pulling;
    kj::Maybe<jsg::Promise<void>> canceling;

    kj::Maybe<jsg::Function<UnderlyingSource::StartAlgorithm>> start;
    kj::Maybe<jsg::Function<UnderlyingSource::PullAlgorithm>> pull;
    kj::Maybe<jsg::Function<UnderlyingSource::CancelAlgorithm>> cancel;
    kj::Maybe<jsg::Function<StreamQueuingStrategy::SizeAlgorithm>> size;

    Algorithms(UnderlyingSource underlyingSource, StreamQueuingStrategy queuingStrategy)
        : start(kj::mv(underlyingSource.start)),
          pull(kj::mv(underlyingSource.pull)),
          cancel(kj::mv(underlyingSource.cancel)),
          size(kj::mv(queuingStrategy.size)) {}

    Algorithms(Algorithms&& other) = default;
    Algorithms& operator=(Algorithms&& other) = default;

    void clear() {
      starting = nullptr;
      pulling = nullptr;
      canceling = nullptr;
      start = nullptr;
      pull = nullptr;
      cancel = nullptr;
      size = nullptr;
    }

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(starting, pulling, canceling, start, pull, cancel, size);
    }
  };

  using Queue = typename Self::QueueType;

  kj::OneOf<StreamStates::Closed, StreamStates::Errored, Queue> state;
  Algorithms algorithms;

  bool closeRequested = false;
  bool disturbed = false;
  bool pullAgain = false;
  bool pulling = false;
  bool started = false;
  size_t highWaterMark = 1;

  struct PendingCancel {
    kj::Maybe<jsg::Promise<void>::Resolver> fulfiller;
    jsg::Promise<void> promise;
  };
  kj::Maybe<PendingCancel> maybePendingCancel;

  friend Self;
};

template <class Self>
class WritableImpl {
  // Utility that provides the core implementation of WritableStreamJsController,
  // separated out for consistency with ReadableStreamJsController/ReadableImpl and
  // to enable it to be more easily reused should new kinds of WritableStream
  // controllers be introduced.
public:
  using PendingAbort = WritableStreamController::PendingAbort;

  struct WriteRequest {
    jsg::Promise<void>::Resolver resolver;
    jsg::Value value;
    size_t size;

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(resolver, value);
    }
  };

  WritableImpl(WriterOwner& owner);

  jsg::Promise<void> abort(jsg::Lock& js,
                            jsg::Ref<Self> self,
                            v8::Local<v8::Value> reason);

  void advanceQueueIfNeeded(jsg::Lock& js, jsg::Ref<Self> self);

  jsg::Promise<void> close(jsg::Lock& js, jsg::Ref<Self> self);

  void dealWithRejection(jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason);

  WriteRequest dequeueWriteRequest();

  void doClose();

  void doError(jsg::Lock& js, v8::Local<v8::Value> reason);

  void error(jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason);

  void finishErroring(jsg::Lock& js, jsg::Ref<Self> self);

  void finishInFlightClose(
      jsg::Lock& js,
      jsg::Ref<Self> self,
      kj::Maybe<v8::Local<v8::Value>> reason = nullptr);

  void finishInFlightWrite(
      jsg::Lock& js,
      jsg::Ref<Self> self,
      kj::Maybe<v8::Local<v8::Value>> reason = nullptr);

  ssize_t getDesiredSize();

  bool isCloseQueuedOrInFlight();

  void rejectCloseAndClosedPromiseIfNeeded(jsg::Lock& js);

  void setOwner(kj::Maybe<WriterOwner&> owner) {
    this->owner = owner;
  }

  WriterOwner& getOwner() {
    return JSG_REQUIRE_NONNULL(owner, TypeError, "This stream has been closed.");
  }

  void setup(
      jsg::Lock& js,
      jsg::Ref<Self> self,
      UnderlyingSink underlyingSink,
      StreamQueuingStrategy queuingStrategy);

  void startErroring(jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason);

  void updateBackpressure(jsg::Lock& js);

  jsg::Promise<void> write(jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> value);

  void visitForGc(jsg::GcVisitor& visitor);

  bool isWritable() const;

private:

  struct Algorithms {
    kj::Maybe<jsg::Promise<void>> aborting;
    kj::Maybe<jsg::Promise<void>> closing;
    kj::Maybe<jsg::Promise<void>> starting;
    kj::Maybe<jsg::Promise<void>> writing;

    kj::Maybe<jsg::Function<UnderlyingSink::AbortAlgorithm>> abort;
    kj::Maybe<jsg::Function<UnderlyingSink::CloseAlgorithm>> close;
    kj::Maybe<jsg::Function<UnderlyingSink::WriteAlgorithm>> write;
    kj::Maybe<jsg::Function<StreamQueuingStrategy::SizeAlgorithm>> size;

    Algorithms() {};
    Algorithms(Algorithms&& other) = default;
    Algorithms& operator=(Algorithms&& other) = default;

    void clear() {
      aborting = nullptr;
      closing = nullptr;
      starting = nullptr;
      writing = nullptr;
      abort = nullptr;
      close = nullptr;
      size = nullptr;
      write = nullptr;
    }

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(starting, aborting, closing, writing, write, close, abort, size);
    }
  };

  struct Writable {};

  kj::Maybe<WriterOwner&> owner;
  jsg::Ref<AbortSignal> signal;
  kj::OneOf<StreamStates::Closed,
            StreamStates::Errored,
            StreamStates::Erroring,
            Writable> state = Writable();
  Algorithms algorithms;
  bool started = false;
  bool backpressure = false;
  size_t highWaterMark = 1;

  std::deque<WriteRequest> writeRequests;
  size_t amountBuffered = 0;

  kj::Maybe<WriteRequest> inFlightWrite;
  kj::Maybe<jsg::Promise<void>::Resolver> inFlightClose;
  kj::Maybe<jsg::Promise<void>::Resolver> closeRequest;
  kj::Maybe<PendingAbort> maybePendingAbort;

  friend Self;
};

}  // namespace jscontroller

template <typename T>
class WeakRef: public kj::Refcounted {
  // Used to allow the TransformStreamDefaultController to hold safe
  // weak refs to the ReadableStreamDefaultController and WritableStreamJsController.
public:
  WeakRef(T& ref) : ref(ref) {}
  KJ_DISALLOW_COPY_AND_MOVE(WeakRef);
  kj::Maybe<T&> tryGet() { return ref; }
  kj::Own<WeakRef> addRef() { return kj::addRef(*this); }
private:
  void reset() { ref = nullptr; }
  kj::Maybe<T&> ref;
  friend T;
};

// =======================================================================================

class ReadableStreamDefaultController: public jsg::Object {
  // ReadableStreamDefaultController is a JavaScript object defined by the streams specification.
  // It is capable of streaming any JavaScript value through it, including typed arrays and
  // array buffers, but treats all values as opaque. BYOB reads are not supported.
public:
  using QueueType = ValueQueue;
  using ReadableImpl = jscontroller::ReadableImpl<ReadableStreamDefaultController>;

  ReadableStreamDefaultController(UnderlyingSource underlyingSource,
                                  StreamQueuingStrategy queuingStrategy);
  ~ReadableStreamDefaultController() noexcept(false) { weakRef->reset(); }

  void start(jsg::Lock& js);

  jsg::Promise<void> cancel(jsg::Lock& js,
                            jsg::Optional<v8::Local<v8::Value>> maybeReason);

  void close(jsg::Lock& js);

  bool canCloseOrEnqueue();
  bool hasBackpressure();
  kj::Maybe<int> getDesiredSize();
  bool hasPendingReadRequests();

  void enqueue(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> chunk);

  void error(jsg::Lock& js, v8::Local<v8::Value> reason);

  void pull(jsg::Lock& js);

  kj::Own<ValueQueue::Consumer> getConsumer(
      kj::Maybe<ValueQueue::ConsumerImpl::StateListener&> stateListener);

  JSG_RESOURCE_TYPE(ReadableStreamDefaultController) {
    JSG_READONLY_INSTANCE_PROPERTY(desiredSize, getDesiredSize);
    JSG_METHOD(close);
    JSG_METHOD(enqueue);
    JSG_METHOD(error);

    JSG_TS_OVERRIDE(<R = any> {
      enqueue(chunk?: R): void;
    });
  }

  kj::Own<WeakRef<ReadableStreamDefaultController>> getWeakRef() {
    return kj::addRef(*weakRef);
  }

private:
  kj::Maybe<IoContext&> ioContext;
  ReadableImpl impl;
  kj::Own<WeakRef<ReadableStreamDefaultController>> weakRef;

  void visitForGc(jsg::GcVisitor& visitor);
};

class ReadableStreamBYOBRequest: public jsg::Object {
  // The ReadableStreamBYOBRequest is provided by the ReadableByteStreamController
  // and is used by user code to fill a view provided by a BYOB read request.
  // Because we always support autoAllocateChunkSize in the ReadableByteStreamController,
  // there will always be a ReadableStreamBYOBRequest available when there is a pending
  // read.
  //
  // The ReadableStreamBYOBRequest is either in an attached or detached state.
  // The request is detached when invalidate() is called. Attempts to use the request
  // after it has been detached will fail.
  //
  // Note that the casing of the name (e.g. "BYOB" instead of the kj style "Byob") is
  // dictated by the streams specification since the class name is used as the exported
  // object name.
public:
  ReadableStreamBYOBRequest(
      jsg::Lock& js,
      kj::Own<ByteQueue::ByobRequest> readRequest,
      jsg::Ref<ReadableByteStreamController> controller);

  KJ_DISALLOW_COPY_AND_MOVE(ReadableStreamBYOBRequest);

  kj::Maybe<int> getAtLeast();
  // getAtLeast is a non-standard Workers-specific extension that specifies
  // the minimum number of bytes the stream should fill into the view. It is
  // added to support the readAtLeast extension on the ReadableStreamBYOBReader.

  kj::Maybe<jsg::V8Ref<v8::Uint8Array>> getView(jsg::Lock& js);

  void invalidate(jsg::Lock& js);

  void respond(jsg::Lock& js, int bytesWritten);

  void respondWithNewView(jsg::Lock& js, jsg::BufferSource view);

  JSG_RESOURCE_TYPE(ReadableStreamBYOBRequest) {
    JSG_READONLY_INSTANCE_PROPERTY(view, getView);
    JSG_METHOD(respond);
    JSG_METHOD(respondWithNewView);

    JSG_READONLY_INSTANCE_PROPERTY(atLeast, getAtLeast);
    // atLeast is an Workers-specific extension used to support the
    // readAtLeast API.
  }

  bool isPartiallyFulfilled();

private:
  struct Impl {
    kj::Own<ByteQueue::ByobRequest> readRequest;
    jsg::Ref<ReadableByteStreamController> controller;
    jsg::V8Ref<v8::Uint8Array> view;

    Impl(jsg::Lock& js,
         kj::Own<ByteQueue::ByobRequest> readRequest,
         jsg::Ref<ReadableByteStreamController> controller)
         : readRequest(kj::mv(readRequest)),
           controller(kj::mv(controller)),
           view(js.v8Ref(this->readRequest->getView(js))) {}

    void updateView(jsg::Lock& js);
  };

  kj::Maybe<IoContext&> ioContext;
  kj::Maybe<Impl> maybeImpl;

  void visitForGc(jsg::GcVisitor& visitor);
};

class ReadableByteStreamController: public jsg::Object {
  // ReadableByteStreamController is a JavaScript object defined by the streams specification.
  // It is capable of only streaming byte data through it in the form of typed arrays.
  // BYOB reads are supported.
public:
  using QueueType = ByteQueue;
  using ReadableImpl = jscontroller::ReadableImpl<ReadableByteStreamController>;

  ReadableByteStreamController(UnderlyingSource underlyingSource,
                               StreamQueuingStrategy queuingStrategy);

  void start(jsg::Lock& js);

  jsg::Promise<void> cancel(jsg::Lock& js,
                             jsg::Optional<v8::Local<v8::Value>> maybeReason);

  void close(jsg::Lock& js);

  void enqueue(jsg::Lock& js, jsg::BufferSource chunk);

  void error(jsg::Lock& js, v8::Local<v8::Value> reason);

  bool canCloseOrEnqueue();
  bool hasBackpressure();
  kj::Maybe<int> getDesiredSize();
  bool hasPendingReadRequests();

  kj::Maybe<jsg::Ref<ReadableStreamBYOBRequest>> getByobRequest(jsg::Lock& js);

  void pull(jsg::Lock& js);

  kj::Own<ByteQueue::Consumer> getConsumer(
      kj::Maybe<ByteQueue::ConsumerImpl::StateListener&> stateListener);

  JSG_RESOURCE_TYPE(ReadableByteStreamController) {
    JSG_READONLY_INSTANCE_PROPERTY(byobRequest, getByobRequest);
    JSG_READONLY_INSTANCE_PROPERTY(desiredSize, getDesiredSize);
    JSG_METHOD(close);
    JSG_METHOD(enqueue);
    JSG_METHOD(error);
  }

private:
  kj::Maybe<IoContext&> ioContext;
  ReadableImpl impl;
  kj::Maybe<jsg::Ref<ReadableStreamBYOBRequest>> maybeByobRequest;

  void visitForGc(jsg::GcVisitor& visitor);

  friend class ReadableStreamBYOBRequest;
  friend class ReadableStreamJsController;
};

// =======================================================================================

class WritableStreamDefaultController: public jsg::Object {
  // The WritableStreamDefaultController is an object defined by the stream specification.
  // Writable streams are always value oriented. It is up the underlying sink implementation
  // to determine whether it is capable of handling whatever type of JavaScript object it
  // is given.
public:
  using WritableImpl = jscontroller::WritableImpl<WritableStreamDefaultController>;
  using WriterOwner = jscontroller::WriterOwner;

  explicit WritableStreamDefaultController(WriterOwner& owner);

  jsg::Promise<void> abort(jsg::Lock& js, v8::Local<v8::Value> reason);

  jsg::Promise<void> close(jsg::Lock& js);

  void error(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason);

  ssize_t getDesiredSize();

  jsg::Ref<AbortSignal> getSignal();

  kj::Maybe<v8::Local<v8::Value>> isErroring(jsg::Lock& js);

  bool isStarted() { return impl.started; }

  void setOwner(kj::Maybe<WriterOwner&> owner);

  void setup(
      jsg::Lock& js,
      UnderlyingSink underlyingSink,
      StreamQueuingStrategy queuingStrategy);

  jsg::Promise<void> write(jsg::Lock& js, v8::Local<v8::Value> value);

  JSG_RESOURCE_TYPE(WritableStreamDefaultController) {
    JSG_READONLY_INSTANCE_PROPERTY(signal, getSignal);
    JSG_METHOD(error);
  }

private:
  kj::Maybe<IoContext&> ioContext;
  WritableImpl impl;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(impl);
   }
};

// =======================================================================================

class TransformStreamDefaultController: public jsg::Object {
  // The relationship between the TransformStreamDefaultController and the
  // readable/writable streams associated with it can be complicated.
  // Strong references to the TransformStreamDefaultController are held by
  // the *algorithms* passed into the readable and writable streams using
  // JSG_VISITABLE_LAMBDAs. When those algorithms are cleared, the strong
  // references holding the TransformStreamDefaultController are freed.
  // However, user code can do silly things like hold the Transform controller
  // long after both the readable and writable sides have been gc'd.
  //
  // We do not want to create a strong reference cycle between the various
  // controllers so we use weak refs within the transform controller to
  // safely reference the readable and writable sides. If either side goes
  // away cleanly (using the algorithms) the weak references are cleared.
  // If either side goes away due to garbage collection while the transform
  // controller is still alive, the weak references are cleared. The transform
  // controller then safely handles the disappearance of either side.
public:
  TransformStreamDefaultController(jsg::Lock& js);

  void init(jsg::Lock& js,
            jsg::Ref<ReadableStream>& readable,
            jsg::Ref<WritableStream>& writable,
            jsg::Optional<Transformer> maybeTransformer);

  inline jsg::Promise<void> getStartPromise() {
    // The startPromise is used by both the readable and writable sides in their respective
    // start algorithms. The promise itself is resolved within the init function when the
    // transformers own start algorithm completes.
    return startPromise.promise.whenResolved();
  }

  kj::Maybe<int> getDesiredSize();

  void enqueue(jsg::Lock& js, v8::Local<v8::Value> chunk);

  void error(jsg::Lock& js, v8::Local<v8::Value> reason);

  void terminate(jsg::Lock& js);

  JSG_RESOURCE_TYPE(TransformStreamDefaultController) {
    JSG_READONLY_PROTOTYPE_PROPERTY(desiredSize, getDesiredSize);
    JSG_METHOD(enqueue);
    JSG_METHOD(error);
    JSG_METHOD(terminate);

    JSG_TS_OVERRIDE(<O = any> {
      enqueue(chunk?: O): void;
    });
  }

  jsg::Promise<void> write(jsg::Lock& js, v8::Local<v8::Value> chunk);
  jsg::Promise<void> abort(jsg::Lock& js, v8::Local<v8::Value> reason);
  jsg::Promise<void> close(jsg::Lock& js);
  jsg::Promise<void> pull(jsg::Lock& js);
  jsg::Promise<void> cancel(jsg::Lock& js, v8::Local<v8::Value> reason);

private:
  struct Algorithms {
    kj::Maybe<jsg::Promise<void>> starting;

    kj::Maybe<jsg::Function<Transformer::TransformAlgorithm>> transform;
    kj::Maybe<jsg::Function<Transformer::FlushAlgorithm>> flush;

    Algorithms() {};
    Algorithms(Algorithms&& other) = default;
    Algorithms& operator=(Algorithms&& other) = default;

    inline void clear() {
      starting = nullptr;
      transform = nullptr;
      flush = nullptr;
    }

    inline void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(starting, transform, flush);
    }
  };

  void errorWritableAndUnblockWrite(jsg::Lock& js,
                                    v8::Local<v8::Value> reason);
  jsg::Promise<void> performTransform(jsg::Lock& js,
                                       v8::Local<v8::Value> chunk);
  void setBackpressure(jsg::Lock& js, bool newBackpressure);

  kj::Maybe<IoContext&> ioContext;
  jsg::PromiseResolverPair<void> startPromise;

  kj::Maybe<ReadableStreamDefaultController&> tryGetReadableController();
  kj::Maybe<WritableStreamJsController&> tryGetWritableController();

  kj::Maybe<kj::Own<WeakRef<ReadableStreamDefaultController>>> maybeReadableController;
  kj::Maybe<kj::Own<WeakRef<WritableStreamJsController>>> maybeWritableController;
  Algorithms algorithms;
  bool backpressure = false;
  kj::Maybe<jsg::PromiseResolverPair<void>> maybeBackpressureChange;

  void visitForGc(jsg::GcVisitor& visitor);
};

}  // namespace workerd::api
