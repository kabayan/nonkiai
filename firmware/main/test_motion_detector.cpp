#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <map>
#include "motion_detector.h"

struct Row {
    std::string label;
    int trial;
    std::string phase;
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <csv_path>" << std::endl;
        return 2;
    }
    std::string csv_path = argv[1];
    std::ifstream file(csv_path);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << csv_path << std::endl;
        return 1;
    }

    std::string line;
    std::map<std::pair<std::string, int>, std::vector<Row>> groups;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("label,trial", 0) == 0) continue;
        std::stringstream ss(line);
        std::string token;
        std::vector<std::string> cols;
        while (std::getline(ss, token, ',')) {
            cols.push_back(token);
        }
        if (cols.size() < 12) continue;

        Row r;
        r.label = cols[0];
        r.trial = std::stoi(cols[1]);
        r.phase = cols[2];
        r.ax = (int16_t)std::stoi(cols[6]);
        r.ay = (int16_t)std::stoi(cols[7]);
        r.az = (int16_t)std::stoi(cols[8]);
        r.gx = (int16_t)std::stoi(cols[9]);
        r.gy = (int16_t)std::stoi(cols[10]);
        r.gz = (int16_t)std::stoi(cols[11]);

        groups[{r.label, r.trial}].push_back(r);
    }

    std::map<std::string, int> total_trials;
    std::map<std::string, int> passed_trials;
    std::map<std::string, std::map<std::string, int>> detected_counts;

    for (auto& pair : groups) {
        const std::string& label = pair.first.first;
        int trial = pair.first.second;
        const auto& rows = pair.second;

        total_trials[label]++;

        std::vector<Row> prep_rows;
        for (const auto& r : rows) {
            if (r.phase == "prep") prep_rows.push_back(r);
        }
        double sum_ax = 0, sum_ay = 0, sum_az = 0;
        int prep_n = 0;
        int start_idx = (prep_rows.size() > 200) ? ((int)prep_rows.size() - 200) : 0;
        for (size_t i = start_idx; i < prep_rows.size(); ++i) {
            sum_ax += prep_rows[i].ax;
            sum_ay += prep_rows[i].ay;
            sum_az += prep_rows[i].az;
            prep_n++;
        }

        MotionDetector detector;
        if (prep_n > 0) {
            detector.set_base_acc((float)(sum_ax / prep_n * MotionDetector::kAccelRes),
                                  (float)(sum_ay / prep_n * MotionDetector::kAccelRes),
                                  (float)(sum_az / prep_n * MotionDetector::kAccelRes));
        }

        std::vector<std::string> detected;
        for (const auto& r : rows) {
            if (r.phase == "record") {
                nk_motion_event_t evt = detector.update(r.ax, r.ay, r.az, r.gx, r.gy, r.gz);
                if (evt != NK_MOTION_NONE) {
                    detected.push_back(nk_motion_event_name(evt));
                }
            }
        }
        if (detector.state() == MotionDetector::State::GESTURE_ACTIVE) {
            nk_motion_event_t evt = detector.flush();
            if (evt != NK_MOTION_NONE) {
                detected.push_back(nk_motion_event_name(evt));
            }
        }

        for (const auto& d : detected) {
            detected_counts[label][d]++;
        }

        bool is_neg = (label == "baseline" || label == "carry" || label == "putdown" || label == "bump");
        bool match = false;
        if (is_neg) {
            match = detected.empty();
        } else {
            for (const auto& d : detected) {
                if (d == label) {
                    match = true;
                    break;
                }
            }
        }
        if (match) passed_trials[label]++;
    }

    std::cout << "\n================ C++ MotionDetector Test Results (with flush) ================\n";
    std::cout << "Label        Trials  Passed  Accuracy  Detections\n";
    std::cout << "--------------------------------------------------------------\n";
    for (const auto& p : total_trials) {
        const std::string& label = p.first;
        int tot = p.second;
        int pass = passed_trials[label];
        float acc = (float)pass / tot * 100.0f;
        std::cout << label;
        for (size_t i = label.length(); i < 13; ++i) std::cout << " ";
        std::cout << tot << "       " << pass << "       " << (int)(acc + 0.5f) << "%      ";
        for (const auto& dc : detected_counts[label]) {
            std::cout << dc.first << ":" << dc.second << " ";
        }
        std::cout << "\n";
    }
    return 0;
}
