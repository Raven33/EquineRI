// GPU-free tests of RI's validation (P88, P91): the object tracker, EQ_VALIDATE,
// and checks called directly.

#include <TestHarness.hpp>

#include <RI/Validation/Checks.hpp>
#include <RI/Validation/ObjectTracker.hpp>

#include <RI/Commands.hpp>
#include <RI/Resources.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <vector>

EQ_TEST(validation, tracker_alive_then_destroyed) {
    RI::Validation::ObjectTracker tracker;
    const uint64 a = RI::Validation::trackObject(tracker);
    const uint64 b = RI::Validation::trackObject(tracker);
    EQ_CHECK(a != b);
    EQ_CHECK(!RI::Validation::checkAlive(tracker, a, "buffer"));
    RI::Validation::untrackObject(tracker, a);
    const RI::Validation::CheckResult dead = RI::Validation::checkAlive(tracker, a, "buffer");
    EQ_CHECK(dead && dead->find("buffer was destroyed") != std::string::npos);
    EQ_CHECK(!RI::Validation::checkAlive(tracker, b, "buffer"));
}

EQ_TEST(validation, tracker_rejects_never_created) {
    const RI::Validation::ObjectTracker tracker;
    const RI::Validation::CheckResult never = RI::Validation::checkAlive(tracker, 0, "texture");
    EQ_CHECK(never && never->find("texture was never created") != std::string::npos);
}

// Both devices hold a live object, so equal per-device numbering would hide the misuse.
EQ_TEST(validation, tracker_rejects_other_device) {
    RI::Validation::ObjectTracker deviceA;
    RI::Validation::ObjectTracker deviceB;
    const uint64 onA = RI::Validation::trackObject(deviceA);
    const uint64 onB = RI::Validation::trackObject(deviceB);
    EQ_CHECK(onA != onB);
    EQ_CHECK(RI::Validation::checkAlive(deviceB, onA, "buffer").has_value());
    EQ_CHECK(RI::Validation::checkAlive(deviceA, onB, "buffer").has_value());
}

// The argument is evaluated only when validation is on.
EQ_TEST(validation, macro_evaluates_only_when_on) {
    int evaluated = 0;
    EQ_VALIDATE((++evaluated, RI::Validation::CheckResult{}));
    EQ_CHECK(evaluated == (EQUINE_VALIDATION ? 1 : 0));
}

#if EQUINE_VALIDATION
// Aborts by design; registered only in validation builds (tests/CMakeLists.txt).
EQ_TEST(validation, macro_reports_failure) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    EQ_VALIDATE(RI::Validation::CheckResult{"slot 3 is still in use"});
}
#endif

// Milestone 3 checks: rendering-section state, view formats and view ranges (P40, P73, P74).
EQ_TEST(validation, texture_and_rendering_checks) {
    using namespace RI::Validation;
    EQ_CHECK(!checkRenderingSection(true, true, "cmdDraw"));
    EQ_CHECK(!checkRenderingSection(false, false, "cmdClearTexture"));
    const CheckResult notOpen = checkRenderingSection(false, true, "cmdEndRendering");
    EQ_CHECK(notOpen && notOpen->find("cmdEndRendering: no rendering section is open") != std::string::npos);
    const CheckResult inside = checkRenderingSection(true, false, "cmdClearTexture");
    EQ_CHECK(inside && inside->find("cmdClearTexture: not allowed inside") != std::string::npos);

    EQ_CHECK(!checkViewFormatCompatible(RI::Format::eRGBA8Unorm, RI::Format::eRGBA8Srgb));
    EQ_CHECK(!checkViewFormatCompatible(RI::Format::eRGBA8Unorm, RI::Format::eR32Float));
    EQ_CHECK(checkViewFormatCompatible(RI::Format::eRGBA8Unorm, RI::Format::eRGBA16Float).has_value());

    EQ_CHECK(!checkViewRange(1, 2, 3, 0, 1, 1));
    EQ_CHECK(checkViewRange(2, 2, 3, 0, 1, 1).has_value());
    EQ_CHECK(checkViewRange(0, 1, 1, 1, 1, 1).has_value());
}

