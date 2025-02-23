// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shell_test_platform_view_vulkan.h"

#include <utility>

#include "flutter/common/graphics/persistent_cache.h"
#include "flutter/shell/common/context_options.h"
#include "flutter/vulkan/vulkan_utilities.h"

#if OS_FUCHSIA
#define VULKAN_SO_PATH "libvulkan.so"
#elif FML_OS_MACOSX
#define VULKAN_SO_PATH "libvk_swiftshader.dylib"
#elif FML_OS_WIN
#define VULKAN_SO_PATH "vk_swiftshader.dll"
#else
#define VULKAN_SO_PATH "libvk_swiftshader.so"
#endif

namespace flutter {
namespace testing {

ShellTestPlatformViewVulkan::ShellTestPlatformViewVulkan(
    PlatformView::Delegate& delegate,
    const TaskRunners& task_runners,
    std::shared_ptr<ShellTestVsyncClock> vsync_clock,
    CreateVsyncWaiter create_vsync_waiter,
    std::shared_ptr<ShellTestExternalViewEmbedder>
        shell_test_external_view_embedder)
    : ShellTestPlatformView(delegate, task_runners),
      create_vsync_waiter_(std::move(create_vsync_waiter)),
      vsync_clock_(std::move(vsync_clock)),
      proc_table_(fml::MakeRefCounted<vulkan::VulkanProcTable>(VULKAN_SO_PATH)),
      shell_test_external_view_embedder_(
          std::move(shell_test_external_view_embedder)) {}

ShellTestPlatformViewVulkan::~ShellTestPlatformViewVulkan() = default;

std::unique_ptr<VsyncWaiter> ShellTestPlatformViewVulkan::CreateVSyncWaiter() {
  return create_vsync_waiter_();
}

void ShellTestPlatformViewVulkan::SimulateVSync() {
  vsync_clock_->SimulateVSync();
}

// |PlatformView|
std::unique_ptr<Surface> ShellTestPlatformViewVulkan::CreateRenderingSurface() {
  return std::make_unique<OffScreenSurface>(proc_table_,
                                            shell_test_external_view_embedder_);
}

// |PlatformView|
std::shared_ptr<ExternalViewEmbedder>
ShellTestPlatformViewVulkan::CreateExternalViewEmbedder() {
  return shell_test_external_view_embedder_;
}

// |PlatformView|
PointerDataDispatcherMaker ShellTestPlatformViewVulkan::GetDispatcherMaker() {
  return [](DefaultPointerDataDispatcher::Delegate& delegate) {
    return std::make_unique<SmoothPointerDataDispatcher>(delegate);
  };
}

// TODO(gw280): This code was forked from vulkan_window.cc specifically for
// shell_test.
//              We need to merge this functionality back into //vulkan.
//              https://github.com/flutter/flutter/issues/51132
ShellTestPlatformViewVulkan::OffScreenSurface::OffScreenSurface(
    fml::RefPtr<vulkan::VulkanProcTable> vk,
    std::shared_ptr<ShellTestExternalViewEmbedder>
        shell_test_external_view_embedder)
    : valid_(false),
      vk_(std::move(vk)),
      shell_test_external_view_embedder_(
          std::move(shell_test_external_view_embedder)) {
  if (!vk_ || !vk_->HasAcquiredMandatoryProcAddresses()) {
    FML_DLOG(ERROR) << "Proc table has not acquired mandatory proc addresses.";
    return;
  }

  // Create the application instance.
  std::vector<std::string> extensions = {
      VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
  };

  application_ = std::make_unique<vulkan::VulkanApplication>(
      *vk_, "FlutterTest", std::move(extensions), VK_MAKE_VERSION(1, 0, 0),
      VK_MAKE_VERSION(1, 1, 0), true);

  if (!application_->IsValid() || !vk_->AreInstanceProcsSetup()) {
    // Make certain the application instance was created and it set up the
    // instance proc table entries.
    FML_DLOG(ERROR) << "Instance proc addresses have not been set up.";
    return;
  }

  // Create the device.

  logical_device_ = application_->AcquireFirstCompatibleLogicalDevice();

  if (logical_device_ == nullptr || !logical_device_->IsValid() ||
      !vk_->AreDeviceProcsSetup()) {
    // Make certain the device was created and it set up the device proc table
    // entries.
    FML_DLOG(ERROR) << "Device proc addresses have not been set up.";
    return;
  }

  // Create the Skia GrContext.
  if (!CreateSkiaGrContext()) {
    FML_DLOG(ERROR) << "Could not create Skia context.";
    return;
  }

  valid_ = true;
}

bool ShellTestPlatformViewVulkan::OffScreenSurface::CreateSkiaGrContext() {
  GrVkBackendContext backend_context;

  if (!CreateSkiaBackendContext(&backend_context)) {
    FML_DLOG(ERROR) << "Could not create Skia backend context.";
    return false;
  }

  const auto options =
      MakeDefaultContextOptions(ContextType::kRender, GrBackendApi::kVulkan);

  sk_sp<GrDirectContext> context =
      GrDirectContext::MakeVulkan(backend_context, options);

  if (context == nullptr) {
    FML_DLOG(ERROR) << "Failed to create GrDirectContext";
    return false;
  }

  context->setResourceCacheLimit(vulkan::kGrCacheMaxByteSize);

  context_ = context;

  return true;
}

bool ShellTestPlatformViewVulkan::OffScreenSurface::CreateSkiaBackendContext(
    GrVkBackendContext* context) {
  auto getProc = vk_->CreateSkiaGetProc();

  if (getProc == nullptr) {
    FML_DLOG(ERROR) << "GetProcAddress is null";
    return false;
  }

  uint32_t skia_features = 0;
  if (!logical_device_->GetPhysicalDeviceFeaturesSkia(&skia_features)) {
    FML_DLOG(ERROR) << "Failed to get Physical Device features";
    return false;
  }

  context->fInstance = application_->GetInstance();
  context->fPhysicalDevice = logical_device_->GetPhysicalDeviceHandle();
  context->fDevice = logical_device_->GetHandle();
  context->fQueue = logical_device_->GetQueueHandle();
  context->fGraphicsQueueIndex = logical_device_->GetGraphicsQueueIndex();
  context->fMinAPIVersion = application_->GetAPIVersion();
  context->fMaxAPIVersion = application_->GetAPIVersion();
  context->fFeatures = skia_features;
  context->fGetProc = std::move(getProc);
  context->fOwnsInstanceAndDevice = false;
  return true;
}

ShellTestPlatformViewVulkan::OffScreenSurface::~OffScreenSurface() {}

bool ShellTestPlatformViewVulkan::OffScreenSurface::IsValid() {
  return valid_;
}

std::unique_ptr<SurfaceFrame>
ShellTestPlatformViewVulkan::OffScreenSurface::AcquireFrame(
    const SkISize& size) {
  auto image_info = SkImageInfo::Make(size, SkColorType::kRGBA_8888_SkColorType,
                                      SkAlphaType::kOpaque_SkAlphaType);
  auto surface = SkSurface::MakeRenderTarget(context_.get(), SkBudgeted::kNo,
                                             image_info, 0, nullptr);
  SurfaceFrame::SubmitCallback callback = [](const SurfaceFrame&,
                                             SkCanvas* canvas) -> bool {
    canvas->flush();
    return true;
  };

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;

  return std::make_unique<SurfaceFrame>(std::move(surface), framebuffer_info,
                                        std::move(callback),
                                        /*frame_size=*/SkISize::Make(800, 600));
}

GrDirectContext* ShellTestPlatformViewVulkan::OffScreenSurface::GetContext() {
  return context_.get();
}

SkMatrix ShellTestPlatformViewVulkan::OffScreenSurface::GetRootTransformation()
    const {
  SkMatrix matrix;
  matrix.reset();
  return matrix;
}

}  // namespace testing
}  // namespace flutter
