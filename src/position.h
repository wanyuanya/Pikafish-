/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef POSITION_H_INCLUDED
#define POSITION_H_INCLUDED

#include <array>
#include <cassert>
#include <cstring>
#include <deque>
#include <iosfwd>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "attacks.h"
#include "bitboard.h"
#include "misc.h"
#include "nnue/features/half_ka_v2_hm.h"
#include "types.h"

namespace Stockfish {

class TranspositionTable;
struct SharedHistories;

// StateInfo struct stores information needed to restore a Position object to
// its previous state when we retract a move. Whenever a move is made on the
// board (by calling Position::do_move), a StateInfo object must be passed.

struct StateInfo {

    // Copied when making a move
    Key   pawnKey;
    Key   minorPieceKey;
    Key   nonPawnKey[COLOR_NB];
    Value majorMaterial[COLOR_NB];
    i16   check10[COLOR_NB];
    int   rule60;
    int   pliesFromNull;

    // Not copied when making a move (will be recomputed anyhow)
    Key        key;
    Bitboard   checkersBB;
    StateInfo* previous;
    Bitboard   blockersForKing[COLOR_NB];
    Bitboard   pinners[COLOR_NB];
    Bitboard   checkSquares[PIECE_TYPE_NB];
    bool       needFullCheck;
    Piece      capturedPiece;
    Move       move;
};


// SkyRule: 每方每步的走法特征计数器, 随 do_move/undo_move 同步维护
struct SkyCounter {
    int      checkSteps[COLOR_NB];      // 连续将军步数(按走子方)
    int      chaseSteps[COLOR_NB];      // 真捉步数(应将步不计)
    int      exposeSteps[COLOR_NB];     // 露捉步数
    uint32_t chaseTarget[COLOR_NB];     // 被捉目标id位图(同目标=长捉, 不同=分捉)
    uint8_t  checkMask[COLOR_NB];       // 将军子from位置位图(区分1/2/3子)
    int      sinceCapture;              // 距上次吃子步数
    bool     respStep[COLOR_NB];        // 应将步标记
};

// SkyRule: 搜索栈, 随深度 push/pop, 不依赖 StateInfo 历史链
constexpr int SKY_MAX_PLY = 256;
extern SkyCounter skyStack[SKY_MAX_PLY];
extern int        skyStackPly;

// A list to keep track of the position states along the setup moves (from the
// start position to the position just before the search starts). Needed by
// 'draw by repetition' detection. Use a std::deque because pointers to
// elements are not invalidated upon list resizing.
using StateListPtr = std::unique_ptr<std::deque<StateInfo>>;

// This error should be used whenever a position is suspected to be unsupported
// by the engine. In particular positions that may cause hard errors like segmentation fault.
struct PositionSetError: std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Position class stores information regarding the board representation as
// pieces, side to move, hash keys, etc. Important methods are
// do_move() and undo_move(), used by the search to update node info when
// traversing the search tree.
class Position {
   public:
    static void init();

    Position()                           = default;
    Position(const Position&)            = delete;
    Position& operator=(const Position&) = delete;

    // FEN string input/output
    std::optional<PositionSetError> set(const std::string& fenStr, StateInfo* si);
    std::optional<PositionSetError> set(const Position& pos, StateInfo* si);
    std::string                     fen() const;

    // Position representation
    Bitboard pieces() const;  // All pieces
    template<typename... PieceTypes>
    Bitboard pieces(PieceTypes... pts) const;
    Bitboard pieces(Color c) const;
    template<typename... PieceTypes>
    Bitboard                            pieces(Color c, PieceTypes... pts) const;
    Piece                               piece_on(Square s) const;
    const std::array<Piece, SQUARE_NB>& piece_array() const;
    bool                                empty(Square s) const;
    template<PieceType Pt>
    int count(Color c) const;
    template<PieceType Pt>
    int    count() const;
    Square king_square(Color c) const;
    u64    mid_encoding(Color c) const;

    // Checking
    Bitboard checkers() const;
    Bitboard blockers_for_king(Color c) const;
    Bitboard check_squares(PieceType pt) const;
    Bitboard pinners(Color c) const;