// Values passed to the pixel shader (P101), matched by semantic and slot.
EQ_TEST(validation, stage_link) {
    using namespace RI::Validation;
    EQ_CHECK(stageSemantic("normal") == "NORMAL0");
    EQ_CHECK(stageSemantic("Color1") == "COLOR1");
    const StageValue outputs[] = {{.semantic = "SV_POSITION0", .slot = 0},
                                  {.semantic = "TEXCOORD0", .slot = 1},
                                  {.semantic = "NORMAL0", .slot = 2},
                                  {.semantic = "PRIMID0", .slot = 0, .slotChecked = false}};
    // The whole struct, and a prefix of it, link.
    EQ_CHECK(!checkStageLink(outputs, std::span(outputs).first(3), "vertex", "register"));
    EQ_CHECK(!checkStageLink(outputs, std::span(outputs).first(2), "vertex", "register"));
    // A per-primitive value is matched by semantic only.
    const StageValue primitive[] = {{.semantic = "PRIMID0", .slot = 5}};
    EQ_CHECK(!checkStageLink(outputs, primitive, "mesh", "register"));
    // Leaving out values before NORMAL moves it.
    const StageValue skipped[] = {{.semantic = "NORMAL0", .slot = 0}};
    const CheckResult moved = checkStageLink(outputs, skipped, "vertex", "register");
    EQ_CHECK(moved && moved->find("the pixel shader reads NORMAL0 at register 0, but the vertex shader writes it at "
                                  "register 2") != std::string::npos);
    // Another component of the same register.
    const StageValue component[] = {{.semantic = "NORMAL0", .slot = 2, .component = 1}};
    const CheckResult shifted = checkStageLink(outputs, component, "vertex", "register");
    EQ_CHECK(shifted && shifted->find("at register 2, component 1") != std::string::npos);
    // A value the vertex shader does not write.
    const StageValue missing[] = {{.semantic = "COLOR0", .slot = 0}};
    const CheckResult absent = checkStageLink(outputs, missing, "vertex", "location");
    EQ_CHECK(absent && absent->find("reads COLOR0, which the vertex shader does not write") != std::string::npos);
}

// Hull and domain shaders come together with a patch list; mesh pipelines take
// none of the new stages (P102).
EQ_TEST(validation, graphics_stages) {
    using namespace RI::Validation;
    EQ_CHECK(!checkGraphicsStages(false, false, false, false, false, 0));
    EQ_CHECK(!checkGraphicsStages(false, false, false, true, false, 0));
    EQ_CHECK(!checkGraphicsStages(false, true, true, true, true, 3));
    EQ_CHECK(!checkGraphicsStages(false, true, true, false, true, 32));
    const CheckResult alone = checkGraphicsStages(false, true, false, false, true, 3);
    EQ_CHECK(alone && alone->find("give the hull and domain shaders together") != std::string::npos);
    const CheckResult noPatches = checkGraphicsStages(false, true, true, false, false, 3);
    EQ_CHECK(noPatches && noPatches->find("need Topology::ePatchList") != std::string::npos);
    const CheckResult patchesAlone = checkGraphicsStages(false, false, false, false, true, 3);
    EQ_CHECK(patchesAlone && patchesAlone->find("needs hull and domain shaders") != std::string::npos);
    const CheckResult points = checkGraphicsStages(false, true, true, false, true, 33);
    EQ_CHECK(points && points->find("patchControlPoints is 33") != std::string::npos);
    const CheckResult mesh = checkGraphicsStages(true, false, false, true, false, 0);
    EQ_CHECK(mesh && mesh->find("mesh pipelines take no hull, domain or geometry shader") != std::string::npos);
}

