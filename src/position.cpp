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

#include "position.h"
#include "skyrule.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <cstring>
#include <vector>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#include "attacks.h"
#include "bitboard.h"
#include "history.h"
#include "misc.h"
#include "movegen.h"
#include "nnue/nnue_common.h"
#include "nnue/nnue_architecture.h"
#include "tt.h"
#include "uci.h"

using std::string;

namespace Stockfish {

using namespace Attacks;

// Default rule: SkyRule (天天象棋规则)
Rule Position::currentRule = SKY_RULE;
std::string Position::skyRuleMsg;
int  Position::rule60MaxPly = 120;   // SkyRule默认阈值；AsianRule固定100着

// SkyRule: 全局计数器栈
SkyCounter skyStack[SKY_MAX_PLY];
int        skyStackPly = 0;

namespace Zobrist {

Key psq[PIECE_NB][SQUARE_NB];
Key side, noPawns;
}

namespace {

constexpr std::string_view PieceToChar(" RACPNBK racpnbk");

static constexpr Piece Pieces[] = {W_ROOK, W_ADVISOR, W_CANNON, W_PAWN, W_KNIGHT, W_BISHOP, W_KING,
                                   B_ROOK, B_ADVISOR, B_CANNON, B_PAWN, B_KNIGHT, B_BISHOP, B_KING};
}  // namespace

// Returns an ASCII representation of the position
std::ostream& operator<<(std::ostream& os, const Position& pos) {

    os << "\n +---+---+---+---+---+---+---+---+---+\n";

    for (Rank r = RANK_9;; --r)
    {
        for (File f = FILE_A; f <= FILE_I; ++f)
            os << " | " << PieceToChar[pos.piece_on(make_square(f, r))];

        os << " | " << int(r) << "\n +---+---+---+---+---+---+---+---+---+\n";

        if (r == RANK_0)
            break;
    }

    os << "   a   b   c   d   e   f   g   h   i\n"
       << "\nFen: " << pos.fen() << "\nKey: " << std::hex << std::uppercase << std::setfill('0')
       << std::setw(16) << pos.key() << std::setfill(' ') << std::dec << "\nCheckers: ";

    for (Bitboard b = pos.checkers(); b;)
        os << UCIEngine::square(pop_lsb(b)) << " ";

    return os;
}


// Initializes at startup the various arrays used to compute hash keys
void Position::init() {

    PRNG rng(1070372);

    for (Piece pc : Pieces)
        for (Square s = SQ_A0; s <= SQ_I9; ++s)
            Zobrist::psq[pc][s] = rng.rand<Key>();

    Zobrist::side    = rng.rand<Key>();
    Zobrist::noPawns = rng.rand<Key>();
}


// Initializes the position object with the given FEN string.
// The FEN string is strictly validated; if it is invalid or inconsistent,
// a PositionSetError describing the problem is returned, otherwise std::nullopt.
std::optional<PositionSetError> Position::set(const string& fenStr, StateInfo* si) {
    /*
   A FEN string defines a particular position using only the ASCII character set.

   A FEN string contains six fields separated by a space. The fields are:

   1) Piece placement (from white's perspective). Each rank is described, starting
      with rank 9 and ending with rank 0. Within each rank, the contents of each
      square are described from file A through file I. Following the Standard
      Algebraic Notation (SAN), each piece is identified by a single letter taken
      from the standard English names. White pieces are designated using upper-case
      letters ("RACPNBK") whilst Black uses lowercase ("racpnbk"). Blank squares are
      noted using digits 1 through 9 (the number of blank squares), and "/"
      separates ranks.

   2) Active color. "w" means white moves next, "b" means black.

   3) Halfmove clock. This is the number of halfmoves since the last pawn advance
      or capture. This is used to determine if a draw can be claimed under the
      fifty-move rule.

   4) Fullmove number. The number of the full move. It starts at 1, and is
      incremented after Black's move.
*/

    unsigned char      token;
    std::istringstream ss(fenStr);

    std::memset(reinterpret_cast<char*>(this), 0, sizeof(Position));
    std::memset(si, 0, sizeof(StateInfo));
    st = si;

    midEncoding[WHITE] = midEncoding[BLACK] = Eval::NNUE::Features::HalfKAv2_hm::BalanceEncoding;

    ss >> std::noskipws;

    int numPieces = 0;
    int file      = FILE_A;
    int rank      = RANK_9;

    // 1. Piece placement
    for (;;)
    {
        if (!(ss >> token))
            return PositionSetError("Invalid FEN. Unexpected end of stream.");

        if (isspace(token))
            break;

        if (isdigit(token))
        {
            const int diff = (token - '0');
            if (diff < 1)
                return PositionSetError("Invalid FEN. Invalid number of squares to skip.");

            file += diff;
            if (file > FILE_NB)
                return PositionSetError("Invalid FEN. Invalid file reached.");
        }
        else if (token == '/')
        {
            if (file != FILE_NB)
                return PositionSetError(
                  "Invalid FEN. Trying to end rank when not at the end of it.");

            --rank;
            file = FILE_A;

            if (rank < RANK_0)
                return PositionSetError("Invalid FEN. Invalid rank reached.");
        }
        else
        {
            if (file >= FILE_NB)
                return PositionSetError("Invalid FEN. Invalid file reached.");

            const usize idx = PieceToChar.find(token);
            if (idx == string::npos)
                return PositionSetError(std::string("Invalid FEN. Invalid piece: ")
                                        + std::string(1, token));

            if (++numPieces > 32)
                return PositionSetError("Invalid FEN. More than 32 pieces on the board.");

            const Square sq = make_square(File(file), Rank(rank));
            put_piece(Piece(idx), sq);

            ++file;
        }
    }

    if (rank != RANK_0 || file != FILE_NB)
        return PositionSetError("Invalid FEN. Board state encoding ended but cursor not at end.");

    if (count<KING>(WHITE) != 1 || count<KING>(BLACK) != 1)
        return PositionSetError("Unsupported position. Incorrect number of kings.");

    const std::string PieceTypeToStr[PIECE_TYPE_NB] = {"",     "rook",   "advisor", "cannon",
                                                       "pawn", "knight", "bishop",  "king"};
    constexpr int     MaxPieces[PIECE_TYPE_NB - 1]  = {0, 2, 2, 2, 5, 2, 2};
    for (Color c : {WHITE, BLACK})
    {
        for (PieceType pt = ROOK; pt < KING; ++pt)
            if (popcount(pieces(c, pt)) > MaxPieces[pt])
                return PositionSetError(std::string("Unsupported position. ")
                                        + (c == WHITE ? "WHITE " : "BLACK ") + "has more than "
                                        + std::to_string(MaxPieces[pt]) + " " + PieceTypeToStr[pt]
                                        + "s.");

        for (PieceType pt : {ADVISOR, PAWN, BISHOP, KING})
        {
            Bitboard valid = Eval::NNUE::Features::HalfKAv2_hm::ValidBB[make_piece(c, pt)];
            // NNUE mirroring feature does not allow white king on the right flank, we allow here.
            if (c == WHITE && pt == KING)
                valid = HalfBB[WHITE] & Palace;
            if (pieces(c, pt) & ~valid)
                return PositionSetError(std::string("Unsupported position. ")
                                        + (c == WHITE ? "WHITE " : "BLACK ") + PieceTypeToStr[pt]
                                        + "(s) on invalid positions.");
        }
    }

    // 2. Active color
    if (!(ss >> token))
        return PositionSetError("Invalid FEN. Unexpected end of stream.");
    if (token != 'w' && token != 'b')
        return PositionSetError(std::string("Invalid FEN. Invalid side to move: ")
                                + std::string(1, token));
    sideToMove = (token == 'w' ? WHITE : BLACK);
    if (!(ss >> token) || !isspace(token) || ss.eof())
        return PositionSetError("Invalid FEN. Expected whitespace after side to move.");

    while ((ss >> token) && !isspace(token))
        ;

    while ((ss >> token) && !isspace(token))
        ;

    // 3-4. Halfmove clock and fullmove number
    ss >> std::skipws >> st->rule60 >> gamePly;
    st->check10[WHITE] = st->check10[BLACK] = 0;  // SkyRule: 初始局面清零将军计数

    if (st->rule60 < 0 || st->rule60 > 119)
        return PositionSetError("Unsupported position. Rule60 counter out of range.");

    if (gamePly < 0 || gamePly > 100000)
        return PositionSetError("Unsupported position. Game ply out of range.");

    // Convert from fullmove starting from 1 to gamePly starting from 0,
    // handle also common incorrect FEN with fullmove = 0.
    gamePly = std::max(2 * (gamePly - 1), 0) + (sideToMove == BLACK);

    set_state();

    if (checkers_to(sideToMove, king_square(~sideToMove)))
        return PositionSetError("Unsupported position. King can be captured.");

    assert(pos_is_ok());

    return std::nullopt;
}


// Sets king attacks to detect if a move gives check
void Position::set_check_info() const {

    update_blockers<WHITE>();
    update_blockers<BLACK>();

    Square ksq = king_square(~sideToMove);

    // We have to take special cares about the hollow cannons and checks
    st->needFullCheck =
      checkers() || (attacks_bb<ROOK>(king_square(sideToMove)) & pieces(~sideToMove, CANNON));

    st->checkSquares[PAWN]   = attacks_bb<PAWN_TO>(ksq, sideToMove);
    st->checkSquares[KNIGHT] = attacks_bb<KNIGHT_TO>(ksq, pieces());
    st->checkSquares[CANNON] = attacks_bb<CANNON>(ksq, pieces());
    st->checkSquares[ROOK]   = attacks_bb<ROOK>(ksq, pieces());
    st->checkSquares[KING] = st->checkSquares[ADVISOR] = st->checkSquares[BISHOP] = 0;

    Bitboard hollowCannons = st->checkSquares[ROOK] & pieces(sideToMove, CANNON);
    if (hollowCannons)
    {
        Bitboard hollowCannonDiscover = Bitboard(0);
        while (hollowCannons)
            hollowCannonDiscover |= between_bb(pop_lsb(hollowCannons), ksq);
        for (PieceType pt = ROOK; pt < KING; ++pt)
            st->checkSquares[pt] |= hollowCannonDiscover;
    }
}


// Computes the hash keys of the position, and other
// data that once computed is updated incrementally as moves are made.
// The function is only used when a new position is set up
void Position::set_state() const {

    st->key               = 0;
    st->minorPieceKey     = 0;
    st->nonPawnKey[WHITE] = st->nonPawnKey[BLACK] = 0;
    st->pawnKey                                   = Zobrist::noPawns;
    st->majorMaterial[WHITE] = st->majorMaterial[BLACK] = VALUE_ZERO;
    st->checkersBB = checkers_to(~sideToMove, king_square(sideToMove));
    st->move       = Move::none();

    set_check_info();

    for (Bitboard b = pieces(); b;)
    {
        Square    s  = pop_lsb(b);
        Piece     pc = piece_on(s);
        PieceType pt = type_of(pc);
        st->key ^= Zobrist::psq[pc][s];

        if (pt == PAWN)
            st->pawnKey ^= Zobrist::psq[pc][s];

        else
        {
            st->nonPawnKey[color_of(pc)] ^= Zobrist::psq[pc][s];

            if (pt != KING && (pt & 1))
            {
                st->majorMaterial[color_of(pc)] += PieceValue[pc];
                if (pt != ROOK)
                    st->minorPieceKey ^= Zobrist::psq[pc][s];
            }
        }
    }

    if (sideToMove == BLACK)
        st->key ^= Zobrist::side;
}


// Returns a FEN representation of the position.
string Position::fen() const {

    int                emptyCnt;
    std::ostringstream ss;

    for (Rank r = RANK_9;; --r)
    {
        for (File f = FILE_A; f <= FILE_I; ++f)
        {
            for (emptyCnt = 0; f <= FILE_I && empty(make_square(f, r)); ++f)
                ++emptyCnt;

            if (emptyCnt)
                ss << emptyCnt;

            if (f <= FILE_I)
                ss << PieceToChar[piece_on(make_square(f, r))];
        }

        if (r == RANK_0)
            break;
        ss << '/';
    }

    ss << (sideToMove == WHITE ? " w " : " b ");

    ss << '-';

    ss << " - " << st->rule60 << " " << 1 + (gamePly - (sideToMove == BLACK)) / 2;

    return ss.str();
}


// Calculates st->blockersForKing[c] and st->pinners[~c],
// which store respectively the pieces preventing king of color c from being in check
// and the slider pieces of color ~c pinning pieces of color c to the king.
template<Color c>
void Position::update_blockers() const {

    Square ksq             = king_square(c);
    st->blockersForKing[c] = 0;
    st->pinners[~c]        = 0;

    // Snipers are pieces that attack 's' when a piece and other pieces are removed
    Bitboard snipers   = ((attacks_bb<ROOK>(ksq) & (pieces(ROOK) | pieces(CANNON) | pieces(KING)))
                          | (attacks_bb<KNIGHT>(ksq) & pieces(KNIGHT)))
                       & pieces(~c);
    Bitboard occupancy = pieces() ^ (snipers & ~pieces(CANNON));

    while (snipers)
    {
        Square   sniperSq = pop_lsb(snipers);
        bool     isCannon = type_of(piece_on(sniperSq)) == CANNON;
        Bitboard b = between_bb(ksq, sniperSq) & (isCannon ? pieces() ^ sniperSq : occupancy);

        if (b && ((!isCannon && !more_than_one(b)) || (isCannon && popcount(b) == 2)))
        {
            st->blockersForKing[c] |= b;
            if (b & pieces(c))
                st->pinners[~c] |= sniperSq;
        }
    }
}


// Computes a bitboard of all pieces which attack a given square.
// Slider attacks use the occupied bitboard to indicate occupancy.
Bitboard Position::attackers_to(Square s, Bitboard occupied) const {

    return (attacks_bb<PAWN_TO>(s, WHITE) & pieces(WHITE, PAWN))
         | (attacks_bb<PAWN_TO>(s, BLACK) & pieces(BLACK, PAWN))
         | (attacks_bb<KNIGHT_TO>(s, occupied) & pieces(KNIGHT))
         | (attacks_bb<ROOK>(s, occupied) & pieces(ROOK))
         | (attacks_bb<CANNON>(s, occupied) & pieces(CANNON))
         | (attacks_bb<BISHOP>(s, occupied) & pieces(BISHOP))
         | (attacks_bb<ADVISOR>(s) & pieces(ADVISOR)) | (attacks_bb<KING>(s) & pieces(KING));
}


// Computes a bitboard of all pieces of a given color
// which gives check to a given square. Slider attacks use the occupied bitboard
// to indicate occupancy.
Bitboard Position::checkers_to(Color c, Square s, Bitboard occupied) const {

    return ((attacks_bb<PAWN_TO>(s, c) & pieces(PAWN))
            | (attacks_bb<KNIGHT_TO>(s, occupied) & pieces(KNIGHT))
            | (attacks_bb<ROOK>(s, occupied) & pieces(KING, ROOK))
            | (attacks_bb<CANNON>(s, occupied) & pieces(CANNON)))
         & pieces(c);
}


// Tests whether a pseudo-legal move is legal
bool Position::legal(Move m) const {

    assert(m.is_ok());

    Color    us       = sideToMove;
    Square   from     = m.from_sq();
    Square   to       = m.to_sq();
    Bitboard occupied = (pieces() ^ from) | to;

    assert(color_of(moved_piece(m)) == us);
    assert(piece_on(king_square(us)) == make_piece(us, KING));

    // If the moving piece is a king, check whether the destination square is
    // attacked by the opponent.
    if (type_of(piece_on(from)) == KING)
        return !(checkers_to(~us, to, occupied));

    // If we don't need full check. A non-king move is always legal when either:
    // 1. Not moving a pinned piece.
    // 2. Moving a pinned non-cannon piece and aligned with king.
    // 3. Moving a pinned cannon and aligned with king but it's not a capture move.
    if (!st->needFullCheck
        && (!(blockers_for_king(us) & from)
            || (((type_of(piece_on(from)) != CANNON) || !capture(m))
                && aligned(from, to, king_square(us)))))
        return true;

    // A non-king move is legal if the king is not under attack after the move.
    return !(checkers_to(~us, king_square(us), occupied) & ~square_bb(to));
}


// Takes a random move and tests whether the move is
// pseudo-legal. It is used to validate moves from TT that can be corrupted
// due to SMP concurrent access or hash position key aliasing.
bool Position::pseudo_legal(const Move m) const {

    Color  us   = sideToMove;
    Square from = m.from_sq();
    Square to   = m.to_sq();
    Piece  pc   = moved_piece(m);

    // If the 'from' square is not occupied by a piece belonging to the side to
    // move, the move is obviously not legal.
    if (pc == NO_PIECE || color_of(pc) != us)
        return false;

    // The destination square cannot be occupied by a friendly piece
    if (pieces(us) & to)
        return false;

    // Handle the special cases
    if (type_of(pc) == PAWN)
    {
        if (!(attacks_bb<PAWN>(from, us) & to))
            return false;
    }
    else if (type_of(pc) == CANNON && !capture(m))
    {
        if (!(attacks_bb<ROOK>(from, pieces()) & to))
            return false;
    }
    else if (!(attacks_bb(type_of(pc), from, pieces()) & to))
        return false;

    if (checkers())
        return MoveList<EVASIONS>(*this).contains(m);

    return true;
}


// Tests whether a pseudo-legal move gives a check
bool Position::gives_check(Move m) const {

    assert(m.is_ok());
    assert(color_of(moved_piece(m)) == sideToMove);

    Square from = m.from_sq();
    Square to   = m.to_sq();
    Square ksq  = king_square(~sideToMove);

    PieceType pt = type_of(moved_piece(m));

    // Is there a direct check?
    if (pt == CANNON && (check_squares(ROOK) & from) && aligned(from, to, ksq))
    {
        if (capture(m) && (ray_pass_bb(ksq, from) & to))
            return true;
    }
    else if (check_squares(pt) & to)
        return true;

    // Is there a discovered check?
    if ((blockers_for_king(~sideToMove) & from) && (!aligned(from, to, ksq) || capture(m)))
        return true;

    return false;
}


// Makes a move, and saves all information necessary
// to a StateInfo object. The move is assumed to be legal. Pseudo-legal
// moves should be filtered out before this function is called.
// If a pointer to the TT table is passed, the entry for the new position
// will be prefetched, and likewise for shared history.
void Position::do_move(Move                      m,
                       StateInfo&                newSt,
                       bool                      givesCheck,
                       Dirties&                  dirties,
                       const TranspositionTable* tt      = nullptr,
                       const SharedHistories*    history = nullptr) {

    using namespace Eval::NNUE;

    assert(m.is_ok());
    assert(&newSt != st);

    // SkyRule: 走前状态记录
    uint32_t skyChaseIds = 0;  // 预留: chase在rule_judge时用rollback算
    bool     skyWasResp  = bool(st->checkersBB);  // 走前被将=应将

    // Update the bloom filter
    ++filter[st->key];

    Key k = st->key ^ Zobrist::side;

    // Copy some fields of the old state to our new StateInfo object except the
    // ones which are going to be recalculated from scratch anyway and then switch
    // our state pointer to point to the new (ready to be updated) state.
    std::memcpy(&newSt, st, offsetof(StateInfo, key));
    newSt.previous = st;
    st             = &newSt;
    st->move       = m;

    // Increment ply counters. Clamp to 10 checks for each side in rule 60
    // In particular, rule60 will be reset to zero later on in case of a capture.
    ++gamePly;
    if (!givesCheck || ++st->check10[sideToMove] <= 10)
    {
        if (st->check10[~sideToMove] > 10 && st->previous->checkersBB)
            ++st->check10[~sideToMove];
        else
            ++st->rule60;
    }
    ++st->pliesFromNull;

    auto& dts = dirties.dirtyThreats;
    auto& dp  = dirties.dirtyPiece;

    Color  us       = sideToMove;
    Color  them     = ~us;
    Square from     = m.from_sq();
    Square to       = m.to_sq();
    Piece  pc       = piece_on(from);
    Piece  captured = piece_on(to);

    dp.pc   = pc;
    dp.from = from;
    dp.to   = to;

    assert(color_of(pc) == us);
    assert(captured == NO_PIECE || color_of(captured) == them);
    assert(type_of(captured) != KING);

    if (captured)
    {
        Square capsq = to;

        // If the captured piece is a pawn, update pawn hash key, otherwise
        // update major material.
        if (type_of(captured) == PAWN)
            st->pawnKey ^= Zobrist::psq[captured][capsq];

        else
        {
            st->nonPawnKey[them] ^= Zobrist::psq[captured][capsq];

            if (type_of(captured) & 1)
            {
                st->majorMaterial[them] -= PieceValue[captured];
                if (type_of(captured) != ROOK)
                    st->minorPieceKey ^= Zobrist::psq[captured][capsq];
            }
        }

        dp.remove_pc = captured;
        dp.remove_sq = capsq;

        // Update hash key
        k ^= Zobrist::psq[captured][capsq];

        // Reset rule 60 counter
        st->check10[WHITE] = st->check10[BLACK] = st->rule60 = 0;
    }
    else
        dp.remove_sq = SQ_NONE;

    // Update hash key
    k ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
    if (tt)
        prefetch(tt->first_entry(adjust_key60(k)));
    // Update the key with the final value
    st->key = k;

    // If the moving piece is a pawn, update pawn hash key.
    if (type_of(pc) == PAWN)
        st->pawnKey ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
    else
    {
        st->nonPawnKey[us] ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];

        if (type_of(pc) == KNIGHT || type_of(pc) == CANNON)
            st->minorPieceKey ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
    }

