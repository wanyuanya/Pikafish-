// AsianRule(亚洲规则)独立实现
// 旧亚规chased/detect_chases fallback
#include "position.h"
#include <cstring>
namespace Stockfish {
using namespace Attacks;
bool Position::chase_legal(Move m) const {

    assert(m.is_ok());

    Color    us       = sideToMove;
    Square   from     = m.from_sq();
    Square   to       = m.to_sq();
    Bitboard occupied = (pieces() ^ from) | to;

    assert(color_of(moved_piece(m)) == us);
    assert(piece_on(king_square(us)) == make_piece(us, KING));

    // If the moving piece is a king, check whether the destination
    // square is not under new attack after the move.
    if (type_of(piece_on(from)) == KING)
        return !(checkers_to(~us, to, occupied));

    // A non-king move is chase legal if the king is not under new attack after the move.
    return !(checkers_to(~us, king_square(us), occupied) & ~square_bb(to));
}


// Calculates the chase information for a given color.
u16 Position::chased(Color c) {

    u16 chase = 0;

    std::swap(c, sideToMove);

    // King and pawn can legally perpetual chase
    Bitboard attackers = pieces(sideToMove) ^ pieces(sideToMove, KING, PAWN);
    while (attackers)
    {
        Square    from         = pop_lsb(attackers);
        PieceType attackerType = type_of(piece_on(from));
        Bitboard  attacks      = attacks_bb(attackerType, from, pieces());

        // Restrict to pinners if pinned, otherwise exclude attacks on unpromoted pawns and checks
        if (blockers_for_king(sideToMove) & from)
            attacks &= pinners(~sideToMove) & ~pieces(KING);
        else
            attacks &= (pieces(~sideToMove) ^ pieces(~sideToMove, KING, PAWN))
                     | (pieces(~sideToMove, PAWN) & HalfBB[sideToMove]);

        while (attacks)
        {
            Square to = pop_lsb(attacks);
            Move   m  = Move(from, to);

            if (chase_legal(m))
            {
                // 条文26: 车长捉已渡河兵卒→负(无论是否反捉)
                if (attackerType == ROOK && type_of(piece_on(to)) == PAWN
                    && (HalfBB[sideToMove] & to))
                    chase |= (1 << idBoard[to]);
                // Attacks against stronger pieces
                else if ((attackerType == KNIGHT || attackerType == CANNON)
                    && type_of(piece_on(to)) == ROOK)
                    chase |= (1 << idBoard[to]);
                else if ((attackerType == ADVISOR || attackerType == BISHOP)
                         && type_of(piece_on(to)) & 1)
                    chase |= (1 << idBoard[to]);
                // Attacks against potentially unprotected pieces
                else
                {
                    bool trueChase             = true;
                    const auto& [captured, id] = do_move(m);
                    Bitboard recaptures        = attackers_to(to) & pieces(sideToMove);
                    while (recaptures)
                    {
                        Square s = pop_lsb(recaptures);
                        if (chase_legal(Move(s, to)))
                        {
                            trueChase = false;
                            break;
                        }
                    }
                    undo_move(m, captured, id);

                    if (trueChase)
                    {
                        // Exclude mutual/symmetric attacks except pins
                        if (attackerType == type_of(piece_on(to)))
                        {
                            sideToMove = ~sideToMove;
                            bool canRecap = chase_legal(Move(to, from));
                            bool knightBlocked = (attackerType == KNIGHT
                                && ((between_bb(from, to) ^ to) & pieces()));
                            if (knightBlocked || !canRecap)
                                chase |= (1 << idBoard[to]);
                            else {
                                // 条文31: 兼兑检测——对方能吃回, 但吃回后攻击者方有其他子反捉
                                // 模拟对方吃回 from, 检查攻击者方是否有子攻击 from
                                Move recapMove(to, from);
                                const auto& [cap2, id2] = do_move(recapMove);
                                Bitboard counter = attackers_to(from) & pieces(sideToMove);
                                bool counterTakes = false;
                                while (counter) {
                                    Square cs = pop_lsb(counter);
                                    if (chase_legal(Move(cs, from))) {
                                        counterTakes = true;
                                        break;
                                    }
                                }
                                undo_move(recapMove, cap2, id2);
                                if (counterTakes)
                                    chase |= (1 << idBoard[to]);  // 兼兑仍长捉
                            }
                            sideToMove = ~sideToMove;
                        }
                        else
                            chase |= (1 << idBoard[to]);
                    }
                }
            }
        }
    }

    std::swap(c, sideToMove);

    return chase;
}

// SkyRule 新框架: 走完 m 后, mover 方真捉的无根子id位图
// 用户明确: 有根=对方有任意子能吃回, 不查牵制
// 亚规: 马炮捉车即使有根也算真捉
// 关键: 不用轻量do_move/undo_move(破坏byTypeBB), 用move_piece/remove_piece同步byTypeBB
Value Position::detect_chases(int d, int ply) {

    // Grant each piece on board a unique id for each side
    int whiteId = 0;
    int blackId = 0;
    for (Square s = SQ_A0; s <= SQ_I9; ++s)
        if (board[s] != NO_PIECE)
            idBoard[s] = color_of(board[s]) == WHITE ? whiteId++ : blackId++;

    Color us = sideToMove, them = ~us;

    // Rollback until we reached st - d
    u16 chase[COLOR_NB] = {0xFFFF, 0xFFFF};
    for (int i = 0; i < d; ++i)
    {
        if (st->checkersBB)
        {
            return VALUE_DRAW;
        }
        else if (!chase[~sideToMove])
        {
            if (!chase[sideToMove])
                break;
            undo_move(st->move, st->capturedPiece);
            st = st->previous;
        }
        else
        {
            u16 after = chased(~sideToMove);
            undo_move(st->move, st->capturedPiece);
            st = st->previous;
            // Take the exact diff to detect the chase
            u16 before = chased(sideToMove);
            chase[sideToMove] &= after & ~before;
        }
    }

    return bool(chase[us]) ^ bool(chase[them]) ? chase[us] ? Value(-24999) : Value(24999)
                                               : VALUE_DRAW;
}


// ============================================================================
// SkyRule(天天象棋规则) 循环判定
// 逐着分析循环中每步的性质: 将(check) / 捉(chase) / 闲(idle)。
// 关键: 用跨局面稳定的"棋子身份"追踪被捉子, 从而区分
//   - 步步捉同一个子(含该子来回逃, 车追移动炮) = 长捉同一子(禁止)
//   - 步步捉但目标身份在变(一子分捉多子)       = 分捉(对方纯闲时允许)
// ============================================================================
}  // namespace Stockfish

