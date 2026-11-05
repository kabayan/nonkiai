#include "motion_interaction.h"
#include "interaction_messages.h"
#include <cstdio>

MotionInteraction::Action MotionInteraction::exit() {
    quiz_ = false;
    stage_ = Stage::Normal;
    answers_ = question_ = 0;
    deadline_us_ = 0;
    return {nullptr, Cue::Exit};
}
MotionInteraction::Action MotionInteraction::question() {
    stage_ = Stage::Question;
    return {kQuizQuestions[question_], Cue::None, false, true};
}
MotionInteraction::Action MotionInteraction::tick(int64_t now_us) {
    if (stage_ == Stage::Waiting && now_us >= deadline_us_) return exit();
    return {};
}
MotionInteraction::Action MotionInteraction::speech_finished(int64_t now_us, bool completed) {
    if (!awaiting_speech()) return {};
    if (!completed) {
        if (quiz_) return exit();
        stage_ = Stage::Normal;
        return {};
    }
    switch (stage_) {
    case Stage::Intro:
    case Stage::Retry: return question();
    case Stage::Question:
        stage_ = Stage::Waiting;
        deadline_us_ = now_us + 30000000;
        return {};
    case Stage::Result:
    case Stage::Goodbye: return exit();
    case Stage::CalibrationPrompt:
        stage_ = Stage::Normal;
        return {nullptr, Cue::None, true};
    default:
        stage_ = Stage::Normal;
        return {};
    }
}
MotionInteraction::Action MotionInteraction::event(nk_motion_event_t evt, int64_t now_us,
                                                   const char *normal_text, uint32_t (*random)()) {
    if (awaiting_speech() || evt == NK_MOTION_NONE) return {};
    if (quiz_) {
        if (now_us >= deadline_us_) return exit();
        if (evt == NK_MOTION_TAP3) {
            stage_ = Stage::Goodbye;
            return {"また遊んでね", Cue::None, false, true};
        }
        const bool yes = evt == NK_MOTION_NOD || evt == NK_MOTION_TAP1;
        const bool no = evt == NK_MOTION_TURN_LR || evt == NK_MOTION_TAP2;
        if (!yes && !no) {
            stage_ = Stage::Retry;
            return {kQuizRetry, Cue::None, false, true};
        }
        answers_ = (answers_ << 1) | (yes ? 1 : 0);
        if (++question_ < 3) return question();
        const auto index = nk_choose_message(3, previous_comment_[answers_], random);
        std::snprintf(result_, sizeof(result_), "相性は%uパーセント。%s",
                      kCompatibilityScores[answers_], kCompatibilityComments[answers_][index]);
        stage_ = Stage::Result;
        return {result_, Cue::None, false, true};
    }
    if (evt == NK_MOTION_SPIN180) {
        quiz_ = true;
        answers_ = question_ = 0;
        stage_ = Stage::Intro;
        return {kQuizIntro, Cue::None, false, true};
    }
    Cue cue = Cue::None;
    if (evt == NK_MOTION_TAP3) cue = Cue::Tap3;
    if (evt == NK_MOTION_CALIB_COMPLETE) cue = Cue::Calibrated;
    if (evt == NK_MOTION_CALIB_FAILED) cue = Cue::Failed;
    if (evt == NK_MOTION_WAKEUP) cue = Cue::Wakeup;
    if (normal_text && *normal_text) {
        stage_ = evt == NK_MOTION_TAP3 ? Stage::CalibrationPrompt : Stage::NormalSpeech;
        return {normal_text, cue, false, evt == NK_MOTION_TAP3};
    }
    // Explicit custom silence still permits calibration after the cue.
    return {normal_text, cue, evt == NK_MOTION_TAP3};
}