    if (history)
    {
        prefetch(&history->pawn_entry(*this)[pc][to]);
        prefetch(&history->pawn_correction_entry(*this));
        prefetch(&history->minor_piece_correction_entry(*this));
        prefetch(&history->nonpawn_correction_entry<WHITE>(*this));
        prefetch(&history->nonpawn_correction_entry<BLACK>(*this));
    }

    bool mirror_before[2] = {
      PSQFeatureSet::KingBuckets[king_square(us)][king_square(them)]
                                [PSQFeatureSet::requires_mid_mirror(*this, us)]
                                  .second,
      PSQFeatureSet::KingBuckets[king_square(them)][king_square(us)]
                                [PSQFeatureSet::requires_mid_mirror(*this, them)]
                                  .second};
    dp.requires_refresh[them] = false;
    dp.requires_refresh[us]   = pc == make_piece(us, KING);

    if (captured)
    {
        auto attack_bucket_before = PSQFeatureSet::make_attack_bucket(*this, them);

        remove_piece(from, &dts);
        swap_piece(to, pc, &dts);

        auto attack_bucket_after = PSQFeatureSet::make_attack_bucket(*this, them);

        dp.requires_refresh[them] |= (attack_bucket_before != attack_bucket_after);
    }
    else
        move_piece(from, to, &dts);