// Cube and 3D texture shapes (P102).
EQ_TEST(validation, texture_shapes) {
    using namespace RI::Validation;
    using RI::TextureShape;
    EQ_CHECK(!checkTextureShape({.width = 4, .height = 2}));
    EQ_CHECK(!checkTextureShape({.shape = TextureShape::eCube, .width = 4, .height = 4, .layers = 12}));
    EQ_CHECK(!checkTextureShape({.shape = TextureShape::e3D, .width = 4, .height = 2, .depth = 8}));
    const CheckResult flat = checkTextureShape({.width = 4, .height = 4, .depth = 2});
    EQ_CHECK(flat && flat->find("only 3D textures") != std::string::npos);
    const CheckResult notSquare = checkTextureShape({.shape = TextureShape::eCube, .width = 4, .height = 2, .layers = 6});
    EQ_CHECK(notSquare && notSquare->find("square") != std::string::npos);
    const CheckResult faces = checkTextureShape({.shape = TextureShape::eCube, .width = 4, .height = 4, .layers = 4});
    EQ_CHECK(faces && faces->find("6 layers per cube") != std::string::npos);
    const CheckResult layered = checkTextureShape({.shape = TextureShape::e3D, .depth = 4, .layers = 2});
    EQ_CHECK(layered && layered->find("not layers") != std::string::npos);
    const CheckResult depth =
        checkTextureShape({.shape = TextureShape::e3D, .depth = 4, .format = RI::Format::eD32Float});
    EQ_CHECK(depth && depth->find("cannot have a depth format") != std::string::npos);
    const CheckResult msaa =
        checkTextureShape({.shape = TextureShape::eCube, .width = 4, .height = 4, .layers = 6, .samples = 4});
    EQ_CHECK(msaa && msaa->find("not multisampled") != std::string::npos);
    EQ_CHECK(!checkVolumeView(3, false));
    EQ_CHECK(!checkVolumeView(1, true));
    const CheckResult slices = checkVolumeView(2, true);
    EQ_CHECK(slices && slices->find("covers one mip") != std::string::npos);
}

// Viewport and scissor values (P103).
EQ_TEST(validation, viewport_and_scissor) {
    using namespace RI::Validation;
    EQ_CHECK(!checkViewport({.width = 1.0f, .height = 1.0f}));
    const CheckResult empty = checkViewport({.width = 0.0f, .height = 4.0f});
    EQ_CHECK(empty && empty->find("give a width and height above 0") != std::string::npos);
    const CheckResult depth = checkViewport({.width = 4.0f, .height = 4.0f, .maxDepth = 2.0f});
    EQ_CHECK(depth && depth->find("outside 0..1") != std::string::npos);
    EQ_CHECK(!checkScissor({.x = 3, .y = 0, .width = 2, .height = 2}));
    const CheckResult negative = checkScissor({.x = -1, .y = 0, .width = 2, .height = 2});
    EQ_CHECK(negative && negative->find("must be 0 or more") != std::string::npos);
}

// A shader's 64-bit atomics against the device's limits (P105).
EQ_TEST(validation, int64_atomics) {
    using namespace RI::Validation;
    EQ_CHECK(!checkInt64Atomics(true, true, true, true, "createComputePipeline", "compute"));
    EQ_CHECK(!checkInt64Atomics(false, false, false, false, "createComputePipeline", "compute"));
    const CheckResult heap = checkInt64Atomics(true, false, false, true, "createComputePipeline", "compute");
    EQ_CHECK(heap && heap->find("compute shader") != std::string::npos &&
             heap->find("int64BufferAtomics") != std::string::npos);
    const CheckResult shared = checkInt64Atomics(false, true, true, false, "createGraphicsPipeline", "mesh");
    EQ_CHECK(shared && shared->find("mesh shader") != std::string::npos &&
             shared->find("int64SharedAtomics") != std::string::npos);
}

// A mesh-primitives query is not begun inside an open statistics query (P105).
EQ_TEST(validation, query_nesting) {
    using namespace RI::Validation;
    using RI::QueryType;
    EQ_CHECK(!checkQueryNesting(QueryType::eMeshPrimitives, 0));
    EQ_CHECK(!checkQueryNesting(QueryType::eMeshPipelineStatistics, 0));
    EQ_CHECK(!checkQueryNesting(QueryType::eOcclusion, 1));
    const CheckResult nested = checkQueryNesting(QueryType::eMeshPrimitives, 1);
    EQ_CHECK(nested && nested->find("begin it before the statistics query") != std::string::npos);
}