// ============================================================
// 亚洲象棋规则完整判决 (基于亚规35条)
// ============================================================
// 条文来源: 东萍象棋网亚洲象棋规则棋例细则
// https://dpxq.com/hldcg/AXFRules/AXFRules.htm
//
// 核心判决:
// 1.长将→负 2.双方长将→和 3.二将一还将→长将者负
// 4.长杀→和 5.一将一杀/一将一捉/一将一闲→和
// 6.长捉→负(单子长捉单子/两子多子轮捉一子)
// 7.马炮长捉有根车→负 8.分捉→和
// 9.兵卒长捉→和 10.将帅长捉→和
// 11.长拦/长兑/长献/长要抽吃→和 12.否则→和

#include "skyrule.h"
#include "movegen.h"

namespace Stockfish {

// 亚规判决: 输入skyrule框架的agg, 按亚规条文判决
Value asian_judge_priority(const Position::SkyAgg& agg, Color stm, int loopLen, int ply) {
    (void)ply;
    int half = loopLen / 2;

    auto loserScore = [&](Color loser) -> Value {
        return (loser == stm) ? Value(-24999) : Value(24999);
    };
    auto typeBit = [](PieceType pt) -> uint32_t { return 1u << unsigned(pt); };

    // 条文2: 双方循环长将(解将反将)→和
    bool bothCheck = (agg.ck[WHITE] == half && agg.ck[BLACK] == half);
    if (bothCheck) return VALUE_DRAW;

    // 条文1/3: 单方长将，或长将方遇到偶尔还将，长将方负。
    // 必须先处理双方长将，否则条文2会被前面的单方判断遮蔽。
    for (int c = 0; c < COLOR_NB; ++c)
    {
        if (agg.ck[c] != half)
            continue;

        int them = 1 - c;
        bool otherTTC = agg.kill[them] == half && agg.ck[them] == 0;
        bool otherChase = agg.ck[them] == 0
                       && agg.ch[them] + agg.responseChase[them] == half;

        // 条文6-8/11-14: 长将与长杀、长捉或应将中的反捉并存，按
        // “一将一杀/一将一捉/一将一闲”处理为和；纯单方长将仍判负。
        if (otherTTC || otherChase)
            return VALUE_DRAW;

        return loserScore((Color)c);
    }

    // 条文3: 二将一还将, 长将者负
    // (一方长将, 另一方偶尔还将) — 长将方ck==loopLen已被条文1捕获

    // 条文4-8: 长杀/一将一杀/一将一捉/一将一闲 → 作和
    // (亚规: 将捉交替作和, 不判负)
    // 优先级3: 将捉交替 → 亚规作和(不返回负)

    // 条文28: 长捉→负 (ch==half && ck==0 && 非分捉)
    for (int c = 0; c < COLOR_NB; ++c) {
        int them = 1 - c;
        bool cSplit = (agg.chaseUnion[c] != 0
                    && (agg.chaseIntersect[c] == 0 || agg.multiTargetSteps[c] > 0));
        bool themSplit = (agg.chaseUnion[them] != 0
                       && (agg.chaseIntersect[them] == 0
                           || agg.multiTargetSteps[them] > 0));
        bool themChase = (agg.ch[them] == half && agg.ck[them] == 0 && !themSplit);
        bool themTTC = agg.kill[them] == half && agg.ck[them] == 0;
        bool longChase = agg.ch[c] == half && agg.ck[c] == 0;

        if (!longChase || themChase || themTTC)
            continue;

        // 条文27: 同一方轮捉两个或多个目标是分捉，先于子力类别
        // 的特殊禁例判定，结果为和。否则“炮/马轮捉车”等规则会把
        // 本应属于分捉的局面误判为长捉犯例。
        if (cSplit)
            continue;

        bool targetRook = agg.targetTypeIntersect[c] & typeBit(ROOK);
        bool targetCannon = agg.targetTypeIntersect[c] & typeBit(CANNON);
        bool passedPawn = (agg.passedPawnIntersect[c] & agg.chaseIntersect[c]) != 0;
        uint32_t nonRookChasers = agg.chaserTypeUnion[c] & ~typeBit(ROOK);
        bool rookChaser = agg.chaserTypeUnion[c] & typeBit(ROOK);
        bool cannonOrKnightChaser = agg.chaserTypeUnion[c]
                                  & (typeBit(CANNON) | typeBit(KNIGHT));
        bool onlyRookChasers = rookChaser && !nonRookChasers;
        bool cannonConfinedRook = targetRook
                               && (agg.cannonConfinedUnion[c] & agg.chaseIntersect[c]);
        bool knightConfinedRook = targetRook
                               && (agg.knightConfinedUnion[c] & agg.chaseIntersect[c]);

        // 条文14: 被卧槽马牵制、不能移动的车，任何子长捉都判负。
        if (knightConfinedRook)
            return loserScore((Color)c);

        // 条文22: 炮牵制的车不能离线。马/炮对这类车的攻击仍是
        // 禁止的长捉；车沿受限线路的攻击则不作为捉子。
        if (cannonConfinedRook && cannonOrKnightChaser)
            return loserScore((Color)c);

        // 条文15/20/21/32: 炮、马轮捉车，或车帅被车炮控制时
        // 以马炮轮捉车，均不能用“有根/反捉”豁免。
        if (targetRook && cannonOrKnightChaser)
            return loserScore((Color)c);

        // 条文26/31: 车长捉已过河兵卒必须变着。
        if (passedPawn && rookChaser)
            return loserScore((Color)c);

        // 条文12/13: 车可长捉有根炮；循环中只要出现过有根炮，
        // 即使另一着变成无根，仍属允许循环。全程无根才判负。
        if (targetCannon && onlyRookChasers) {
            if (agg.chaseRootedSteps[c] > 0)
                continue;
            return loserScore((Color)c);
        }

        // 条文23/24/25: 将帅或兵卒参与长捉时，若存在至少一步
        // 没有与其它子联合，属于允许的长捉；若每一步都联合，
        // 则按多子长捉判负。
        bool hasMultipleChasers = popcount(agg.chaserUnion[c]) > 1;
        bool specialOnly = agg.chaseSpecialOnly[c] > 0;
        bool specialAlwaysJoined = agg.chaseMixed[c] == half;
        if (specialOnly && !specialAlwaysJoined)
            continue;

        // 条文19: 同类子互能合法吃回，属于长献/长兑，作和；
        // 若循环中始终不能吃回，则落入一般长捉判负。
        bool sameType = (agg.targetTypeIntersect[c] & agg.chaserTypeIntersect[c]) != 0;
        bool mutualSameType = sameType
                           && (agg.exchangeIntersect[c] & agg.chaseIntersect[c]);
        if (mutualSameType)
            continue;

        // 条文30/35: 多子轮捉真根子不作为长捉；马炮捉车等
        // 特别禁例已经在上面优先处理。
        bool rootedTarget = (agg.rootedIntersect[c] & agg.chaseIntersect[c]) != 0;
        if (hasMultipleChasers && rootedTarget)
            continue;

        // 条文28/33: 两子或多子轮捉同一子，或单子持续捉同一
        // 个无根/假根目标，均属犯例。规则36规定兼兑不改变
        // 其“长捉”性质，exchangeIds 已在循环特征中保留。
        return loserScore((Color)c);
    }

    // 条文23/24: 兵卒/将帅长捉→和；其“每步均联合”反例已在
    // 上面的 specialAlwaysJoined 分支判为长捉犯例。

    // 条文27: 双方分捉→和
    // 条文33/34/35: 长要抽吃/长拦/长兑/长献→和

    // 默认: 作和
    return VALUE_DRAW;
}

// 条文4: 长杀检测——走子后是否形成杀势(下一步将死)
bool Position::is_mate_threat(Color mover) {
    Color them = ~mover;
    Square ksq = king_square(them);

    // A direct check is classified as check, not TTC.
    if (checkers_to(mover, ksq, pieces()))
        return false;

    // TTC is a threat which already has a legal mate-in-one continuation.
    // The loop extractor works on a rollback Position whose StateInfo points
    // into the real history.  Use a private state copy while generating and
    // making candidate checking moves so this probe cannot corrupt history.
    StateInfo* savedState = st;
    StateInfo  probeState = *st;
    Color      savedSide  = sideToMove;
    st = &probeState;
    sideToMove = mover;
    st->checkersBB = checkers_to(~sideToMove, king_square(sideToMove), pieces());
    set_check_info();

    bool threat = false;
    MoveList<LEGAL> moves(*this);
    for (const auto& move : moves)
    {
        if (!gives_check(move))
            continue;

        StateInfo next;
        do_move(move, next, nullptr);
        bool mate = bool(checkers()) && MoveList<LEGAL>(*this).size() == 0;
        undo_move(move);
        if (mate)
        {
            threat = true;
            break;
        }
    }

    st = savedState;
    sideToMove = savedSide;
    return threat;
}

}  // namespace Stockfish