    bool mirror_after[2] = {
      PSQFeatureSet::KingBuckets[king_square(us)][king_square(them)]
                                [PSQFeatureSet::requires_mid_mirror(*this, us)]
                                  .second,
      PSQFeatureSet::KingBuckets[king_square(them)][king_square(us)]
                                [PSQFeatureSet::requires_mid_mirror(*this, them)]
                                  .second};
    dp.requires_refresh[us] |= (mirror_before[0] != mirror_after[0]);
    dp.requires_refresh[them] |= (mirror_before[1] != mirror_after[1]);

    // Set capture piece
    st->capturedPiece = captured;

    // Calculate checkers bitboard (if move gives check)
    st->checkersBB = givesCheck ? checkers_to(us, king_square(them)) : Bitboard(0);
    assert(givesCheck == bool(checkers_to(us, king_square(them))));

    sideToMove = ~sideToMove;

    // Update king attacks used for fast check detection
    set_check_info();

    assert(pos_is_ok());

    assert(dp.pc != NO_PIECE);
    assert(!bool(captured) ^ (dp.remove_sq != SQ_NONE));
    assert(dp.from != SQ_NONE && dp.to != SQ_NONE);

    // SkyRule: push skyStack. 从上层继承, 本步更新
    if (skyStackPly < SKY_MAX_PLY - 1)
    {
        int p = ++skyStackPly;
        SkyCounter& cur = skyStack[p];
        SkyCounter& prev = skyStack[p-1];

        // 继承上层
        std::memcpy(&cur, &prev, sizeof(SkyCounter));

        // 本步走子方 = us (走m前的sideToMove)
        // 吃子清零
        if (captured)
        {
            cur.checkSteps[WHITE] = cur.checkSteps[BLACK] = 0;
            cur.chaseSteps[WHITE] = cur.chaseSteps[BLACK] = 0;
            cur.exposeSteps[WHITE] = cur.exposeSteps[BLACK] = 0;
            cur.chaseTarget[WHITE] = cur.chaseTarget[BLACK] = 0;
            cur.checkMask[WHITE] = cur.checkMask[BLACK] = 0;
            cur.sinceCapture = 0;
        }
        else
            ++cur.sinceCapture;

        // 应将: 走前 checkersBB 非空
        bool resp = skyWasResp;
        cur.respStep[us] = resp;

        // 将军
        if (givesCheck)
        {
            cur.checkSteps[us]++;
            cur.checkMask[us] |= (1u << (int(from) & 31));
        }
        else
        {
            // 非将军步, 清将军子mask(连续将军才累加)
            cur.checkMask[us] = 0;
        }

        // 真捉: chaseSteps在rule_judge时用rollback算, 这里只记应将和将军
        (void)skyChaseIds;
        // 露捉: 走子是将帅
        if (type_of(piece_on(from)) == KING)
            cur.exposeSteps[us]++;
    }
}