// Sections without attachments (P105): a RenderingDesc needs one, the size form
// takes 1..16384.
EQ_TEST(validation, rendering_without_attachments) {
    using namespace RI::Validation;
    using RI::RenderingSize;
    EQ_CHECK(!checkRenderingAttachments(1, false));
    EQ_CHECK(!checkRenderingAttachments(0, true));
    const CheckResult none = checkRenderingAttachments(0, false);
    EQ_CHECK(none && none->find("RenderingSize") != std::string::npos);
    EQ_CHECK(!checkRenderingSize({.width = 1, .height = 16384}));
    for (const RenderingSize size : {RenderingSize{.width = 0, .height = 4}, RenderingSize{.width = 16385, .height = 4},
                                     RenderingSize{.width = 4, .height = 0}}) {
        const CheckResult bad = checkRenderingSize(size);
        EQ_CHECK(bad && bad->find("must be 1..16384") != std::string::npos);
    }
}

// The tracker under creation on other threads (P106): ids registered and
// forgotten on four threads while this one checks a live object.
EQ_TEST(validation, tracker_across_threads) {
    RI::Validation::ObjectTracker tracker;
    const uint64 kept = RI::Validation::trackObject(tracker);
    std::atomic<bool> done{false};
    std::atomic<bool> lost{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < 2000; ++i) {
                RI::Validation::untrackObject(tracker, RI::Validation::trackObject(tracker));
            }
        });
    }
    std::thread checker([&] {
        while (!done) {
            if (RI::Validation::checkAlive(tracker, kept, "pipeline")) {
                lost = true;
            }
        }
    });
    for (std::thread &worker : workers) {
        worker.join();
    }
    done = true;
    checker.join();
    EQ_CHECK(!lost);
    EQ_CHECK(tracker.live.size() == 1);
}

// Buffer views (P106): D3D12's element limit on every backend, then the device's bytes.
EQ_TEST(validation, buffer_view_size) {
    using RI::Validation::checkBufferViewSize;
    constexpr uint64 kElements = RI::kMaxBufferViewElements;
    constexpr uint64 kLarge = uint64{1} << 40;
    EQ_CHECK(!checkBufferViewSize(kElements * 4, 0, false, kLarge, 65536));
    const RI::Validation::CheckResult raw = checkBufferViewSize((kElements + 1) * 4, 0, false, kLarge, 65536);
    EQ_CHECK(raw && raw->find("kMaxBufferViewElements") != std::string::npos);
    EQ_CHECK(!checkBufferViewSize(kElements * 16, 16, false, kLarge, 65536));
    EQ_CHECK(checkBufferViewSize((kElements + 1) * 16, 16, false, kLarge, 65536).has_value());
    const RI::Validation::CheckResult bytes = checkBufferViewSize(1 << 20, 0, false, 1 << 19, 65536);
    EQ_CHECK(bytes && bytes->find("maxBufferViewSize") != std::string::npos);
    EQ_CHECK(!checkBufferViewSize(65536, 0, true, kLarge, 65536));
    const RI::Validation::CheckResult constant = checkBufferViewSize(65536 + 1, 0, true, kLarge, 65536);
    EQ_CHECK(constant && constant->find("maxConstantViewSize") != std::string::npos);
}

// Group counts (P106): 65,535 per dimension; mesh draws 2^22 in all.
EQ_TEST(validation, group_counts) {
    using RI::Validation::checkGroupCounts;
    EQ_CHECK(!checkGroupCounts(65535, 65535, 1, false, "cmdDispatch"));
    const RI::Validation::CheckResult wide = checkGroupCounts(65536, 1, 1, false, "cmdDispatch");
    EQ_CHECK(wide && wide->find("cmdDispatch: 65536 x 1 x 1 groups") != std::string::npos);
    EQ_CHECK(checkGroupCounts(1, 1, 65536, false, "cmdDispatch").has_value());
    EQ_CHECK(!checkGroupCounts(2048, 2048, 1, true, "cmdDrawMesh"));
    const RI::Validation::CheckResult mesh = checkGroupCounts(2048, 2048, 2, true, "cmdDrawMesh");
    EQ_CHECK(mesh && mesh->find("kMaxMeshGroups") != std::string::npos);
    EQ_CHECK(!checkGroupCounts(0, 0, 0, true, "cmdDrawMesh"));
}

