/*
 * feature4 (genesis-world-poc) — pybind wrapper around vk_gaussian_splatting.
 *
 * Goal (M1.0): expose an in-process, headless, per-frame render entry so Genesis
 * can drive vk_gs like it drives Nyx (nyx_py_renderer), instead of the offline
 * CLI (loadCameraPresets + sequencer -> PNG batch -> exit).
 *
 * M1.0a: prove the in-process loop { init -> set_camera -> step(=run 1 frame) ->
 *        save PNG } and that Application::run() is re-entrant across calls.
 * M1.0b: replace PNG save with in-memory readback -> numpy (host).
 * M2:    replace host readback with VK external memory -> HIP/DLPack (zero copy).
 *
 * This file is compiled ONLY into the `vkgs` python module target (see CMakeLists),
 * never into the executable (which owns main.cpp).
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include <gaussian_splatting_ui.h>
#include "elem_camera_custom.hpp"
#include "hardware_support.h"

namespace py = pybind11;
using namespace vk_gaussian_splatting;

namespace {

// Subclass to reach protected engine members (m_gBuffers, COLOR_* enum) needed
// for camera control and framebuffer save/readback.
class GaussianSplattingHeadless : public GaussianSplattingUI
{
public:
  GaussianSplattingHeadless(nvutils::ProfilerManager* pm,
                            nvutils::ParameterRegistry* pr,
                            bool*                       benchmark,
                            const std::string&          shaderRoot)
      : GaussianSplattingUI(pm, pr, benchmark)
  {
    // The engine resolves shader dirs relative to nvutils::getExecutablePath(),
    // which for a python module points at the interpreter, not the vk_gs tree.
    // Append the real source dirs so runtime slang compilation can find them:
    //   <root>/shaders          -> "image_compare_composite.comp.slang", etc.
    //   <root>/nvpro_core2      -> "nvshaders/tonemapper.slang", etc.
    if(!shaderRoot.empty())
    {
      std::filesystem::path root(shaderRoot);
      m_slangCompiler.addSearchPaths({root / "shaders", root / "nvpro_core2"});
    }
  }

  uint32_t splatCount() { return m_assets.splatSets.getTotalGlobalSplatCount(); }

  // Scene is renderable once the async ply loader is done and splats exist.
  bool sceneReady()
  {
    return m_plyLoader.getStatus() != PlyLoaderAsync::State::E_LOADING
           && m_assets.splatSets.getTotalGlobalSplatCount() > 0;
  }

  void setCameraLookAt(const glm::vec3& eye, const glm::vec3& center, const glm::vec3& up, float fovyDeg)
  {
    cameraManip->setLookat(eye, center, up);
    cameraManip->setFov(fovyDeg);
  }

  // M1.0a: save the tone-mapped LDR output to a PNG via the app helper.
  // Called after a frame has been rendered (GPU idle is ensured inside saveImageToFile).
  void saveMainImage(nvapp::Application* app, const std::string& path)
  {
    app->saveImageToFile(m_gBuffers.getColorImage(COLOR_LDR), m_gBuffers.getSize(), path, 100);
  }
};

// Build a Vulkan context configured like main.cpp (RT extensions optional, forcegpu).
static void configureVulkan(nvvk::ContextInitInfo& vkSetup, int forceGpu)
{
  vkSetup.enableAllFeatures = true;
  vkSetup.apiVersion        = VK_API_VERSION_1_4;
  vkSetup.forceGPU          = forceGpu;

  vkSetup.instanceExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  static VkPhysicalDeviceMeshShaderFeaturesEXT meshFeaturesEXT = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
  static VkPhysicalDeviceFragmentShadingRateFeaturesKHR fragFeaturesKHR = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
  static VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR baryFeaturesKHR = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};

  vkSetup.deviceExtensions.emplace_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
  vkSetup.deviceExtensions.emplace_back(VK_EXT_MESH_SHADER_EXTENSION_NAME, &meshFeaturesEXT, false);
  vkSetup.deviceExtensions.emplace_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, &fragFeaturesKHR, true);
  vkSetup.deviceExtensions.emplace_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME, &baryFeaturesKHR, false);
  vkSetup.deviceExtensions.emplace_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);

  static VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeature = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  vkSetup.deviceExtensions.emplace_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, &accelFeature, false);
  static VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeature = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  vkSetup.deviceExtensions.emplace_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, &rtPipelineFeature, false);
  vkSetup.deviceExtensions.emplace_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, nullptr, false);
  static VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR rtPositionFetch = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR, .rayTracingPositionFetch = VK_TRUE};
  vkSetup.deviceExtensions.emplace_back(VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME, &rtPositionFetch, false);

  vkSetup.deviceExtensions.emplace_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
}

}  // namespace

// In-process headless renderer. One instance owns the Vulkan context + Application
// + the GaussianSplatting element, loads the splat scene once, then renders one
// frame per render()/step() call.
class Renderer
{
public:
  Renderer(const std::string& ply, int width, int height, int forceGpu, const std::string& shaderRoot)
  {
    nvvk::ContextInitInfo vkSetup;
    configureVulkan(vkSetup, forceGpu);
    if(m_vkContext.init(vkSetup) != VK_SUCCESS)
      throw std::runtime_error("vkgs.Renderer: Vulkan context init failed (Vulkan 1.4 required)");
    isSupported.initFromVulkanContext(m_vkContext);

    registerCommandLineParameters(&m_parameterRegistry);

    m_gs = std::make_shared<GaussianSplattingHeadless>(&m_profilerManager, &m_parameterRegistry, &m_benchmark, shaderRoot);

    nvapp::ApplicationCreateInfo appInfo;
    appInfo.name               = "vkgs_pybind";
    appInfo.instance           = m_vkContext.getInstance();
    appInfo.device             = m_vkContext.getDevice();
    appInfo.physicalDevice     = m_vkContext.getPhysicalDevice();
    appInfo.queues             = m_vkContext.getQueueInfos();
    appInfo.headless           = true;
    appInfo.headlessFrameCount = 1;  // one frame per run()
    appInfo.windowSize         = {uint32_t(width), uint32_t(height)};
    appInfo.useMenu            = false;
    if(m_application.init(appInfo) != VK_SUCCESS)
      throw std::runtime_error("vkgs.Renderer: Application init failed");

    auto elemCamera = std::make_shared<ElementCameraCustom>();
    elemCamera->setCameraManipulator(m_gs->cameraManip);

    m_application.addElement(m_gs);       // engine
    m_application.addElement(elemCamera);  // camera (after engine? see main.cpp order)

    // Request the splat scene load. The ply loader runs on a background thread and
    // its results are consumed per-frame in onRender, so pump frames until the
    // scene is ready (splats uploaded) before returning control to Python.
    m_gs->onFileDrop(std::filesystem::path(ply));
    const int kMaxLoadFrames = 2000;
    int       frames         = 0;
    for(; frames < kMaxLoadFrames && !m_gs->sceneReady(); ++frames)
      m_application.run();
    if(!m_gs->sceneReady())
      throw std::runtime_error("vkgs.Renderer: scene did not finish loading (splatCount=0 after "
                               + std::to_string(frames) + " frames): " + ply);
  }

  ~Renderer()
  {
    m_application.deinit();
    m_vkContext.deinit();
  }

  void setCamera(const std::array<float, 3>& eye,
                 const std::array<float, 3>& center,
                 const std::array<float, 3>& up,
                 float                       fovyDeg)
  {
    m_gs->setCameraLookAt({eye[0], eye[1], eye[2]}, {center[0], center[1], center[2]}, {up[0], up[1], up[2]}, fovyDeg);
  }

  // render one frame
  void step() { m_application.run(); }

  uint32_t splatCount() { return m_gs->splatCount(); }

  // M1.0a: save last rendered frame to PNG
  void savePng(const std::string& path) { m_gs->saveMainImage(&m_application, path); }

private:
  nvutils::ProfilerManager                   m_profilerManager;
  nvutils::ParameterRegistry                 m_parameterRegistry;
  bool                                       m_benchmark = true;  // synchronous loads, no vsync
  nvvk::Context                              m_vkContext;
  nvapp::Application                         m_application;
  std::shared_ptr<GaussianSplattingHeadless> m_gs;
};

PYBIND11_MODULE(vkgs, m)
{
  m.doc() = "In-process headless vk_gaussian_splatting renderer (feature4 M1.0)";

  py::class_<Renderer>(m, "Renderer")
      .def(py::init<const std::string&, int, int, int, const std::string&>(), py::arg("ply"), py::arg("width") = 1280,
           py::arg("height") = 720, py::arg("gpu") = 0, py::arg("shader_root") = "/work/vk_gaussian_splatting")
      .def("set_camera", &Renderer::setCamera, py::arg("eye"), py::arg("center"), py::arg("up"), py::arg("fovy") = 60.0f)
      .def("step", &Renderer::step, py::call_guard<py::gil_scoped_release>())
      .def("splat_count", &Renderer::splatCount)
      .def("save_png", &Renderer::savePng, py::arg("path"));
}