// Unmakes a move. When it returns, the position should
// be restored to exactly the same state as before the move was made.
void Position::undo_move(Move m) {

    assert(m.is_ok());

    sideToMove = ~sideToMove;

    Square from = m.from_sq();
    Square to   = m.to_sq();

    assert(empty(from));
    assert(type_of(st->capturedPiece) != KING);

    move_piece(to, from);  // Put the piece back at the source square

    if (st->capturedPiece)
    {
        Square capsq = to;

        put_piece(st->capturedPiece, capsq);  // Restore the captured piece
    }

    // Finally point our state pointer back to the previous state
    st = st->previous;
    --gamePly;

    // SkyRule: pop skyStack
    if (skyStackPly > 0)
        --skyStackPly;

    // Update the bloom filter
    --filter[st->key];

    assert(pos_is_ok());
}

inline void add_dirty_threat(DirtyThreats* const dts,
                             bool                PutPiece,
                             Piece               pc,
                             Piece               threatened,
                             Square              s,
                             Square              threatenedSq) {
    dts->list.push_back({pc, threatened, s, threatenedSq, PutPiece});
}


template<bool ComputeRay>
void Position::update_piece_threats(Piece pc, bool putPiece, Square s, DirtyThreats* const dts) {
    Bitboard occupied = pieces();

    const Bitboard rAttacks = attacks_bb<ROOK>(s, occupied);
    const Bitboard cAttacks = attacks_bb<CANNON>(s, occupied);

    // Outgoing threats
    Bitboard threatened;

    switch (type_of(pc))
    {
    case PAWN :
        threatened = attacks_bb<PAWN>(s, color_of(pc));
        break;
    case ROOK :
        threatened = rAttacks;
        break;
    case CANNON :
        threatened = cAttacks;
        break;

    default :
        threatened = attacks_bb(type_of(pc), s, occupied);
    }

    threatened &= occupied;

    while (threatened)
    {
        Square threatenedSq = pop_lsb(threatened);
        Piece  threatenedPc = piece_on(threatenedSq);

        assert(threatenedSq != s);
        assert(threatenedPc);

        add_dirty_threat(dts, putPiece, pc, threatenedPc, s, threatenedSq);
    }

    // Incoming threats
    Bitboard incoming_threats = (attacks_bb<PAWN_TO>(s, WHITE) & pieces(WHITE, PAWN))
                              | (attacks_bb<PAWN_TO>(s, BLACK) & pieces(BLACK, PAWN))
                              | (attacks_bb<KNIGHT_TO>(s, occupied) & pieces(KNIGHT))
                              | (attacks_bb<BISHOP>(s, occupied) & pieces(BISHOP))
                              | (attacks_bb<ADVISOR>(s) & pieces(ADVISOR))
                              | (attacks_bb<KING>(s) & pieces(KING));

    // Discovered threats
    if constexpr (ComputeRay)
    {
        // Rooks threat pieces on the other side
        Bitboard sliders = rAttacks & pieces(ROOK);
        while (sliders)
        {
            Square sliderSq = pop_lsb(sliders);
            Piece  slider   = piece_on(sliderSq);

            const Bitboard discovered = ray_pass_bb(sliderSq, s) & rAttacks & occupied;

            assert(!more_than_one(discovered));
            if (discovered)
            {
                const Square threatenedSq = lsb(discovered);
                const Piece  threatenedPc = piece_on(threatenedSq);
                add_dirty_threat(dts, !putPiece, slider, threatenedPc, sliderSq, threatenedSq);
            }

            add_dirty_threat(dts, putPiece, slider, pc, sliderSq, s);
        }
        // Cannons threat pieces on the other side
        sliders = cAttacks & pieces(CANNON);
        while (sliders)
        {
            Square sliderSq = pop_lsb(sliders);
            Piece  slider   = piece_on(sliderSq);

            // Jumping over the first piece before 's'
            const Bitboard discovered = ray_pass_bb(sliderSq, s) & rAttacks & occupied;

            assert(!more_than_one(discovered));
            if (discovered)
            {
                const Square threatenedSq = lsb(discovered);
                const Piece  threatenedPc = piece_on(threatenedSq);
                add_dirty_threat(dts, !putPiece, slider, threatenedPc, sliderSq, threatenedSq);
            }

            add_dirty_threat(dts, putPiece, slider, pc, sliderSq, s);
        }
        sliders = rAttacks & pieces(CANNON);
        while (sliders)
        {
            Square sliderSq = pop_lsb(sliders);
            Piece  slider   = piece_on(sliderSq);

            // Jumping over 's'
            Bitboard discovered = ray_pass_bb(sliderSq, s) & rAttacks & occupied;

            assert(!more_than_one(discovered));
            if (discovered)
            {
                const Square threatenedSq = lsb(discovered);
                const Piece  threatenedPc = piece_on(threatenedSq);
                add_dirty_threat(dts, putPiece, slider, threatenedPc, sliderSq, threatenedSq);
            }

            // Jumping over the first piece after 's'
            discovered = ray_pass_bb(sliderSq, s) & cAttacks & occupied;

            assert(!more_than_one(discovered));
            if (discovered)
            {
                const Square threatenedSq = lsb(discovered);
                const Piece  threatenedPc = piece_on(threatenedSq);
                add_dirty_threat(dts, !putPiece, slider, threatenedPc, sliderSq, threatenedSq);
            }
        }

        // Knights with 's' in between threat pieces on the other side
        // Bishops with 's' in between threat pieces on the other side
        Bitboard leapers = (unconstrained_attacks_bb<KING>(s) & pieces(KNIGHT))
                         | (unconstrained_attacks_bb<ADVISOR>(s) & pieces(BISHOP));
        while (leapers)
        {
            Square leaperSq = pop_lsb(leapers);
            Piece  leaper   = piece_on(leaperSq);

            Bitboard discovered = leaper_pass_bb(leaperSq, s) & occupied;

            assert(type_of(leaper) == KNIGHT ? popcount(discovered) <= 2
                                             : !more_than_one(discovered));
            while (discovered)
            {
                const Square threatenedSq = pop_lsb(discovered);
                const Piece  threatenedPc = piece_on(threatenedSq);
                add_dirty_threat(dts, !putPiece, leaper, threatenedPc, leaperSq, threatenedSq);
            }
        }
    }
    else
        incoming_threats |= (rAttacks & pieces(ROOK)) | (cAttacks & pieces(CANNON));

    while (incoming_threats)
    {
        Square srcSq = pop_lsb(incoming_threats);
        Piece  srcPc = piece_on(srcSq);

        assert(srcSq != s);
        assert(srcPc != NO_PIECE);

        add_dirty_threat(dts, putPiece, srcPc, pc, srcSq, s);
    }
}

