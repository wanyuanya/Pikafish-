// SkyRule(天天象棋规则)独立实现
#include "skyrule.h"
#include <algorithm>
#include <cstring>

namespace Stockfish {
using namespace Attacks;
bool skyPreLoopChase[2] = {false, false};

uint32_t Position::sky_real_chase(Move m, Color mover) {

    Square from = m.from_sq();
    Square to   = m.to_sq();
    Piece  cap  = piece_on(to);
    Color  them = ~mover;
    uint32_t before = 0, after = 0;

    // 纯几何: them方子能否沿攻击线到达sq(直接读board[])
    auto can_recap = [&](Square sq) -> bool {
        int sr = rank_of(sq), sc = file_of(sq);
        // 车: 直线扫描
        for (int dr = -1; dr <= 1; ++dr)
          for (int dc = -1; dc <= 1; ++dc) {
            if (!dr && !dc) continue;
            if (dr && dc && (abs(dr) != abs(dc))) continue;  // 车只走横竖
            int r = sr + dr, c = sc + dc;
            while (r >= 0 && r <= 9 && c >= 0 && c <= 8) {
                Piece pc = piece_on(make_square(File(c), Rank(r)));
                if (pc != NO_PIECE) {
                    if (color_of(pc) == them && type_of(pc) == ROOK) return true;
                    break;
                }
                r += dr; c += dc;
            }
        }
        // 炮: 直线隔一子
        for (int dr = -1; dr <= 1; ++dr)
          for (int dc = -1; dc <= 1; ++dc) {
            if (!dr && !dc) continue;
            if (dr && dc && (abs(dr) != abs(dc))) continue;
            int r = sr + dr, c = sc + dc;
            int blockers = 0;
            while (r >= 0 && r <= 9 && c >= 0 && c <= 8) {
                Piece pc = piece_on(make_square(File(c), Rank(r)));
                if (pc != NO_PIECE) {
                    if (blockers == 1 && color_of(pc) == them && type_of(pc) == CANNON) return true;
                    blockers++;
                    if (blockers > 1) break;
                }
                r += dr; c += dc;
            }
        }
        // 马: 日字+蹩马腿
        static const int dr8[8] = {-2,-2,-1,1,2,2,1,-1};
        static const int dc8[8] = {-1,1,2,2,1,-1,-2,-2};
        static const int br8[8] = {-1,-1,0,0,1,1,0,0};
        static const int bc8[8] = {0,0,1,1,0,0,-1,-1};
        for (int i = 0; i < 8; ++i) {
            int r = sr - dr8[i], c = sc - dc8[i];
            if (r < 0 || r > 9 || c < 0 || c > 8) continue;
            Piece pc = piece_on(make_square(File(c), Rank(r)));
            if (pc == NO_PIECE || color_of(pc) != them || type_of(pc) != KNIGHT) continue;
            int br = sr - br8[i], bc = sc - bc8[i];
            if (br >= 0 && br <= 9 && bc >= 0 && bc <= 8 && piece_on(make_square(File(bc), Rank(br))) != NO_PIECE) continue;
            return true;
        }
        // 相/象: 田字+塞象眼+不过河
        for (int dr = -2; dr <= 2; dr += 4)
          for (int dc = -2; dc <= 2; dc += 4) {
            int r = sr - dr, c = sc - dc;
            if (r < 0 || r > 9 || c < 0 || c > 8) continue;
            Piece pc = piece_on(make_square(File(c), Rank(r)));
            if (pc == NO_PIECE || color_of(pc) != them || type_of(pc) != BISHOP) continue;
            // 塞象眼
            int er = sr - dr/2, ec = sc - dc/2;
            if (piece_on(make_square(File(ec), Rank(er))) != NO_PIECE) continue;
            // 不过河: 红相在0-4, 黑象在5-9
            if (them == WHITE && r > 4) continue;
            if (them == BLACK && r < 5) continue;
            return true;
        }
        // 士: 斜线一格
        for (int dr = -1; dr <= 1; dr += 2)
          for (int dc = -1; dc <= 1; dc += 2) {
            int r = sr - dr, c = sc - dc;
            if (r < 0 || r > 9 || c < 0 || c > 8) continue;
            Piece pc = piece_on(make_square(File(c), Rank(r)));
            if (pc == NO_PIECE || color_of(pc) != them || type_of(pc) != ADVISOR) continue;
            // 九宫内
            if (c < 3 || c > 5) continue;
            if (them == WHITE && r > 2) continue;
            if (them == BLACK && r < 7) continue;
            return true;
        }
        // 将: 直线一格
        for (int dr = -1; dr <= 1; ++dr)
          for (int dc = -1; dc <= 1; ++dc) {
            if (!dr && !dc) continue;
            if (dr && dc) continue;
            int r = sr - dr, c = sc - dc;
            if (r < 0 || r > 9 || c < 0 || c > 8) continue;
            Piece pc = piece_on(make_square(File(c), Rank(r)));
            if (pc == NO_PIECE || color_of(pc) != them || type_of(pc) != KING) continue;
            if (c < 3 || c > 5) continue;
            if (them == WHITE && r > 2) continue;
            if (them == BLACK && r < 7) continue;
            return true;
        }
        // 兵: 红兵往前(r+1), 过河后左右; 黑卒往后(r-1), 过河后左右
        // 红兵在(sr-1,sc)往前吃
        {
            Piece pc = piece_on(make_square(File(sc), Rank(sr-1)));
            if (pc != NO_PIECE && color_of(pc) == them && type_of(pc) == PAWN) {
                // 红兵往前不需要过河
                return true;
            }
        }
        // 黑卒在(sr+1,sc)往后吃
        {
            Piece pc = piece_on(make_square(File(sc), Rank(sr+1)));
            if (pc != NO_PIECE && color_of(pc) == them && type_of(pc) == PAWN) {
                return true;
            }
        }
        // 红兵左右吃(过河后): 红兵在(sr,sc±1), 过河=rank>=5
        for (int dc = -1; dc <= 1; dc += 2) {
            int c = sc + dc;
            if (c < 0 || c > 8) continue;
            Piece pc = piece_on(make_square(File(c), Rank(sr)));
            if (pc != NO_PIECE && color_of(pc) == WHITE && type_of(pc) == PAWN && sr >= 5)
                return true;
        }
        // 黑卒左右吃(过河后): 黑卒在(sr,sc±1), 过河=rank<=4
        for (int dc = -1; dc <= 1; dc += 2) {
            int c = sc + dc;
            if (c < 0 || c > 8) continue;
            Piece pc = piece_on(make_square(File(c), Rank(sr)));
            if (pc != NO_PIECE && color_of(pc) == BLACK && type_of(pc) == PAWN && sr <= 4)
                return true;
        }
        return false;
    };

    auto collect = [&]() -> uint32_t {
        uint32_t r = 0;
        Bitboard attackers = pieces(mover) ^ pieces(mover, KING, PAWN);
        while (attackers) {
            Square    af  = pop_lsb(attackers);
            PieceType at  = type_of(piece_on(af));
            Bitboard  att = attacks_bb(at, af, pieces());
            att &= (pieces(them) ^ pieces(them, KING, PAWN))
                 | (pieces(them, PAWN) & HalfBB[mover]);
            while (att) {
                Square sq = pop_lsb(att);
                bool npRook = (at == KNIGHT || at == CANNON) && type_of(piece_on(sq)) == ROOK;
                if (!can_recap(sq) || npRook)
                    r |= 1u << (idBoard[sq] & 31);
            }
        }
        return r;
    };

    before = collect();
    if (cap) remove_piece(to);
    move_piece(from, to);
    sideToMove = them;
    after = collect();
    sideToMove = mover;
    move_piece(to, from);
    if (cap) put_piece(cap, to);
    return after;  // 长捉是持续威胁, 不走diff
}

// SkyRule 新框架: 提取循环段每步特征
std::vector<Position::SkyStep> Position::sky_extract_loop(int loopLen) {

    std::vector<SkyStep> steps;
    steps.reserve(loopLen);

    // 建rollback副本
    Position rollback;
    memcpy((void*)&rollback, (const void*)this, offsetof(Position, filter));

    // 第1遍: 从当前st往前undo loopLen步到循环起点
    std::vector<Move> fwd;
    std::vector<int>  stepFilter;
    fwd.reserve(loopLen);
    stepFilter.reserve(loopLen);
    for (int k = 0; k < loopLen; ++k) {
        StateInfo* cur = rollback.st;
        Move m = cur->move;
        fwd.push_back(m);
        stepFilter.push_back((int)filter[cur->key]);
        Piece cap = cur->capturedPiece;
        rollback.sideToMove = ~rollback.sideToMove;
        rollback.move_piece(m.to_sq(), m.from_sq());
        if (cap) rollback.put_piece(cap, m.to_sq());
        rollback.st = cur->previous;
    }
    std::reverse(fwd.begin(), fwd.end());
    std::reverse(stepFilter.begin(), stepFilter.end());

    // 第2遍: 循环起点初始化idBoard(子身份固定)
    {
        int wId = 0, bId = 0;
        for (Square s = SQ_A0; s <= SQ_I9; ++s)
            if (rollback.board[s] != NO_PIECE) {
                Color c = color_of(rollback.board[s]);
                rollback.idBoard[s] = (c == WHITE) ? wId++ : bId++;
            }
    }

    // 记录preLoopChase: 循环起点局面双方是否在捉
    // 在sky_judge_loop里通过全局变量传回来
    {
        for (int c = 0; c < COLOR_NB; ++c) {
            Color mover = (Color)c;
            Color them = ~mover;
            Bitboard attackers = rollback.pieces(mover) ^ rollback.pieces(mover, KING, PAWN);
            bool hasChase = false;
            while (attackers && !hasChase) {
                Square af = pop_lsb(attackers);
                PieceType at = type_of(rollback.piece_on(af));
                Bitboard att = attacks_bb(at, af, rollback.pieces());
                att &= (rollback.pieces(them) ^ rollback.pieces(them, KING, PAWN));
                if (att) hasChase = true;
            }
            skyPreLoopChase[mover] = hasChase;
        }
    }

    // 第3遍: forward走loopLen步
    for (int k = 0; k < loopLen; ++k) {
        Move m = fwd[k];
        Color mover = rollback.sideToMove;
        bool resp = bool(rollback.checkers());

        // 走前记录子id, 走后跟随
        int moverId = rollback.idBoard[m.from_sq()];

        // 走将帅前: 遍历其他非将兵子记before
        bool moverIsKing = type_of(rollback.piece_on(m.from_sq())) == KING;
        uint32_t beforeOthers = 0;
        if (moverIsKing) {
            Bitboard others = rollback.pieces(mover) ^ rollback.pieces(mover, KING, PAWN);
            while (others) {
                Square s = pop_lsb(others);
                PieceType at = type_of(rollback.piece_on(s));
                Bitboard att = attacks_bb(at, s, rollback.pieces());
                att &= (rollback.pieces(~mover) ^ rollback.pieces(~mover, KING, PAWN))
                     | (rollback.pieces(~mover, PAWN) & HalfBB[mover]);
                while (att) {
                    Square sq = pop_lsb(att);
                    beforeOthers |= 1u << (rollback.idBoard[sq] & 31);
                }
            }
        }

        uint32_t chaseIds = rollback.sky_real_chase(m, mover);
        if (resp) chaseIds = 0;

        // 实际走m
        Piece cap = rollback.piece_on(m.to_sq());
        if (cap) rollback.remove_piece(m.to_sq());
        rollback.move_piece(m.from_sq(), m.to_sq());
        rollback.sideToMove = ~rollback.sideToMove;

        // idBoard跨步跟随: 子从from到to, id不变
        rollback.idBoard[m.to_sq()]   = moverId;
        rollback.idBoard[m.from_sq()] = -1;

        bool isCheck = bool(rollback.checkers());

        // expose差集: 将帅走开后其他子新产生的攻击
        bool expose = false;
        if (moverIsKing) {
            uint32_t afterOthers = 0;
            Bitboard others = rollback.pieces(mover) ^ rollback.pieces(mover, KING, PAWN);
            while (others) {
                Square s = pop_lsb(others);
                PieceType at = type_of(rollback.piece_on(s));
                Bitboard att = attacks_bb(at, s, rollback.pieces());
                att &= (rollback.pieces(~mover) ^ rollback.pieces(~mover, KING, PAWN))
                     | (rollback.pieces(~mover, PAWN) & HalfBB[mover]);
                while (att) {
                    Square sq = pop_lsb(att);
                    afterOthers |= 1u << (rollback.idBoard[sq] & 31);
                }
            }
            expose = (afterOthers & ~beforeOthers) != 0;
            if (getenv("SKYDBG"))
                fprintf(stderr, "    expose mover=%d before=0x%x after=0x%x diff=0x%x\n",
                        (int)mover, beforeOthers, afterOthers, afterOthers & ~beforeOthers);
        }

        if (getenv("SKYDBG"))
            fprintf(stderr, "  step mover=%d isCheck=%d chase=0x%x exp=%d resp=%d\n",
                    (int)mover,(int)isCheck,chaseIds,(int)expose,(int)resp);

        steps.push_back({mover, isCheck, chaseIds, expose, resp, m.from_sq(), stepFilter[k] >= 2});
    }
    return steps;
}

// SkyRule 新框架: 聚合循环段每步特征
Position::SkyAgg Position::sky_aggregate(const std::vector<SkyStep>& steps, int loopLen) {

    SkyAgg agg;
    (void)loopLen;

    // 将军子from位置去重集(按方)
    uint64_t checkFromSet[COLOR_NB] = {0, 0};

    for (const SkyStep& s : steps)
    {
        Color c = s.mover;

        if (s.isCheck)
        {
            agg.ck[c]++;
            checkFromSet[c] |= (1ull << (int(s.from) & 63));
        }
        else if (s.chaseIds && !s.resp)
        {
            agg.ch[c]++;
            agg.chaseIntersect[c] &= s.chaseIds;
            agg.chaseUnion[c]     |= s.chaseIds;
        }
        else
        {
            agg.idle[c]++;
        }

        if (s.expose)
            agg.expose[c]++;
    }

    // 将军子from位置去重数
    for (int c = 0; c < COLOR_NB; ++c)
    {
        int cnt = 0;
        uint64_t v = checkFromSet[c];
        while (v) { cnt++; v &= v - 1; }
        agg.checkPieceCount[c] = cnt;
        agg.preLoopChase[c] = skyPreLoopChase[c];
    }

    return agg;
}

// SkyRule 新框架: 优先级判决
// 返回VALUE_DRAW=和棋, +24999/-24999=违规, VALUE_NONE=未达阈值
Value Position::sky_judge_priority(const SkyAgg& agg, Color stm, int loopLen, int ply) {

    (void)ply;
    int half = loopLen / 2;  // 循环回合数

    fprintf(stderr, "SKYDBG loopLen=%d half=%d stm=%d ply=%d ckW=%d chW=%d exW=%d intW=0x%x uniW=0x%x ckB=%d chB=%d exB=%d intB=0x%x uniB=0x%x preW=%d preB=%d\n",
            loopLen, half, (int)stm, ply,
            agg.ck[WHITE], agg.ch[WHITE], agg.expose[WHITE], agg.chaseIntersect[WHITE], agg.chaseUnion[WHITE],
            agg.ck[BLACK], agg.ch[BLACK], agg.expose[BLACK], agg.chaseIntersect[BLACK], agg.chaseUnion[BLACK],
            (int)agg.preLoopChase[WHITE], (int)agg.preLoopChase[BLACK]);

    // 判定某方是否违规
    // loserColor = 违规方, 返回对应分数
    auto loserScore = [&](Color loser) -> Value {
        // stm视角: 违规方是stm自己→-24999, 是对方→+24999
        return (loser == stm) ? Value(-24999) : Value(24999);
    };

    // 优先级1: 长将, 2-fold即判
    for (int c = 0; c < COLOR_NB; ++c)
    {
        if (agg.ck[c] == loopLen)
            return loserScore((Color)c);
    }

    // 优先级2: 露捉, 且对方非长将
    for (int c = 0; c < COLOR_NB; ++c)
    {
        int them = 1 - c;
        bool themLongCheck = (agg.ck[them] == loopLen);
        if (agg.expose[c] > 0 && !themLongCheck)
            return loserScore((Color)c);
    }

    // 优先级3: 将捉交替, 2-fold即判
    for (int c = 0; c < COLOR_NB; ++c)
    {
        if (agg.ck[c] > 0 && agg.ch[c] > 0 && agg.idle[c] == 0)
            return loserScore((Color)c);
    }

    // 优先级4: 长捉, 对方不长捉时才判
    for (int c = 0; c < COLOR_NB; ++c)
    {
        int them = 1 - c;
        bool cSplit    = (agg.chaseIntersect[c] == 0 && agg.chaseUnion[c] != 0);
        bool themSplit = (agg.chaseIntersect[them] == 0 && agg.chaseUnion[them] != 0);
        bool themChase = (agg.ch[them] == half && agg.ck[them] == 0 && !themSplit);
        if (agg.ch[c] == half && agg.ck[c] == 0 && !cSplit && !themChase)
            return loserScore((Color)c);
    }

    // 优先级5: 分捉多子
    for (int c = 0; c < COLOR_NB; ++c)
    {
        bool cSplit = (agg.chaseIntersect[c] == 0 && agg.chaseUnion[c] != 0);
        if (agg.ch[c] == half && cSplit && agg.ck[c] == 0)
        {
            int them = 1 - c;
            if (agg.ck[them] > 0)
                return loserScore((Color)c);
            return VALUE_NONE;
        }
    }

    // 优先级6: 平级tiebreak: 双方对称时, 循环前先捉方变
    for (int c = 0; c < COLOR_NB; ++c)
    {
        int them = 1 - c;
        bool cSplit    = (agg.chaseIntersect[c] == 0 && agg.chaseUnion[c] != 0);
        bool themSplit = (agg.chaseIntersect[them] == 0 && agg.chaseUnion[them] != 0);
        bool cChase    = (agg.ch[c] == half && agg.ck[c] == 0 && !cSplit);
        bool themChase = (agg.ch[them] == half && agg.ck[them] == 0 && !themSplit);
        bool cMix      = (agg.ck[c] > 0 && agg.ch[c] > 0 && agg.idle[c] == 0);
        bool themMix   = (agg.ck[them] > 0 && agg.ch[them] > 0 && agg.idle[them] == 0);
        if (((cChase && themChase) || (cMix && themMix)) && agg.preLoopChase[c])
            return loserScore((Color)c);
    }

    // 优先级7: 都不违规
    fprintf(stderr, "SKYDBG PRIORITY7 loopLen=%d half=%d stm=%d ckW=%d chW=%d ckB=%d chB=%d\n",
            loopLen, half, (int)stm,
            agg.ck[WHITE], agg.ch[WHITE],
            agg.ck[BLACK], agg.ch[BLACK]);
    return VALUE_DRAW;
}

// SkyRule: 和chased()逻辑相同，但按位置(Bitboard)返回被捉子集合
// 用于并行规则：两个相同防守子交替补同一位置时，按位置算长捉
Bitboard Position::chased_positions(Color c) {

    Bitboard chase = 0;

    std::swap(c, sideToMove);

    Bitboard attackers = pieces(sideToMove) ^ pieces(sideToMove, KING, PAWN);
    while (attackers)
    {
        Square    from         = pop_lsb(attackers);
        PieceType attackerType = type_of(piece_on(from));
        Bitboard  attacks      = attacks_bb(attackerType, from, pieces());

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
                if ((attackerType == KNIGHT || attackerType == CANNON)
                    && type_of(piece_on(to)) == ROOK)
                    chase |= square_bb(to);
                else if ((attackerType == ADVISOR || attackerType == BISHOP)
                         && type_of(piece_on(to)) & 1)
                    chase |= square_bb(to);
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
                        if (attackerType == type_of(piece_on(to)))
                        {
                            sideToMove = ~sideToMove;
                            if ((attackerType == KNIGHT && ((between_bb(from, to) ^ to) & pieces()))
                                || !chase_legal(Move(to, from)))
                                chase |= square_bb(to);
                            sideToMove = ~sideToMove;
                        }
                        else
                            chase |= square_bb(to);
                    }
                }
            }
        }
    }

    std::swap(c, sideToMove);

    return chase;
}

