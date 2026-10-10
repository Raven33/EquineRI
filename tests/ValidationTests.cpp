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