// Bindful constant buffers (P112): merging stages, the root budget, binds, and
// slots left unset.
EQ_TEST(validation, constant_buffers) {
    using RI::Validation::ConstantBufferSlots;
    ConstantBufferSlots pipeline;
    ConstantBufferSlots vertex{.mask = 0b11u};
    vertex.sizes[0] = 16;
    vertex.sizes[1] = 64;
    ConstantBufferSlots pixel{.mask = 0b110u};
    pixel.sizes[1] = 64;
    pixel.sizes[2] = 32;
    EQ_CHECK(!RI::Validation::mergeConstantBuffers(pipeline, vertex, "createGraphicsPipeline", "vertex"));
    EQ_CHECK(!RI::Validation::mergeConstantBuffers(pipeline, pixel, "createGraphicsPipeline", "pixel"));
    EQ_CHECK(pipeline.mask == 0b111u && pipeline.sizes[2] == 32);
    ConstantBufferSlots clash{.mask = 0b10u};
    clash.sizes[1] = 48;
    const RI::Validation::CheckResult merged = RI::Validation::mergeConstantBuffers(pipeline, clash, "c", "pixel");
    EQ_CHECK(merged && merged->find("slot 1 with 48 bytes") != std::string::npos);

    EQ_CHECK(!RI::Validation::checkRootBudget(232, pipeline, 256, 65536, "c")); // 232 + 3 * 8 = 256
    const RI::Validation::CheckResult budget = RI::Validation::checkRootBudget(233, pipeline, 256, 65536, "c");
    EQ_CHECK(budget && budget->find("together they take 260 bytes") != std::string::npos);
    EQ_CHECK(RI::Validation::checkRootBudget(0, pipeline, 256, 63, "c").has_value());

    using RI::Validation::checkConstantBufferBind;
    EQ_CHECK(!checkConstantBufferBind(2, pipeline, true, 256, 512));
    EQ_CHECK(checkConstantBufferBind(3, pipeline, true, 0, 512).has_value());
    EQ_CHECK(checkConstantBufferBind(8, pipeline, true, 0, 512).has_value());
    const RI::Validation::CheckResult usage = checkConstantBufferBind(0, pipeline, false, 0, 512);
    EQ_CHECK(usage && usage->find("eConstant") != std::string::npos);
    EQ_CHECK(checkConstantBufferBind(0, pipeline, true, 128, 512).has_value());
    EQ_CHECK(!checkConstantBufferBind(1, pipeline, true, 256, 320));
    EQ_CHECK(checkConstantBufferBind(1, pipeline, true, 256, 319).has_value());
    EQ_CHECK(checkConstantBufferBind(1, pipeline, true, 512, 320).has_value());

    EQ_CHECK(!RI::Validation::checkConstantBuffersSet(0b101u, 0b111u, "cmdDraw"));
    const RI::Validation::CheckResult unset = RI::Validation::checkConstantBuffersSet(0b101u, 0b001u, "cmdDraw");
    EQ_CHECK(unset && unset->find("cmdDraw: constant buffer slot 2 is not set") != std::string::npos);
}

// Small writes (P107): usage, alignment, the size limit and the range.
EQ_TEST(validation, small_write) {
    using RI::Validation::checkSmallWrite;
    EQ_CHECK(!checkSmallWrite(true, 0, 64, 64));
    EQ_CHECK(!checkSmallWrite(true, 60, 4, 64));
    const RI::Validation::CheckResult usage = checkSmallWrite(false, 0, 4, 64);
    EQ_CHECK(usage && usage->find("eCopyDst") != std::string::npos);
    EQ_CHECK(checkSmallWrite(true, 2, 4, 64).has_value());
    EQ_CHECK(checkSmallWrite(true, 0, 6, 64).has_value());
    EQ_CHECK(checkSmallWrite(true, 0, 0, 64).has_value());
    const RI::Validation::CheckResult large = checkSmallWrite(true, 0, 68, 256);
    EQ_CHECK(large && large->find("kMaxSmallWriteSize") != std::string::npos);
    EQ_CHECK(checkSmallWrite(true, 64, 4, 64).has_value());
}

