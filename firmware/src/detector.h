// On-device presence detection from CSI amplitudes.
//
// Motion score = average per-subcarrier standard deviation of the (gain-normalised)
// amplitudes over a sliding window, x100. An empty room gives a low, steady score;
// people moving make the channel fluctuate and the score rises.
//
// Calibration records the score of an empty room; the threshold is that level
// times a sensitivity margin. Both are kept in flash across reboots.

#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include <algorithm>

class PresenceDetector {
 public:
  static const int N_SUB = 64;
  static const int WINDOW = 200;         // samples (~2 s at 100 samples/s)
  static const int MIN_SAMPLES = 6;
  static const uint32_t HOLD_MS = 10000; // stay present this long after the last movement
  static const uint32_t COUNTDOWN_MS = 10000;
  static const uint32_t RECORD_MS = 30000;
  static const int MAX_CALIB = 400;

  enum CalibState { CALIB_IDLE, CALIB_COUNTDOWN, CALIB_RECORDING };

  void begin() {
    prefs.begin("presence", false);
    baseline = prefs.getFloat("baseline", 0);
    margin = prefs.getFloat("margin", 1.5f);
    if (baseline <= 0) startCalibration();  // first run: learn the empty room
  }

  void addSample(uint32_t ms, const uint8_t *amp) {
    if (!maskReady) {
      nActive = 0;
      for (int i = 0; i < N_SUB; i++)
        if (amp[i] > 0) active[nActive++] = i;
      if (nActive == 0) return;
      maskReady = true;
    }
    float mean = 0;
    for (int k = 0; k < nActive; k++) mean += amp[active[k]];
    mean /= nActive;
    if (mean <= 0) return;
    for (int k = 0; k < nActive; k++) buf[head][k] = amp[active[k]] / mean;
    stamps[head] = ms;
    head = (head + 1) % WINDOW;
    if (count < WINDOW) count++;
    received++;
  }

  // Call every ~200 ms.
  void update(uint32_t now) {
    if (count >= MIN_SAMPLES) {
      score = computeScore();
      hasScore = true;
    }
    updateRate();

    if (calib == CALIB_COUNTDOWN && now - calibStart >= COUNTDOWN_MS) {
      calib = CALIB_RECORDING;
      calibStart = now;
      nCalib = 0;
    } else if (calib == CALIB_RECORDING) {
      if (hasScore && nCalib < MAX_CALIB) calibScores[nCalib++] = score;
      if (now - calibStart >= RECORD_MS) finishCalibration();
    }

    if (calib == CALIB_IDLE && hasScore && baseline > 0 && score > threshold()) lastMotion = now;
    present = calib == CALIB_IDLE && lastMotion > 0 && now - lastMotion < HOLD_MS;
  }

  void startCalibration() {
    calib = CALIB_COUNTDOWN;
    calibStart = millis();
    lastMotion = 0;
  }

  void setMargin(float m) {
    margin = constrain(m, 1.05f, 5.0f);
    prefs.putFloat("margin", margin);
  }

  float threshold() const { return baseline * margin; }
  uint32_t calibRemainingMs(uint32_t now) const {
    uint32_t total = calib == CALIB_COUNTDOWN ? COUNTDOWN_MS : RECORD_MS;
    uint32_t el = now - calibStart;
    return el >= total ? 0 : total - el;
  }

  bool present = false;
  bool hasScore = false;
  float score = 0;
  float baseline = 0;
  float margin = 1.5f;
  float rate = 0;
  uint32_t lastMotion = 0;
  CalibState calib = CALIB_IDLE;

 private:
  float computeScore() {
    float total = 0;
    for (int k = 0; k < nActive; k++) {
      float s = 0, s2 = 0;
      for (int j = 0; j < count; j++) {
        float v = buf[j][k];
        s += v;
        s2 += v * v;
      }
      float m = s / count;
      float var = s2 / count - m * m;
      total += var > 0 ? sqrtf(var) : 0;
    }
    return total / nActive * 100;
  }

  void updateRate() {
    if (count < 2) return;
    int newest = (head + WINDOW - 1) % WINDOW;
    int oldest = count < WINDOW ? 0 : head;
    uint32_t span = stamps[newest] - stamps[oldest];
    if (span > 0) rate = (count - 1) * 1000.0f / span;
  }

  void finishCalibration() {
    calib = CALIB_IDLE;
    if (nCalib < 10) return;  // not enough data; keep the previous baseline
    std::sort(calibScores, calibScores + nCalib);
    baseline = calibScores[(int)(nCalib * 0.95f)];
    prefs.putFloat("baseline", baseline);
    lastMotion = 0;
  }

  Preferences prefs;
  float buf[WINDOW][N_SUB];
  uint32_t stamps[WINDOW];
  int head = 0, count = 0;
  uint32_t received = 0;
  int active[N_SUB];
  int nActive = 0;
  bool maskReady = false;
  float calibScores[MAX_CALIB];
  int nCalib = 0;
  uint32_t calibStart = 0;
};