    // Attacks to/from a given square
    Bitboard attackers_to(Square s) const;
    Bitboard attackers_to(Square s, Bitboard occupied) const;
    template<Color c>
    void     update_blockers() const;
    Bitboard checkers_to(Color c, Square s) const;
    Bitboard checkers_to(Color c, Square s, Bitboard occupied) const;
    template<PieceType Pt>
    Bitboard attacks_by(Color c) const;

    // Properties of moves
    bool  legal(Move m) const;
    bool  pseudo_legal(const Move m) const;
    bool  capture(Move m) const;
    bool  gives_check(Move m) const;
    Piece moved_piece(Move m) const;
    Piece captured_piece() const;

    // Doing and undoing moves
    void do_move(Move m, StateInfo& newSt, const TranspositionTable* tt);
    void do_move(Move                      m,
                 StateInfo&                newSt,
                 bool                      givesCheck,
                 Dirties&                  dirties,
                 const TranspositionTable* tt,
                 const SharedHistories*    worker);
    void undo_move(Move m);
    void do_null_move(StateInfo& newSt);
    void undo_null_move();

    // Static Exchange Evaluation
    bool see_ge(Move m, int threshold = 0) const;

    // Accessing hash keys
    Key key() const;
    Key prefetch_key(Move m) const;
    Key pawn_key() const;
    Key minor_piece_key() const;
    Key defender_piece_key() const;
    Key non_pawn_key(Color c) const;

    // Other properties of the position
    Color side_to_move() const;
    int   game_ply() const;
    bool  rule_judge(Value& result, int ply = 0);
    int   rule60_count() const;
    u16   chased(Color c);
    Bitboard chased_positions(Color c);  // SkyRule: 按位置返回被捉子集合(并行规则)
    Value major_material(Color c) const;
    Value major_material() const;

    // Rule setting (SkyRule / AsianRule / etc.)
    static void set_rule(Rule r) { currentRule = r; }
    static Rule get_rule() { return currentRule; }
    static void set_rule60MaxPly(int n) { rule60MaxPly = n; }

    // SkyRule违规信息(用于UCI输出显示)
    static void        set_sky_rule_msg(const std::string& m) { skyRuleMsg = m; }
    static std::string get_sky_rule_msg() { return skyRuleMsg; }

    // SkyRule调试: 获取当前局面在filter中的重复次数
    int debug_filter() const { return filter[st->key]; }

    // 亚规条文4: 长杀检测——走子后是否形成杀势
    bool is_mate_threat(Color mover);

