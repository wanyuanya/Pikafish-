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

namespace Stockfish {

// 亚规判决: 输入skyrule框架的agg, 按亚规条文判决
Value asian_judge_priority(const Position::SkyAgg& agg, Color stm, int loopLen, int ply) {
    (void)ply;
    int half = loopLen / 2;

    auto loserScore = [&](Color loser) -> Value {
        return (loser == stm) ? Value(-24999) : Value(24999);
    };

    // 条文1: 长将→负 (单方一子长将或多子交替长将)
    for (int c = 0; c < COLOR_NB; ++c)
        if (agg.ck[c] == loopLen)
            return loserScore((Color)c);

    // 条文2: 双方循环长将(解将反将)→和
    bool bothCheck = (agg.ck[WHITE] == loopLen && agg.ck[BLACK] == loopLen);
    if (bothCheck) return VALUE_DRAW;

    // 条文3: 二将一还将, 长将者负
    // (一方长将, 另一方偶尔还将) — 长将方ck==loopLen已被条文1捕获

    // 条文4-8: 长杀/一将一杀/一将一捉/一将一闲 → 作和
    // (亚规: 将捉交替作和, 不判负)
    // 优先级3: 将捉交替 → 亚规作和(不返回负)

    // 条文28: 长捉→负 (ch==half && ck==0 && 非分捉)
    for (int c = 0; c < COLOR_NB; ++c) {
        int them = 1 - c;
        bool cSplit = (agg.chaseIntersect[c] == 0 && agg.chaseUnion[c] != 0);
        bool themSplit = (agg.chaseIntersect[them] == 0 && agg.chaseUnion[them] != 0);
        bool themChase = (agg.ch[them] == half && agg.ck[them] == 0 && !themSplit);
        // 条文27: 分捉→和 (chaseIntersect==0)
        if (agg.ch[c] == half && cSplit && agg.ck[c] == 0)
            continue;  // 分捉, 和棋
        // 条文28: 长捉→负
        if (agg.ch[c] == half && agg.ck[c] == 0 && !cSplit && !themChase)
            return loserScore((Color)c);
    }

    // 条文23/24: 兵卒/将帅长捉→和 (由chased几何判定已过滤)

    // 条文27: 双方分捉→和
    // 条文33/34/35: 长要抽吃/长拦/长兑/长献→和

    // 默认: 作和
    return VALUE_DRAW;
}

// 条文4: 长杀检测——走子后是否形成杀势(下一步将死)
bool Position::is_mate_threat(Color mover) {
    Color them = ~mover;
    Square ksq = king_square(them);

    // 已将军→这步是将军不是杀
    if (checkers_to(mover, ksq, pieces()))
        return false;

    // 简化: 不做完整叫杀检测, 默认false(和棋覆盖)
    return false;
}

}  // namespace Stockfish