// Copy regions (P108): the default, inside the mip, not empty; depth, stencil
// and multisampled views only whole.
EQ_TEST(validation, copy_region) {
    using RI::Validation::checkCopyRegion;
    EQ_CHECK(!checkCopyRegion(0, 0, 0, 0, 64, 32, false, "source", "cmdCopyTexture"));
    EQ_CHECK(!checkCopyRegion(0, 0, 0, 0, 64, 32, true, "source", "cmdCopyTexture"));
    EQ_CHECK(!checkCopyRegion(10, 4, 54, 28, 64, 32, false, "source", "cmdCopyTexture"));
    EQ_CHECK(!checkCopyRegion(0, 0, 64, 32, 64, 32, true, "source", "cmdCopyTexture"));
    const RI::Validation::CheckResult empty = checkCopyRegion(3, 4, 0, 0, 64, 32, false, "source", "cmdCopyTexture");
    EQ_CHECK(empty && empty->find("is empty") != std::string::npos);
    EQ_CHECK(checkCopyRegion(0, 0, 8, 0, 64, 32, false, "source", "cmdCopyTexture").has_value());
    const RI::Validation::CheckResult outside =
        checkCopyRegion(60, 0, 8, 8, 64, 32, false, "destination", "cmdCopyBufferToTexture");
    EQ_CHECK(outside && outside->find("destination region 8x8 at 60, 0 is outside the 64x32 mip") != std::string::npos);
    EQ_CHECK(checkCopyRegion(-1, 0, 8, 8, 64, 32, false, "source", "cmdCopyTexture").has_value());
    const RI::Validation::CheckResult whole = checkCopyRegion(0, 0, 8, 8, 64, 32, true, "source", "copyLayout");
    EQ_CHECK(whole && whole->find("copies only whole") != std::string::npos);
}

// A clear's range (P107): multiples of 4, inside the buffer; kWholeSize to the end.
EQ_TEST(validation, clear_range) {
    using RI::Validation::checkClearRange;
    EQ_CHECK(!checkClearRange(0, RI::kWholeSize, 64));
    EQ_CHECK(!checkClearRange(4, 8, 64));
    EQ_CHECK(!checkClearRange(48, RI::kWholeSize, 64));
    EQ_CHECK(checkClearRange(2, 8, 64).has_value());
    EQ_CHECK(checkClearRange(4, 6, 64).has_value());
    EQ_CHECK(checkClearRange(60, 8, 64).has_value());
    EQ_CHECK(checkClearRange(68, RI::kWholeSize, 64).has_value());
    EQ_CHECK(checkClearRange(64, RI::kWholeSize, 64).has_value());
    EQ_CHECK(checkClearRange(0, 0, 64).has_value());
}

// The device thread (P106): the same thread passes, another is named.
EQ_TEST(validation, device_thread) {
    const std::thread::id here = std::this_thread::get_id();
    EQ_CHECK(!RI::Validation::checkDeviceThread(here, "cmdDispatch"));
    RI::Validation::CheckResult other;
    std::thread([&] { other = RI::Validation::checkDeviceThread(here, "cmdDispatch"); }).join();
    EQ_CHECK(other && other->find("cmdDispatch: called from thread") != std::string::npos);
}

// Names (P106): a named object destroyed is reported by name; renaming
// replaces the name; past kDestroyedNames, the oldest names are forgotten.
EQ_TEST(validation, tracker_names) {
    using namespace RI::Validation;
    ObjectTracker tracker;
    const uint64 unnamed = trackObject(tracker);
    const uint64 named = trackObject(tracker);
    setTrackedName(tracker, named, "first");
    setTrackedName(tracker, named, "particles");
    untrackObject(tracker, unnamed);
    untrackObject(tracker, named);
    const CheckResult byName = checkAlive(tracker, named, "buffer");
    EQ_CHECK(byName && byName->find("this buffer 'particles' was destroyed") != std::string::npos);
    const CheckResult byId = checkAlive(tracker, unnamed, "buffer");
    EQ_CHECK(byId && byId->find("this buffer was destroyed") != std::string::npos);
    EQ_CHECK(tracker.destroyed.size() == 1);

    for (size_t i = 0; i < kDestroyedNames; ++i) {
        const uint64 id = trackObject(tracker);
        setTrackedName(tracker, id, "transient");
        untrackObject(tracker, id);
    }
    EQ_CHECK(tracker.destroyed.size() == kDestroyedNames);
    const CheckResult forgotten = checkAlive(tracker, named, "buffer");
    EQ_CHECK(forgotten && forgotten->find("'particles'") == std::string::npos);
}