    // SkyRule/AsianRule: 公开结构体(供asiarule/skyrule模块使用)
    struct SkyChaseInfo {
        uint32_t chaseIds = 0;
        uint32_t attackIds = 0;
        uint32_t exchangeIds = 0;
        uint32_t rootedIds = 0;
        uint32_t cannonConfinedIds = 0;
        uint32_t knightConfinedIds = 0;
        uint32_t chaserIds = 0;
        uint32_t specialChaserIds = 0;
        uint32_t targetTypeMask = 0;
        uint32_t chaserTypeMask = 0;
        uint32_t passedPawnIds = 0;
        bool     mateThreat = false;
    };
    struct SkyStep {
        Color    mover;
        bool     isCheck;
        uint32_t chaseIds;
        bool     expose;
        bool     resp;
        Square   from;
        bool     inLoop = true;
        // AsianRule: stable identities of the pieces that create the chase.
        // A king/pawn chaser is kept separately for the rule 23-25 exceptions.
        uint32_t chaserIds = 0;
        uint32_t specialChaserIds = 0;
        uint32_t attackIds = 0;
        uint32_t exchangeIds = 0;
        uint32_t rootedIds = 0;
        uint32_t targetTypeMask = 0;
        uint32_t chaserTypeMask = 0;
        uint32_t passedPawnIds = 0;
        uint32_t cannonConfinedIds = 0;
        uint32_t knightConfinedIds = 0;
        bool     isKill = false;
    };
    struct SkyAgg {
        int      ck[COLOR_NB] = {0,0};
        int      kill[COLOR_NB] = {0,0};
        int      ch[COLOR_NB] = {0,0};
        int      responseChase[COLOR_NB] = {0,0};
        int      idle[COLOR_NB] = {0,0};
        int      expose[COLOR_NB] = {0,0};
        uint32_t chaseTarget[COLOR_NB] = {0,0};
        uint32_t chaseIntersect[COLOR_NB] = {0xFFFFFFFFu, 0xFFFFFFFFu};
        uint32_t chaseUnion[COLOR_NB] = {0,0};
        uint32_t attackIntersect[COLOR_NB] = {0xFFFFFFFFu, 0xFFFFFFFFu};
        uint32_t attackUnion[COLOR_NB] = {0,0};
        uint32_t exchangeUnion[COLOR_NB] = {0,0};
        uint32_t exchangeIntersect[COLOR_NB] = {0xFFFFFFFFu, 0xFFFFFFFFu};
        uint32_t rootedUnion[COLOR_NB] = {0,0};
        uint32_t rootedIntersect[COLOR_NB] = {0xFFFFFFFFu, 0xFFFFFFFFu};
        uint32_t cannonConfinedUnion[COLOR_NB] = {0,0};
        uint32_t knightConfinedUnion[COLOR_NB] = {0,0};
        uint32_t chaserIntersect[COLOR_NB] = {0xFFFFFFFFu, 0xFFFFFFFFu};
        uint32_t chaserUnion[COLOR_NB] = {0,0};
        uint32_t targetTypeIntersect[COLOR_NB] = {0xFFFFFFFFu, 0xFFFFFFFFu};
        uint32_t targetTypeUnion[COLOR_NB] = {0,0};
        uint32_t chaserTypeIntersect[COLOR_NB] = {0xFFFFFFFFu, 0xFFFFFFFFu};
        uint32_t chaserTypeUnion[COLOR_NB] = {0,0};
        uint32_t passedPawnIntersect[COLOR_NB] = {0xFFFFFFFFu, 0xFFFFFFFFu};
        uint32_t passedPawnUnion[COLOR_NB] = {0,0};
        int      chaseSpecialOnly[COLOR_NB] = {0,0};
        int      chaseMixed[COLOR_NB] = {0,0};
        int      chaseRootedSteps[COLOR_NB] = {0,0};
        int      chaseUnrootedSteps[COLOR_NB] = {0,0};
        int      multiTargetSteps[COLOR_NB] = {0,0};
        int      checkPieceCount[COLOR_NB] = {0,0};
        bool     preLoopChase[COLOR_NB] = {false,false};
        bool     split[COLOR_NB] = {false,false};
    };

    // Position consistency check, for debugging
    bool                            pos_is_ok() const;
    std::optional<PositionSetError> flip();

    StateInfo* state() const;

    void put_piece(Piece pc, Square s, DirtyThreats* const dts = nullptr);
    void remove_piece(Square s, DirtyThreats* const dts = nullptr);
    void swap_piece(Square s, Piece pc, DirtyThreats* const dts = nullptr);

   private:
    // Initialization helpers (used while setting up a position)
    void set_state() const;
    void set_check_info() const;

    // Other helpers
    template<bool ComputeRay = true>
    void update_piece_threats(Piece pc, bool putPiece, Square s, DirtyThreats* const dts);
    void move_piece(Square from, Square to, DirtyThreats* const dts = nullptr);
    std::pair<Piece, int> do_move(Move m);
    void                  undo_move(Move m, Piece captured, int id = 0);
    Value                 detect_chases(int d, int ply = 0);
    // SkyRule(天天象棋规则): 带棋子身份追踪的逐着打/闲循环判定
    Value                 sky_judge_loop(int loopLen, int ply = 0);
    bool                  chase_legal(Move m) const;

