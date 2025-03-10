/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CanvasChild.h"

#include "MainThreadUtils.h"
#include "mozilla/gfx/CanvasManagerChild.h"
#include "mozilla/gfx/DrawTargetRecording.h"
#include "mozilla/gfx/Tools.h"
#include "mozilla/gfx/Rect.h"
#include "mozilla/gfx/Point.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/ProcessChild.h"
#include "mozilla/layers/CanvasDrawEventRecorder.h"
#include "mozilla/layers/SourceSurfaceSharedData.h"
#include "mozilla/Maybe.h"
#include "nsIObserverService.h"
#include "RecordedCanvasEventImpl.h"

namespace mozilla {
namespace layers {

class RecorderHelpers final : public CanvasDrawEventRecorder::Helpers {
 public:
  explicit RecorderHelpers(const RefPtr<CanvasChild>& aCanvasChild)
      : mCanvasChild(aCanvasChild) {}

  ~RecorderHelpers() override = default;

  bool InitTranslator(const TextureType& aTextureType, Handle&& aReadHandle,
                      nsTArray<Handle>&& aBufferHandles,
                      const uint64_t& aBufferSize,
                      CrossProcessSemaphoreHandle&& aReaderSem,
                      CrossProcessSemaphoreHandle&& aWriterSem,
                      const bool& aUseIPDLThread) override {
    if (!mCanvasChild) {
      return false;
    }
    return mCanvasChild->SendInitTranslator(
        aTextureType, std::move(aReadHandle), std::move(aBufferHandles),
        aBufferSize, std::move(aReaderSem), std::move(aWriterSem),
        aUseIPDLThread);
  }

  bool AddBuffer(Handle&& aBufferHandle, const uint64_t& aBufferSize) override {
    if (!mCanvasChild) {
      return false;
    }
    return mCanvasChild->SendAddBuffer(std::move(aBufferHandle), aBufferSize);
  }

  bool ReaderClosed() override {
    if (!mCanvasChild) {
      return false;
    }
    return !mCanvasChild->CanSend() || ipc::ProcessChild::ExpectingShutdown();
  }

  bool RestartReader() override {
    if (!mCanvasChild) {
      return false;
    }
    return mCanvasChild->SendRestartTranslation();
  }

 private:
  const WeakPtr<CanvasChild> mCanvasChild;
};

class SourceSurfaceCanvasRecording final : public gfx::SourceSurface {
 public:
  MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(SourceSurfaceCanvasRecording, final)

  SourceSurfaceCanvasRecording(
      const RefPtr<gfx::SourceSurface>& aRecordedSuface,
      CanvasChild* aCanvasChild,
      const RefPtr<CanvasDrawEventRecorder>& aRecorder)
      : mRecordedSurface(aRecordedSuface),
        mCanvasChild(aCanvasChild),
        mRecorder(aRecorder) {
    // It's important that AddStoredObject is called first because that will
    // run any pending processing required by recorded objects that have been
    // deleted off the main thread.
    mRecorder->AddStoredObject(this);
    mRecorder->RecordEvent(RecordedAddSurfaceAlias(this, aRecordedSuface));
  }

  ~SourceSurfaceCanvasRecording() {
    ReferencePtr surfaceAlias = this;
    if (NS_IsMainThread()) {
      ReleaseOnMainThread(std::move(mRecorder), surfaceAlias,
                          std::move(mRecordedSurface), std::move(mCanvasChild));
      return;
    }

    mRecorder->AddPendingDeletion(
        [recorder = std::move(mRecorder), surfaceAlias,
         aliasedSurface = std::move(mRecordedSurface),
         canvasChild = std::move(mCanvasChild)]() mutable -> void {
          ReleaseOnMainThread(std::move(recorder), surfaceAlias,
                              std::move(aliasedSurface),
                              std::move(canvasChild));
        });
  }

  gfx::SurfaceType GetType() const final { return mRecordedSurface->GetType(); }

  gfx::IntSize GetSize() const final { return mRecordedSurface->GetSize(); }

  gfx::SurfaceFormat GetFormat() const final {
    return mRecordedSurface->GetFormat();
  }

  already_AddRefed<gfx::DataSourceSurface> GetDataSurface() final {
    EnsureDataSurfaceOnMainThread();
    return do_AddRef(mDataSourceSurface);
  }

 private:
  void EnsureDataSurfaceOnMainThread() {
    // The data can only be retrieved on the main thread.
    if (!mDataSourceSurface && NS_IsMainThread()) {
      mDataSourceSurface = mCanvasChild->GetDataSurface(mRecordedSurface);
    }
  }

  // Used to ensure that clean-up that requires it is done on the main thread.
  static void ReleaseOnMainThread(RefPtr<CanvasDrawEventRecorder> aRecorder,
                                  ReferencePtr aSurfaceAlias,
                                  RefPtr<gfx::SourceSurface> aAliasedSurface,
                                  RefPtr<CanvasChild> aCanvasChild) {
    MOZ_ASSERT(NS_IsMainThread());

    aRecorder->RemoveStoredObject(aSurfaceAlias);
    aRecorder->RecordEvent(RecordedRemoveSurfaceAlias(aSurfaceAlias));
    aAliasedSurface = nullptr;
    aCanvasChild = nullptr;
    aRecorder = nullptr;
  }

