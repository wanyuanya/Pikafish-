#pragma once

// SkyRule(天天象棋规则)独立模块
// 所有天天象棋规则相关的判定函数集中在此

#include "position.h"

namespace Stockfish {

// SkyRule全局状态(在skyrule.cpp定义)
extern bool     skySplitAllowed;
extern int      skyMoveCheckW;
extern int      skyMoveCheckB;
extern int      skyChaseCnt[COLOR_NB];
extern uint32_t skyChaseLast[COLOR_NB];
extern bool     skyPreLoopChase[2];

// 阈值查询
inline int sky_check_limit(int pieceCount) {
    if (pieceCount <= 1) return 6;
    if (pieceCount == 2) return 12;
    return 18;
}
inline int sky_check_chase_limit(int pieceCount) {
    if (pieceCount <= 1) return 12;
    return 18;
}

}  // namespace Stockfish