bool skySplitAllowed = false;  // 分捉多子允许招法标志(全局)

// Detects chases from state st - d to state st
Value Position::sky_judge_loop(int loopLen, int ply) {

    // SkyRule 新框架: 先用新优先级表判决, 判出明确负分就返回, 其他fallback旧逻辑
    {
        auto steps = sky_extract_loop(loopLen);
        auto agg   = sky_aggregate(steps, loopLen);
        Value v    = sky_judge_priority(agg, sideToMove, loopLen, ply);
        if (v == Value(24999) || v == Value(-24999) || v == VALUE_DRAW)
            return v;
        // 新框架已判: VALUE_NONE(分捉允许)
        if (v == VALUE_NONE)
            skySplitAllowed = true;
        return v;
    }

    struct Agg {
        int ck = 0, ch = 0, idle = 0, kingChase = 0;
        bool hasStrong = false;  // 是否捉了车(强子)
        bool allMoverRook = true;  // 所有捉步的捉子都是车(车捉车互捉才豁免, 马炮捉车禁止)
        uint32_t intersect = 0xFFFFFFFFu;
        uint32_t uni = 0;
        uint32_t checkPiece = 0xFFFFFFFFu;
    };
    struct SI { Color mover; bool isCheck; uint32_t newIds; uint32_t afterIds; uint32_t checkId; bool moverKing; bool moverRook; bool wasInCheck; };

    Position rollback;
    memcpy((void*)&rollback, (const void*)this, offsetof(Position, filter));

    // SkyRule: chased()依赖idBoard[], 必须先按当前局面初始化(每方独立编号0-15)
    // 同时记录哪些id是车(强子), 用于区分长捉强子vs弱子
    uint32_t strongMask[2] = {0, 0};  // [color] = 车的id位掩码
    {
        int whiteId = 0, blackId = 0;
        for (Square s = SQ_A0; s <= SQ_I9; ++s)
            if (rollback.board[s] != NO_PIECE) {
                Color c = color_of(rollback.board[s]);
                int id = c == WHITE ? whiteId++ : blackId++;
                rollback.idBoard[s] = id;
                if (type_of(rollback.board[s]) == ROOK)
                    strongMask[c] |= (1u << id);
            }
    }

    const Color us = sideToMove;
    const Color them = ~us;

    // 在循环终点建立 位置->稳定身份 映射
    int posId[SQUARE_NB];
    for (int i = 0; i < SQUARE_NB; ++i) posId[i] = -1;
    int nextId = 0;
    for (Square s = SQ_A0; s <= SQ_I9; ++s)
        if (board[s] != NO_PIECE) posId[s] = nextId++;

    auto toIds = [&](Bitboard bb) -> uint32_t {
        uint32_t m = 0;
        while (bb) { Square s = pop_lsb(bb); if (posId[s] >= 0) m |= (1u << posId[s]); }
        return m;
    };

    std::vector<SI> steps;
    steps.reserve(loopLen);

    for (int k = 0; k < loopLen; ++k)
    {
        StateInfo* cur  = rollback.st;      // 走后局面
        Move       m    = cur->move;
        Color      mover = ~rollback.side_to_move();
        bool       isCheck = bool(cur->checkersBB);

        // 走后 mover 方白吃(真捉)的对方子 -> 身份(用原生chased())
        StateInfo* savedSt = rollback.st;
        u16 afterIds = rollback.chased(mover);
        rollback.st = savedSt;  // chased()内部do_move/undo_move会改st, 恢复

        Square toSq = m.to_sq(), fromSq = m.from_sq();
        int mid = posId[toSq];
        Piece captured = cur->capturedPiece;
        bool moverKing = type_of(rollback.piece_on(toSq)) == KING;  // undo前取走子类型
        bool moverRook = type_of(rollback.piece_on(toSq)) == ROOK;  // 车捉车互捉允许, 马炮捉车禁止
        rollback.undo_move(m, captured);   // 轻量回退到走前
        rollback.st = cur->previous;       // StateInfo 沿链回退
        // 身份映射同步回退(重复循环内不吃子)
        posId[toSq] = -1;
        if (mid >= 0) posId[fromSq] = mid;

        // 走前 mover 方白吃的对方子 -> 身份
        StateInfo* savedSt2 = rollback.st;
        u16 beforeIds = rollback.chased(mover);
        rollback.st = savedSt2;
        u16 newIds = afterIds & ~beforeIds;  // 这步新产生的捉
        // 走前mover是否被对方将军(应将判定)
        bool wasInCheck = bool(rollback.checkers());

        steps.push_back({mover, isCheck, (uint32_t)newIds, (uint32_t)afterIds, isCheck ? (uint32_t)(1u << posId[fromSq]) : 0u, moverKing, moverRook, wasInCheck});
    }
    std::reverse(steps.begin(), steps.end());   // 转为时间顺序

    // 循环前一步谁先捉(例7黑先捉判黑, 例12红先捉判红)
    bool preChaseW = false, preChaseB = false;
    {
        StateInfo* preSt = rollback.st;
        if (preSt && preSt->previous && preSt->move != Move())
        {
            Move preM = preSt->move;
            Piece preCap = preSt->capturedPiece;
            rollback.undo_move(preM, preCap);
            rollback.st = preSt->previous;
            StateInfo* s1 = rollback.st;
            u16 preW = rollback.chased(WHITE);
            rollback.st = s1;
            u16 preB = rollback.chased(BLACK);
            rollback.st = s1;
            preChaseW = (preW != 0);
            preChaseB = (preB != 0);
            rollback.do_move(preM, *preSt);
            rollback.st = preSt;
        }
    }

    Agg agg[COLOR_NB];
    int half = loopLen / 2;
    // 第一遍: 先收集每方将军步走后捉的目标(跨步一将一捉识别需要)
    uint32_t checkTarget[COLOR_NB] = {0, 0};
    for (const SI& s : steps)
        if (s.isCheck) checkTarget[s.mover] |= s.afterIds;
    for (const SI& s : steps)
    {
        Agg& g = agg[s.mover];
        uint32_t strongBits = s.newIds & strongMask[~s.mover];
        // 跨步一将一捉: 闲步虽无新捉, 但走后仍捉着将军步那个目标
        uint32_t persistIds = (!s.isCheck && s.afterIds & checkTarget[s.mover])
                              ? (s.afterIds & checkTarget[s.mover]) : 0u;
        if (s.isCheck)
        {
            // 天天规则: 将捉同时出现优先算将, 将军步不单独计ch
            g.ck++;
            g.checkPiece &= s.checkId;
        }
        else if (s.newIds)
        { g.ch++; g.intersect &= s.newIds; g.uni |= s.newIds; if (s.moverKing) g.kingChase++; if (strongBits) g.hasStrong = true; if (!s.moverRook) g.allMoverRook = false; }
        else if (persistIds && !(s.moverRook && (persistIds & strongMask[~s.mover])))
        {
            // 借将掩护持续捉同一子 = 一将一捉
            // 车捉车(走子是车且捉的是强子)互捉允许; 马炮捉车/捉弱子禁止
            g.ch++; g.intersect &= persistIds; g.uni |= persistIds; if (!s.moverRook) g.allMoverRook = false;
        }
        else
            g.idle++;
    }

    auto longCheck   = [&](Color c){ return agg[c].ck == half && agg[c].checkPiece != 0; };  // 单子连续将军才是长将
    auto multiKing   = [&](Color c){ return agg[c].ck == half && agg[c].checkPiece == 0; };  // 两子以上轮流将军
    auto hitMix      = [&](Color c){ return agg[c].idle == 0 && agg[c].ck > 0 && agg[c].ch > 0; };
    auto singleCheck = [&](Color c){ return hitMix(c) && agg[c].ck == 1; };
    auto multiCheck  = [&](Color c){ return hitMix(c) && agg[c].ck > 1; };
    auto longChase   = [&](Color c){ return agg[c].ck == 0 && agg[c].ch == half && agg[c].intersect != 0; };
    auto splitChase = [&](Color c){ return agg[c].ck == 0 && agg[c].ch == half && agg[c].intersect == 0; };
    auto kingChase  = [&](Color c){ return agg[c].ck == 0 && agg[c].ch == half && agg[c].kingChase == half; };
    auto level      = [&](Color c){
        if (longCheck(c)) return 3;
        if (multiKing(c)) return 2;  // 多子轮流将军也是禁止(12回合)
        if (kingChase(c)) return 3;
        if (multiCheck(c)) return 2;  // 多子将捉18回合(天天规则)
        if (singleCheck(c)) return agg[c].hasStrong ? 0 : 1;
        if (longChase(c))   return (agg[c].hasStrong && agg[c].allMoverRook) ? 0 : 1;
        return 0;
    };
    auto reasonFor  = [&](Color c)->const char* {
        if (longCheck(c)) return "长将";
        if (kingChase(c)) return "露捉";
        if (longChase(c)) return "长捉";
        if (hitMix(c))    return "将捉交替";
        if (splitChase(c))return "分捉多子";
        return "违规着法";
    };

    Value result = VALUE_DRAW;
    Color loser = COLOR_NB;
    const char* reason = nullptr;

    int lvUs = level(us), lvTh = level(them);

    if (lvUs > lvTh)
    {
        loser = us;   reason = reasonFor(us);
    }
    else if (lvTh > lvUs)
    {
        loser = them; reason = reasonFor(them);
    }
    else
    {
        // 双方都禁止(level=1): 综合判决
        if (lvUs == 1 && lvTh == 1)
        {
            bool usLong = (agg[us].ck == 0);
            bool thLong = (agg[them].ck == 0);
            bool usSplit = splitChase(us);
            bool thSplit = splitChase(them);

            // 1. 分捉多子且双方都不将军: 允许
            if ((usSplit || thSplit) && agg[us].ck == 0 && agg[them].ck == 0)
            { skySplitAllowed = true; return VALUE_NONE; }
            // 2. 分捉多子对方将军: 分捉方变
            if (usSplit && agg[them].ck > 0 && !longCheck(them))
            { loser = us; reason = "分捉多子"; }
            else if (thSplit && agg[us].ck > 0 && !longCheck(us))
            { loser = them; reason = "分捉多子"; }
            // 3. 一方将捉一方长捉: 综合循环前状态+有根无根
            else if (usLong != thLong)
            {
                Color chaseSide = usLong ? us : them;      // 长捉方
                Color checkSide = usLong ? them : us;      // 将捉方
                // 将捉方捉的是有根子(hasStrong) -> 长捉方变
                // 将捉方捉的是无根子 -> 将捉方变
                bool checkHasStrong = agg[checkSide].hasStrong;
                // 循环前长捉方已在捉 -> 长捉方先违规
                bool longPreChase = (chaseSide == WHITE) ? preChaseW : preChaseB;
                if (checkHasStrong || longPreChase)
                    loser = chaseSide;
                else
                    loser = checkSide;
                reason = reasonFor(loser);
            }
            else
                result = VALUE_DRAW;
        }
        else
        {
            if (splitChase(us) && agg[them].ck > 0 && !longCheck(them))
            { loser = us;   reason = "分捉多子"; }
            else if (splitChase(them) && agg[us].ck > 0 && !longCheck(us))
            { loser = them; reason = "分捉多子"; }
            else if ((splitChase(us) || splitChase(them))
                     && agg[us].ck == 0 && agg[them].ck == 0)
            {
                // 分捉多子且双方都不将军: 天天允许招法, 不判和棋, 让搜索正常出局面分
                skySplitAllowed = true; return VALUE_NONE;
            }
            else
                result = VALUE_DRAW;
        }
    }

    if (loser == us)
    {
        result = Value(-24999);
        Position::set_sky_rule_msg(std::string("我方") + reason + ",违规判负");
    }
    else if (loser == them)
    {
        result = Value(24999);
        Position::set_sky_rule_msg(std::string("对方") + reason + ",违规判负");
    }
    else
    {
        Position::set_sky_rule_msg("");
    }

#ifdef SKY_DEBUG  // SKY_DEBUG_ON
    fprintf(stderr, "SKY JUDGE ply=%d us=%d result=%d\n", ply, (int)us, (int)result);
#endif
    return result;
}


// SkyRule: 保存position moves后的将军计数, rule_judge用这个不遍历搜索sp链
int skyMoveCheckW = 0, skyMoveCheckB = 0;

// Tests whether the position may end the game by rule 60, insufficient material, draw repetition,
// perpetual check repetition or perpetual chase repetition that allows a player to claim a game result.
}  // namespace Stockfish
