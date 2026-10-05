#ifndef PLANNER_CALIBRATION_H
#define PLANNER_CALIBRATION_H

/* Calibration tables for the Step 3.5 recovery planner: monotone (isotonic)
 * maps from a source's raw score to the probability that the class's fault is
 * actually active. 32 equal-width bins on [0, 1]; a score s uses bin
 * min(31, (int)(s * 32)).
 *
 * Fitted on DEV seeds 1001-1020 only, from the planner's own calibration dump
 * (--planner-calib), by laptop/tools/planner_calibration_fit.py - never edit by
 * hand. This file is the identity map until that script has been run. The AI
 * model and ml/ are untouched; this layer sits after fault_predictor_run(). */
#define PLANNER_CAL_BINS 32

/* Rows: class (0 mobility, 1 communication, 2 resource). */
static const float PLANNER_CAL_AI[3][PLANNER_CAL_BINS] = {
    { 0.0051f, 0.0095f, 0.0152f, 0.0188f, 0.0202f, 0.0202f, 0.0202f, 0.0202f, 0.0202f, 0.0202f, 0.0202f, 0.0202f, 0.0202f, 0.0202f, 0.0202f, 0.0202f, 0.0202f, 0.0202f, 0.0278f, 0.0831f, 0.0845f, 0.1304f, 0.1390f, 0.1439f, 0.1439f, 0.1476f, 0.1476f, 0.1633f, 0.2492f, 0.3640f, 0.5213f, 0.8424f },
    { 0.0002f, 0.2638f, 0.2638f, 0.2638f, 0.2638f, 0.2638f, 0.2638f, 0.2646f, 0.2646f, 0.2646f, 0.3000f, 0.3168f, 0.3168f, 0.3168f, 0.3168f, 0.3168f, 0.3168f, 0.3168f, 0.3946f, 0.3946f, 0.3946f, 0.3946f, 0.3995f, 0.3995f, 0.3995f, 0.3995f, 0.3995f, 0.3995f, 0.3995f, 0.3995f, 0.3995f, 0.6067f },
    { 0.0007f, 0.0221f, 0.0398f, 0.0474f, 0.0654f, 0.0802f, 0.0904f, 0.1137f, 0.1137f, 0.1167f, 0.1210f, 0.1389f, 0.1389f, 0.1499f, 0.1499f, 0.1547f, 0.1574f, 0.1629f, 0.1720f, 0.1740f, 0.1740f, 0.1923f, 0.2035f, 0.2128f, 0.2207f, 0.2498f, 0.2880f, 0.3166f, 0.3241f, 0.3875f, 0.5432f, 0.6638f },
};
/* Watchdog anomaly_confidence and consensus severity y, per class, fitted on the dump of the build they are
 * used in: the fixed-gate dynamics differ between an AI-on and an AI-off build, so each build has its own
 * table (_AIBUILD from the FAULT_MODEL_LOADED=1 dump, _NOAI from the AI-off dump). */
/* Watchdog, per class, used when its dominant failure mode maps to that class. */
static const float PLANNER_CAL_WD_AIBUILD[3][PLANNER_CAL_BINS] = {
    { 0.0750f, 0.4836f, 0.6751f, 0.6751f, 0.6751f, 0.7139f, 0.7568f, 0.8232f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f, 0.8467f },
    { 0.1847f, 0.1847f, 0.2855f, 0.3864f, 0.3864f, 0.3864f, 0.3864f, 0.5039f, 0.5039f, 0.6129f, 0.6129f, 0.6129f, 0.6129f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f },
    { 0.0769f, 0.0769f, 0.1006f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f, 0.1032f },
};
static const float PLANNER_CAL_WD_NOAI[3][PLANNER_CAL_BINS] = {
    { 0.2353f, 0.4287f, 0.6288f, 0.6288f, 0.6288f, 0.6414f, 0.6899f, 0.7797f, 0.8305f, 0.8305f, 0.8305f, 0.8305f, 0.8305f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f },
    { 0.2101f, 0.2101f, 0.3352f, 0.3805f, 0.3805f, 0.3805f, 0.3805f, 0.4417f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f, 0.5789f },
    { 0.0871f, 0.0871f, 0.1033f, 0.1033f, 0.1033f, 0.1033f, 0.1033f, 0.1033f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f, 0.1304f },
};
/* Consensus FTI severity y, rows: 0 mobility, 1 communication. */
static const float PLANNER_CAL_CONS_AIBUILD[2][PLANNER_CAL_BINS] = {
    { 0.0376f, 0.0376f, 0.0376f, 0.0376f, 0.0376f, 0.0376f, 0.0376f, 0.0376f, 0.0376f, 0.0376f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f, 0.5241f },
    { 0.0127f, 0.0127f, 0.0127f, 0.0127f, 0.0127f, 0.0127f, 0.0127f, 0.0127f, 0.0127f, 0.0127f, 0.0127f, 0.0127f, 0.0127f, 0.0127f, 0.2855f, 0.2855f, 0.2855f, 0.2855f, 0.2855f, 0.2855f, 0.2855f, 0.2855f, 0.2855f, 0.3529f, 0.3529f, 0.3529f, 0.3529f, 0.3529f, 0.3529f, 0.3529f, 0.3529f, 0.3529f },
};
static const float PLANNER_CAL_CONS_NOAI[2][PLANNER_CAL_BINS] = {
    { 0.0366f, 0.0366f, 0.0366f, 0.0366f, 0.0366f, 0.0366f, 0.0366f, 0.0366f, 0.0366f, 0.0366f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f, 0.5465f },
    { 0.0122f, 0.0122f, 0.0122f, 0.0122f, 0.0122f, 0.0122f, 0.0122f, 0.0122f, 0.0122f, 0.0122f, 0.0122f, 0.0122f, 0.0122f, 0.0122f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f, 0.3590f },
};

#if FAULT_MODEL_LOADED
#define PLANNER_CAL_WD PLANNER_CAL_WD_AIBUILD
#define PLANNER_CAL_CONS PLANNER_CAL_CONS_AIBUILD
#else
#define PLANNER_CAL_WD PLANNER_CAL_WD_NOAI
#define PLANNER_CAL_CONS PLANNER_CAL_CONS_NOAI
#endif

#endif