    // SkyRule 新框架: 每步走法的特征判定
    // 走完 m 后, mover 方真捉的无根子id位图(有根不算, 不查牵制)
    SkyChaseInfo          sky_real_chase(Move m, Color mover);
    // 走完 m 后, 走子是将帅且露出了攻击 = 露捉
    bool                  sky_is_expose(Move m, Color mover) const;
    // 走前本方被将军 = 应将
    bool                  sky_is_response(Color mover) const;
    // 稳定子身份id
    int                   sky_target_id(Square s) const { return idBoard[s]; }

    // SkyRule 新框架: 循环段提取与聚合
    std::vector<SkyStep> sky_extract_loop(int loopLen);
    SkyAgg sky_aggregate(const std::vector<SkyStep>& steps, int loopLen);

    // 优先级判决: 返回+24999/-24999/VALUE_DRAW/VALUE_NONE
    Value sky_judge_priority(const SkyAgg& agg, Color stm, int loopLen, int ply);

    template<bool AfterMove = false>
    Key adjust_key60(Key k) const;

    // Static rule setting
    static Rule        currentRule;
    static std::string skyRuleMsg;
    static int         rule60MaxPly;   // SkyRule可配置；AsianRule固定100着

    // Data members
    std::array<Piece, SQUARE_NB>        board;
    std::array<Bitboard, PIECE_TYPE_NB> byTypeBB;
    std::array<Bitboard, COLOR_NB>      byColorBB;

    int        pieceCount[PIECE_NB];
    u64        midEncoding[COLOR_NB];
    StateInfo* st;
    int        gamePly;
    Color      sideToMove;

    // Bloom filter for fast repetition filtering
    BloomFilter filter;

    // Board for chasing detection
    int idBoard[SQUARE_NB];

