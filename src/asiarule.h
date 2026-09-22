#pragma once

// AsianRule(亚洲规则)独立模块
// 亚洲象棋规则完整判决, 基于亚规35条
// https://dpxq.com/hldcg/AXFRules/AXFRules.htm

#include "position.h"

namespace Stockfish {

// 亚规判决: 输入skyrule框架的agg, 按亚规条文判决
// 返回+24999/-24999=违规, VALUE_DRAW=和棋
Value asian_judge_priority(const Position::SkyAgg& agg, Color stm, int loopLen, int ply);

// 条文4: 长杀检测——走子后是否形成杀势(下一步将死)
bool is_mate_threat(Color mover);

}  // namespace Stockfish