  RefPtr<gfx::SourceSurface> mRecordedSurface;
  RefPtr<CanvasChild> mCanvasChild;
  RefPtr<CanvasDrawEventRecorder> mRecorder;
  RefPtr<gfx::DataSourceSurface> mDataSourceSurface;
};

CanvasChild::CanvasChild() = default;

CanvasChild::~CanvasChild() = default;

static void NotifyCanvasDeviceReset() {
  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (obs) {
    obs->NotifyObservers(nullptr, "canvas-device-reset", nullptr);
  }
}

ipc::IPCResult CanvasChild::RecvNotifyDeviceChanged() {
  NotifyCanvasDeviceReset();
  mRecorder->RecordEvent(RecordedDeviceChangeAcknowledged());
  return IPC_OK();
}

/* static */ bool CanvasChild::mDeactivated = false;

ipc::IPCResult CanvasChild::RecvDeactivate() {
  RefPtr<CanvasChild> self(this);
  mDeactivated = true;
  if (auto* cm = gfx::CanvasManagerChild::Get()) {
    cm->DeactivateCanvas();
  }
  NotifyCanvasDeviceReset();
  return IPC_OK();
}

void CanvasChild::EnsureRecorder(gfx::IntSize aSize, gfx::SurfaceFormat aFormat,
                                 TextureType aTextureType) {
  if (!mRecorder) {
    auto recorder = MakeRefPtr<CanvasDrawEventRecorder>();
    if (!recorder->Init(aTextureType, MakeUnique<RecorderHelpers>(this))) {
      return;
    }

    mRecorder = recorder.forget();
  }

  MOZ_RELEASE_ASSERT(mRecorder->GetTextureType() == aTextureType,
                     "We only support one remote TextureType currently.");

  EnsureDataSurfaceShmem(aSize, aFormat);
}

void CanvasChild::ActorDestroy(ActorDestroyReason aWhy) {
  if (mRecorder) {
    mRecorder->DetachResources();
  }
}

void CanvasChild::Destroy() {
  if (CanSend()) {
    Send__delete__(this);
  }
}

void CanvasChild::OnTextureWriteLock() {
  mHasOutstandingWriteLock = true;
  mLastWriteLockCheckpoint = mRecorder->CreateCheckpoint();
}

void CanvasChild::OnTextureForwarded() {
  if (mHasOutstandingWriteLock) {
    mRecorder->RecordEvent(RecordedCanvasFlush());
    if (!mRecorder->WaitForCheckpoint(mLastWriteLockCheckpoint)) {
      gfxWarning() << "Timed out waiting for last write lock to be processed.";
    }

    mHasOutstandingWriteLock = false;
  }

  // We hold onto the last transaction's external surfaces until we have waited
  // for the write locks in this transaction. This means we know that the
  // surfaces have been picked up in the canvas threads and there is no race
  // with them being removed from SharedSurfacesParent. Note this releases the
  // current contents of mLastTransactionExternalSurfaces.
  mRecorder->TakeExternalSurfaces(mLastTransactionExternalSurfaces);
}

bool CanvasChild::EnsureBeginTransaction() {
  if (!mIsInTransaction) {
    RecordEvent(RecordedCanvasBeginTransaction());
    mIsInTransaction = true;
  }

  return true;
}

void CanvasChild::EndTransaction() {
  if (mIsInTransaction) {
    RecordEvent(RecordedCanvasEndTransaction());
    mIsInTransaction = false;
    mDormant = false;
  } else if (mRecorder) {
    // Schedule to drop free buffers if we have no non-empty transactions.
    if (!mDormant) {
      mDormant = true;
      NS_DelayedDispatchToCurrentThread(
          NewRunnableMethod("CanvasChild::DropFreeBuffersWhenDormant", this,
                            &CanvasChild::DropFreeBuffersWhenDormant),
          StaticPrefs::gfx_canvas_remote_drop_buffer_milliseconds());
    }
  }

  ++mTransactionsSinceGetDataSurface;
}

void CanvasChild::DropFreeBuffersWhenDormant() {
  // Drop any free buffers if we have not had any non-empty transactions.
  if (mDormant && mRecorder) {
    mRecorder->DropFreeBuffers();
  }
}

void CanvasChild::ClearCachedResources() {
  if (mRecorder) {
    mRecorder->DropFreeBuffers();
  }
}

bool CanvasChild::ShouldBeCleanedUp() const {
  // Always return true if we've been deactivated.
  if (Deactivated()) {
    return true;
  }

  // We can only be cleaned up if nothing else references our recorder.
  return !mRecorder || mRecorder->hasOneRef();
}

already_AddRefed<gfx::DrawTarget> CanvasChild::CreateDrawTarget(
    gfx::IntSize aSize, gfx::SurfaceFormat aFormat) {
  if (!mRecorder) {
    return nullptr;
  }

  RefPtr<gfx::DrawTarget> dummyDt = gfx::Factory::CreateDrawTarget(
      gfx::BackendType::SKIA, gfx::IntSize(1, 1), aFormat);
  RefPtr<gfx::DrawTarget> dt = MakeAndAddRef<gfx::DrawTargetRecording>(
      mRecorder, dummyDt, gfx::IntRect(gfx::IntPoint(0, 0), aSize));

  return dt.forget();
}

bool CanvasChild::EnsureDataSurfaceShmem(gfx::IntSize aSize,
                                         gfx::SurfaceFormat aFormat) {
  if (!mRecorder) {
    return false;
  }

  size_t dataFormatWidth = aSize.width * BytesPerPixel(aFormat);
  size_t sizeRequired =
      ipc::SharedMemory::PageAlignedSize(dataFormatWidth * aSize.height);

  if (!mDataSurfaceShmemAvailable || mDataSurfaceShmem->Size() < sizeRequired) {
    RecordEvent(RecordedPauseTranslation());
    auto dataSurfaceShmem = MakeRefPtr<ipc::SharedMemoryBasic>();
    if (!dataSurfaceShmem->Create(sizeRequired) ||
        !dataSurfaceShmem->Map(sizeRequired)) {
      return false;
    }

    auto shmemHandle = dataSurfaceShmem->TakeHandle();
    if (!shmemHandle) {
      return false;
    }

    if (!SendSetDataSurfaceBuffer(std::move(shmemHandle), sizeRequired)) {
      return false;
    }

    mDataSurfaceShmem = dataSurfaceShmem.forget();
    mDataSurfaceShmemAvailable = true;
  }

  MOZ_ASSERT(mDataSurfaceShmemAvailable);
  return true;
}

void CanvasChild::RecordEvent(const gfx::RecordedEvent& aEvent) {
  // We drop mRecorder in ActorDestroy to break the reference cycle.
  if (!mRecorder) {
    return;
  }

  mRecorder->RecordEvent(aEvent);
}

int64_t CanvasChild::CreateCheckpoint() {
  return mRecorder->CreateCheckpoint();
}

already_AddRefed<gfx::DataSourceSurface> CanvasChild::GetDataSurface(
    const gfx::SourceSurface* aSurface) {
  MOZ_DIAGNOSTIC_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aSurface);