    Dirties scratchDirties;
};

std::ostream& operator<<(std::ostream& os, const Position& pos);

inline Color Position::side_to_move() const { return sideToMove; }

inline Piece Position::piece_on(Square s) const {
    assert(is_ok(s));
    return board[s];
}

inline const std::array<Piece, SQUARE_NB>& Position::piece_array() const { return board; }

inline bool Position::empty(Square s) const { return piece_on(s) == NO_PIECE; }

inline Piece Position::moved_piece(Move m) const { return piece_on(m.from_sq()); }

inline Bitboard Position::pieces() const { return byTypeBB[ALL_PIECES]; }

template<typename... PieceTypes>
inline Bitboard Position::pieces(PieceTypes... pts) const {
    return (byTypeBB[pts] | ...);
}

inline Bitboard Position::pieces(Color c) const { return byColorBB[c]; }

template<typename... PieceTypes>
inline Bitboard Position::pieces(Color c, PieceTypes... pts) const {
    return pieces(c) & pieces(pts...);
}

template<PieceType Pt>
inline int Position::count(Color c) const {
    return pieceCount[make_piece(c, Pt)];
}

template<PieceType Pt>
inline int Position::count() const {
    return count<Pt>(WHITE) + count<Pt>(BLACK);
}

inline Square Position::king_square(Color c) const {
    return c == WHITE ? lsb(u64(pieces(KING))) : Square(64 + lsb(u64(pieces(KING) >> 64)));
}

inline u64 Position::mid_encoding(Color c) const { return midEncoding[c]; }

inline Bitboard Position::attackers_to(Square s) const { return attackers_to(s, pieces()); }

inline Bitboard Position::checkers_to(Color c, Square s) const {
    return checkers_to(c, s, pieces());
}

template<PieceType Pt>
inline Bitboard Position::attacks_by(Color c) const {

    Bitboard threats   = 0;
    Bitboard attackers = pieces(c, Pt);
    while (attackers)
        if (Pt == PAWN)
            threats |= Attacks::attacks_bb<PAWN>(pop_lsb(attackers), c);
        else
            threats |= Attacks::attacks_bb<Pt>(pop_lsb(attackers), pieces());
    return threats;
}

inline Bitboard Position::checkers() const { return st->checkersBB; }

inline Bitboard Position::blockers_for_king(Color c) const { return st->blockersForKing[c]; }

inline Bitboard Position::pinners(Color c) const { return st->pinners[c]; }

inline Bitboard Position::check_squares(PieceType pt) const { return st->checkSquares[pt]; }

inline Key Position::key() const { return adjust_key60(st->key); }

template<bool AfterMove>
inline Key Position::adjust_key60(Key k) const {
    return (st->rule60 < (14 - AfterMove) ? k : k ^ make_key((st->rule60 - (14 - AfterMove)) / 8))
         ^ (filter[k] ? make_key(14) : 0);
}

inline Key Position::pawn_key() const { return st->pawnKey; }

inline Key Position::minor_piece_key() const { return st->minorPieceKey; }

inline Key Position::non_pawn_key(Color c) const { return st->nonPawnKey[c]; }

inline Value Position::major_material(Color c) const { return st->majorMaterial[c]; }

inline Value Position::major_material() const {
    return major_material(WHITE) + major_material(BLACK);
}

inline int Position::game_ply() const { return gamePly; }

inline int Position::rule60_count() const { return st->rule60; }

inline bool Position::capture(Move m) const {
    assert(m.is_ok());
    return !empty(m.to_sq());
}

inline Piece Position::captured_piece() const { return st->capturedPiece; }

inline void Position::put_piece(Piece pc, Square s, DirtyThreats* const dts) {

    board[s] = pc;
    byTypeBB[ALL_PIECES] |= byTypeBB[type_of(pc)] |= s;
    byColorBB[color_of(pc)] |= s;
    pieceCount[pc]++;
    pieceCount[make_piece(color_of(pc), ALL_PIECES)]++;
    midEncoding[color_of(pc)] += Eval::NNUE::Features::HalfKAv2_hm::MidMirrorEncoding[pc][s];

    if (dts)
        update_piece_threats(pc, true, s, dts);
}

inline void Position::remove_piece(Square s, DirtyThreats* const dts) {

    Piece pc = board[s];

    if (dts)
        update_piece_threats(pc, false, s, dts);

    byTypeBB[ALL_PIECES] ^= s;
    byTypeBB[type_of(pc)] ^= s;
    byColorBB[color_of(pc)] ^= s;
    board[s] = NO_PIECE;
    pieceCount[pc]--;
    pieceCount[make_piece(color_of(pc), ALL_PIECES)]--;
    midEncoding[color_of(pc)] -= Eval::NNUE::Features::HalfKAv2_hm::MidMirrorEncoding[pc][s];
}

inline void Position::move_piece(Square from, Square to, DirtyThreats* const dts) {

    Piece    pc     = board[from];
    Bitboard fromTo = from | to;

    if (dts)
        update_piece_threats(pc, false, from, dts);

    byTypeBB[ALL_PIECES] ^= fromTo;
    byTypeBB[type_of(pc)] ^= fromTo;
    byColorBB[color_of(pc)] ^= fromTo;
    board[from] = NO_PIECE;
    board[to]   = pc;
    midEncoding[color_of(pc)] -= Eval::NNUE::Features::HalfKAv2_hm::MidMirrorEncoding[pc][from];
    midEncoding[color_of(pc)] += Eval::NNUE::Features::HalfKAv2_hm::MidMirrorEncoding[pc][to];

    if (dts)
        update_piece_threats(pc, true, to, dts);
}

inline void Position::swap_piece(Square s, Piece pc, DirtyThreats* const dts) {
    Piece old = board[s];

    remove_piece(s);

    if (dts)
        update_piece_threats<false>(old, false, s, dts);

    put_piece(pc, s);

    if (dts)
        update_piece_threats<false>(pc, true, s, dts);
}

inline void Position::do_move(Move m, StateInfo& newSt, const TranspositionTable* tt = nullptr) {
    new (&scratchDirties.dirtyThreats) DirtyThreats;
    do_move(m, newSt, gives_check(m), scratchDirties, tt, nullptr);
}

inline StateInfo* Position::state() const { return st; }

inline std::optional<PositionSetError> Position::set(const Position& pos, StateInfo* si) {

    auto err = set(pos.fen(), si);

    // Special cares for bloom filter
    std::memcpy(&filter, &pos.filter, sizeof(BloomFilter));

    return err;
}

}  // namespace Stockfish

#endif  // #ifndef POSITION_H_INCLUDED