Key Position::prefetch_key(Move m) const {
    Square from     = m.from_sq();
    Square to       = m.to_sq();
    Piece  pc       = piece_on(from);
    Piece  captured = piece_on(to);
    Key    k        = st->key ^ Zobrist::side;

    k ^= Zobrist::psq[captured][to] ^ Zobrist::psq[pc][to] ^ Zobrist::psq[pc][from];

    if (captured)
        return k;

    return adjust_key60<true>(k);
}


// Used to do a "null move": it flips
// the side to move without executing any move on the board.
void Position::do_null_move(StateInfo& newSt) {

    assert(!checkers());
    assert(&newSt != st);

    // Update the bloom filter
    ++filter[st->key];

    std::memcpy(&newSt, st, sizeof(StateInfo));

    newSt.previous = st;
    st             = &newSt;

    st->key ^= Zobrist::side;

    st->pliesFromNull = 0;

    st->capturedPiece = NO_PIECE;

    sideToMove = ~sideToMove;

    set_check_info();

    assert(pos_is_ok());
}


// Must be used to undo a "null move"
void Position::undo_null_move() {

    assert(!checkers());

    st         = st->previous;
    sideToMove = ~sideToMove;

    // Update the bloom filter
    --filter[st->key];
}


// Tests if the SEE (Static Exchange Evaluation)
// value of the move is greater or equal to the given threshold. We'll use an
// algorithm similar to alpha-beta pruning with a null window.
bool Position::see_ge(Move m, int threshold) const {

    assert(m.is_ok());

    Square from = m.from_sq(), to = m.to_sq();

    assert(piece_on(from) != NO_PIECE);

    int swap = PieceValue[piece_on(to)] - threshold;
    if (swap < 0)
        return false;

    swap = PieceValue[piece_on(from)] - swap;
    if (swap <= 0)
        return true;

    assert(color_of(piece_on(from)) == sideToMove);
    Bitboard occupied  = pieces() ^ from ^ to;  // xoring to is important for pinned piece logic
    Color    stm       = sideToMove;
    Bitboard attackers = attackers_to(to, occupied);

    // Flying general
    bool kingAttacks = attackers & pieces(KING);
    if (kingAttacks)
        attackers |= attacks_bb<ROOK>(to, occupied) & pieces(KING);

    Bitboard nonCannons = attackers & ~pieces(CANNON);
    Bitboard cannons    = attackers & pieces(CANNON);
    Bitboard stmAttackers, bb;
    int      res = 1;

    while (true)
    {
        stm = ~stm;
        attackers &= occupied;

        // If stm has no more attackers then give up: stm loses
        if (!(stmAttackers = attackers & pieces(stm)))
            break;

        // Don't allow pinned pieces to attack as long as there are
        // pinners on their original square.
        if (pinners(~stm) & occupied)
        {
            stmAttackers &= ~blockers_for_king(stm);

            if (!stmAttackers)
                break;
        }

        res ^= 1;

        // Locate and remove the next least valuable attacker, and add to the
        // bitboard 'attackers' any protential attackers when it is removed.
        if ((bb = stmAttackers & pieces(PAWN)))
        {
            if ((swap = PawnValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);

            nonCannons |=
              attacks_bb<ROOK>(to, occupied) & (kingAttacks ? pieces(KING, ROOK) : pieces(ROOK));
            cannons   = attacks_bb<CANNON>(to, occupied) & pieces(CANNON);
            attackers = nonCannons | cannons;
        }

        else if ((bb = stmAttackers & pieces(BISHOP)))
        {
            if ((swap = BishopValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);
        }

        else if ((bb = stmAttackers & pieces(ADVISOR)))
        {
            if ((swap = AdvisorValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);

            nonCannons |= attacks_bb<KNIGHT_TO>(to, occupied) & pieces(KNIGHT);
            attackers = nonCannons | cannons;
        }

        else if ((bb = stmAttackers & pieces(CANNON)))
        {
            if ((swap = CannonValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);

            cannons   = attacks_bb<CANNON>(to, occupied) & pieces(CANNON);
            attackers = nonCannons | cannons;
        }

        else if ((bb = stmAttackers & pieces(KNIGHT)))
        {
            if ((swap = KnightValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);
        }

        else if ((bb = stmAttackers & pieces(ROOK)))
        {
            swap = RookValue - swap;
            occupied ^= least_significant_square_bb(bb);

            nonCannons |=
              attacks_bb<ROOK>(to, occupied) & (kingAttacks ? pieces(KING, ROOK) : pieces(ROOK));
            cannons   = attacks_bb<CANNON>(to, occupied) & pieces(CANNON);
            attackers = nonCannons | cannons;
        }

        else  // KING
              // If we "capture" with the king but the opponent still has attackers,
              // reverse the result.
            return (attackers & ~pieces(stm)) ? res ^ 1 : res;
    }

    return bool(res);
}


// A lighter version of do_move(), used in chasing detection
std::pair<Piece, int> Position::do_move(Move m) {

    assert(capture(m));

    Square from     = m.from_sq();
    Square to       = m.to_sq();
    Piece  captured = piece_on(to);
    int    id       = idBoard[to];

    // Update id board
    idBoard[to]   = idBoard[from];
    idBoard[from] = 0;

    // Update board and piece lists
    remove_piece(to);
    move_piece(from, to);

    sideToMove = ~sideToMove;

    return {captured, id};
}


// A lighter version of undo_move(), used in chasing detection
void Position::undo_move(Move m, Piece captured, int id) {

    sideToMove = ~sideToMove;

    Square from = m.from_sq();
    Square to   = m.to_sq();

    // Put back id board
    idBoard[from] = idBoard[to];
    idBoard[to]   = id;

    move_piece(to, from);  // Put the piece back at the source square

    if (captured)
        put_piece(captured, to);  // Restore the captured piece
}


// Tests whether a pseudo-legal move is chase legal
bool Position::rule_judge(Value& result, int ply) {

    // Asian competition rules use 100 plies without a capture or pawn move;
    // SkyRule keeps its configurable threshold.
    const int naturalLimit = currentRule == ASIAN_RULE ? 100 : rule60MaxPly;

    // SkyRule: 用全局变量检测连将, 不遍历搜索sp链
    // 不限制ply==0: 根节点不截断后搜索会展开, 内部节点也需要判连将
    if (currentRule == SKY_RULE)
    {
        if (skyMoveCheckW >= 6 && skyMoveCheckW <= 12)
        {
            result = (WHITE == sideToMove) ? Value(-24999) : Value(24999);
            return true;
        }
        if (skyMoveCheckB >= 6 && skyMoveCheckB <= 12)
        {
            result = (BLACK == sideToMove) ? Value(-24999) : Value(24999);
            return true;
        }
    }

    // Restore rule 60 by adding back the checks
    // SkyRule: 禁用null move后pliesFromNull仍可能被搜索框架重置, 直接用rule60确保完整循环检测
    int end = (currentRule == SKY_RULE)
                ? st->rule60 + std::max(0, st->check10[WHITE] - 10) + std::max(0, st->check10[BLACK] - 10)
                : std::min(st->rule60 + std::max(0, st->check10[WHITE] - 10)
                             + std::max(0, st->check10[BLACK] - 10),
                           st->pliesFromNull);

    if (end >= 4 && (currentRule == SKY_RULE || filter[st->key] >= 1))
    {
        int        cnt       = 0;
        StateInfo* stp       = st->previous->previous;
        bool       checkThem = st->checkersBB && stp->checkersBB;
        bool       checkUs   = st->previous->checkersBB && stp->previous->checkersBB;

        for (int i = 4; i <= end; i += 2)
        {
            stp = stp->previous->previous;
            checkThem &= bool(stp->checkersBB);

            // Return a score if a position repeats once earlier but strictly
            // after the root, or repeats twice before or at the root.
            if (stp->key == st->key && (++cnt == 2 || ply > i || currentRule == SKY_RULE))
            {
                if (currentRule == SKY_RULE || currentRule == ASIAN_RULE)
                {
                    result = sky_judge_loop(i, ply);
                    if (result == VALUE_DRAW && (checkThem || checkUs))
                        result = !checkUs ? mate_in(ply) : !checkThem ? mated_in(ply) : VALUE_DRAW;
                }
                else if (!checkThem && !checkUs)
                {
                    Position rollback;
                    memcpy((void*) &rollback, (const void*) this, offsetof(Position, filter));
                    skySplitAllowed = false;
                    result = rollback.detect_chases(i, ply);
                }
                else
                {
                    // Checking detection
                    result = !checkUs ? mate_in(ply) : !checkThem ? mated_in(ply) : VALUE_DRAW;
                }

                // SkyRule: 只有判和(VALUE_DRAW)或判负(±24999)才返回true截断搜索
                // VALUE_NONE(未达阈值)继续搜索找更长循环
                if (currentRule == SKY_RULE || currentRule == ASIAN_RULE)
                {
                    if (result == VALUE_DRAW || result == Value(24999) || result == Value(-24999))
                        return true;
                    // 分捉多子允许招法: 不继续找更长循环, 防止更长循环误判
                    if (currentRule == SKY_RULE && skySplitAllowed)
                    {
                        result = VALUE_NONE;
                        return true;
                    }
                }
                else if (result == VALUE_DRAW || cnt == 2)
                    return true;

                // 2 fold mates need further investigations
                // SkyRule: 未达阈值(VALUE_NONE)或判和(VALUE_DRAW)时继续找更长循环, 判负(±24999)时不继续
                if (filter[st->key] <= 1 && !(currentRule == SKY_RULE && (result == Value(24999) || result == Value(-24999))))
                {
                    // Not exceeding rule 60 and have the same previous step
                    if (st->rule60 < naturalLimit && st->previous->key == stp->previous->key)
                    {
                        // Even if we entering this loop again, it will not lead to a 3 fold repetition
                        StateInfo* prev = st->previous;
                        while ((prev = prev->previous) != stp)
                            if (filter[prev->key] > 1)
                                break;
                        if (prev == stp)
                            return true;
                    }
                    // We know there can't be another fold
                    break;
                }
            }

            if (i + 1 <= end)
                checkUs &= bool(stp->previous->checkersBB);
        }
    }

    // 60 move rule (120 plies without capture)
    // Asian competition rules use 100 plies (50 moves) for the natural
    // move limit. Keep the configurable SkyRule limit for SkyRule itself.
    if (st->rule60 >= naturalLimit)
    {
        result = MoveList<LEGAL>(*this).size() ? VALUE_DRAW : mated_in(ply);
        return true;
    }

    // SkyRule: 总步数达到400步(200回合)自动判和
    if (currentRule == SKY_RULE && gamePly >= 400)
    {
        result = MoveList<LEGAL>(*this).size() ? VALUE_DRAW : mated_in(ply);
        return true;
    }

    // Draw by insufficient material
    if (count<PAWN>() == 0)
    {
        enum DrawLevel : int {
            NO_DRAW,      // There is no drawing situation exists
            DIRECT_DRAW,  // A draw can be directly yielded without any checks
            MATE_DRAW     // We need to check for mate before yielding a draw
        };

        int level = [&]() {
            // No cannons left on the board
            if (!major_material())
                return DIRECT_DRAW;

            // One cannon left on the board
            if (major_material() == CannonValue)
            {
                // See which side is holding this cannon, and this side must not possess any advisors
                Color cannonSide = major_material(WHITE) == CannonValue ? WHITE : BLACK;
                if (count<ADVISOR>(cannonSide) == 0)
                {
                    // No advisors left on the board
                    if (count<ADVISOR>(~cannonSide) == 0)
                        return DIRECT_DRAW;

                    // One advisor left on the board
                    if (count<ADVISOR>(~cannonSide) == 1)
                        return count<BISHOP>(cannonSide) == 0 ? DIRECT_DRAW : MATE_DRAW;

                    // Two advisors left on the board
                    if (count<BISHOP>(cannonSide) == 0)
                        return MATE_DRAW;
                }
            }

            // Two cannons left on the board, one for each side, and no advisors left on the board
            if (major_material(WHITE) == CannonValue && major_material(BLACK) == CannonValue
                && count<ADVISOR>() == 0)
                return count<BISHOP>() == 0 ? DIRECT_DRAW : MATE_DRAW;

            return NO_DRAW;
        }();

        if (level != NO_DRAW)
        {
            if (level == MATE_DRAW)
            {
                MoveList<LEGAL> moves(*this);
                if (moves.size() == 0)
                {
                    result = mated_in(ply);
                    return true;
                }
                for (const auto& move : moves)
                {
                    StateInfo tempSt;
                    do_move(move, tempSt);
                    bool mate = MoveList<LEGAL>(*this).size() == 0;
                    undo_move(move);
                    if (mate)
                        return false;
                }
            }
            result = VALUE_DRAW;
            return true;
        }
    }

    return false;
}


// Flips position with the white and black sides reversed. This
// is only useful for debugging e.g. for finding evaluation symmetry bugs.
std::optional<PositionSetError> Position::flip() {

    string            f, token;
    std::stringstream ss(fen());

    for (Rank r = RANK_9;; --r)  // Piece placement
    {
        std::getline(ss, token, r > RANK_0 ? '/' : ' ');
        f.insert(0, token + (f.empty() ? " " : "/"));

        if (r == RANK_0)
            break;
    }

    ss >> token;                        // Active color
    f += (token == "w" ? "B " : "W ");  // Will be lowercased later

    ss >> token;
    f += token + " ";

    std::transform(f.begin(), f.end(), f.begin(),
                   [](unsigned char c) { return char(islower(c) ? toupper(c) : tolower(c)); });

    ss >> token;
    f += token;

    std::getline(ss, token);  // Half and full moves
    f += token;

    return set(f, st);
}


// Performs some consistency checks for the position object
// and raise an assert if something wrong is detected.
// This is meant to be helpful when debugging.
bool Position::pos_is_ok() const {

    if ((sideToMove != WHITE && sideToMove != BLACK) || piece_on(king_square(WHITE)) != W_KING
        || piece_on(king_square(BLACK)) != B_KING)
        assert(0 && "pos_is_ok: Default");

    if (count<KING>(WHITE) != 1 || count<KING>(BLACK) != 1
        || checkers_to(sideToMove, king_square(~sideToMove)))
        assert(0 && "pos_is_ok: Kings");

    if ((pieces(WHITE, PAWN) & ~PawnBB[WHITE]) || (pieces(BLACK, PAWN) & ~PawnBB[BLACK])
        || count<PAWN>(WHITE) > 5 || count<PAWN>(BLACK) > 5)
        assert(0 && "pos_is_ok: Pawns");

    if ((pieces(WHITE) & pieces(BLACK)) || (pieces(WHITE) | pieces(BLACK)) != pieces()
        || popcount(pieces(WHITE)) > 16 || popcount(pieces(BLACK)) > 16)
        assert(0 && "pos_is_ok: Bitboards");

    for (PieceType p1 = PAWN; p1 <= KING; ++p1)
        for (PieceType p2 = PAWN; p2 <= KING; ++p2)
            if (p1 != p2 && (pieces(p1) & pieces(p2)))
                assert(0 && "pos_is_ok: Bitboards");

    for (Piece pc : Pieces)
        if (pieceCount[pc] != popcount(pieces(color_of(pc), type_of(pc)))
            || pieceCount[pc] != std::count(board.begin(), board.end(), pc))
            assert(0 && "pos_is_ok: Pieces");

    return true;
}

}  // namespace Stockfish