  // mTransactionsSinceGetDataSurface is used to determine if we want to prepare
  // a DataSourceSurface in the GPU process up front at the end of the
  // transaction, but that only makes sense if the canvas JS is requesting data
  // in between transactions.
  if (!mIsInTransaction) {
    mTransactionsSinceGetDataSurface = 0;
  }

  if (!EnsureBeginTransaction()) {
    return nullptr;
  }

  RecordEvent(RecordedPrepareDataForSurface(aSurface));

  gfx::IntSize ssSize = aSurface->GetSize();
  gfx::SurfaceFormat ssFormat = aSurface->GetFormat();
  if (!EnsureDataSurfaceShmem(ssSize, ssFormat)) {
    return nullptr;
  }

  mDataSurfaceShmemAvailable = false;
  RecordEvent(RecordedGetDataForSurface(aSurface));
  auto checkpoint = CreateCheckpoint();

  auto* data = static_cast<uint8_t*>(mDataSurfaceShmem->memory());
  auto* closure = new DataShmemHolder{do_AddRef(mDataSurfaceShmem), this};
  auto dataFormatWidth = ssSize.width * BytesPerPixel(ssFormat);

  RefPtr<gfx::DataSourceSurface> dataSurface =
      gfx::Factory::CreateWrappingDataSourceSurface(
          data, dataFormatWidth, ssSize, ssFormat, ReleaseDataShmemHolder,
          closure);

  mRecorder->WaitForCheckpoint(checkpoint);
  return dataSurface.forget();
}

/* static */ void CanvasChild::ReleaseDataShmemHolder(void* aClosure) {
  if (!NS_IsMainThread()) {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "CanvasChild::ReleaseDataShmemHolder",
        [aClosure]() { ReleaseDataShmemHolder(aClosure); }));
    return;
  }

  auto* shmemHolder = static_cast<DataShmemHolder*>(aClosure);
  shmemHolder->canvasChild->ReturnDataSurfaceShmem(shmemHolder->shmem.forget());
  delete shmemHolder;
}

already_AddRefed<gfx::SourceSurface> CanvasChild::WrapSurface(
    const RefPtr<gfx::SourceSurface>& aSurface) {
  if (!aSurface) {
    return nullptr;
  }

  return MakeAndAddRef<SourceSurfaceCanvasRecording>(aSurface, this, mRecorder);
}

void CanvasChild::ReturnDataSurfaceShmem(
    already_AddRefed<ipc::SharedMemoryBasic> aDataSurfaceShmem) {
  RefPtr<ipc::SharedMemoryBasic> data = aDataSurfaceShmem;
  // We can only reuse the latest data surface shmem.
  if (data == mDataSurfaceShmem) {
    MOZ_ASSERT(!mDataSurfaceShmemAvailable);
    mDataSurfaceShmemAvailable = true;
  }
}

}  // namespace layers
}  // namespace mozilla
