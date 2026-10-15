// GPU-free tests of RI's validation (P88, P91): the object tracker, EQ_VALIDATE,
// and checks called directly.

#include <TestHarness.hpp>

#include <RI/Validation/Checks.hpp>
#include <RI/Validation/ObjectTracker.hpp>

#include <csignal>
#include <cstdlib>

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
