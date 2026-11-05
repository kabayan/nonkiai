#pragma once
#include "motion_detector.h"

// Pure state machine; the sampler performs speech, cues and calibration actions.
class MotionInteraction {
public:
    enum class Cue { None, Tap3, Calibrated, Failed, Wakeup, Exit };
    struct Action {
        const char *text = nullptr;
        Cue cue = Cue::None;
        bool calibrate = false;
        bool exact = false; // no random leading filler for quiz instructions
    };
    Action event(nk_motion_event_t event, int64_t now_us, const char *normal_text,
                 uint32_t (*random)());
    Action speech_finished(int64_t now_us, bool completed);
    Action tick(int64_t now_us);
    bool quiz() const { return quiz_; }
    bool awaiting_speech() const { return stage_ != Stage::Normal && stage_ != Stage::Waiting; }
private:
    enum class Stage { Normal, NormalSpeech, CalibrationPrompt, Intro, Question, Retry, Waiting, Result, Goodbye };
    Action exit();
    Action question();
    Stage stage_ = Stage::Normal;
    bool quiz_ = false;
    unsigned question_ = 0;
    unsigned answers_ = 0;
    int previous_comment_[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
    int64_t deadline_us_ = 0;
    char result_[128] = {};
};
