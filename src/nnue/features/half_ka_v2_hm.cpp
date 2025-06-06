/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2025 The Stockfish developers (see AUTHORS file)

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

// Definition of input features HalfKAv2_hm of NNUE evaluation function

#include "half_ka_v2_hm.h"

#include "../../position.h"
#include "../../types.h"
#include "../nnue_accumulator.h"

namespace Stockfish::Eval::NNUE::Features {

bool HalfKAv2_hm::requires_mid_mirror(const Position& pos, Color c) {
    return ((1ULL << 63) & pos.mid_encoding(c) & pos.mid_encoding(~c))
        && (pos.mid_encoding(c) < BalanceEncoding
            || (pos.mid_encoding(c) == BalanceEncoding && pos.mid_encoding(~c) < BalanceEncoding));
}

// Get attack bucket
IndexType HalfKAv2_hm::make_attack_bucket(const Position& pos, Color c) {
    return AttackBucket[pos.count<ROOK>(c)][pos.count<KNIGHT>(c)][pos.count<CANNON>(c)];
}

// Get layer stack bucket
IndexType HalfKAv2_hm::make_layer_stack_bucket(const Position& pos) {
    Color us = pos.side_to_move();
    return LayerStackBuckets[pos.count<ROOK>(us)][pos.count<ROOK>(~us)]
                            [pos.count<KNIGHT>(us) + pos.count<CANNON>(us)]
                            [pos.count<KNIGHT>(~us) + pos.count<CANNON>(~us)];
}

// Index of a feature for a given king position and another piece on some square
template<Color Perspective>
inline IndexType HalfKAv2_hm::make_index(Square s, Piece pc, int bucket, bool mirror) {
    return IndexType(
      IndexMap[mirror][Perspective == BLACK][type_of(pc) == ADVISOR || type_of(pc) == BISHOP][s]
      + PieceSquareIndex[Perspective][pc] + PS_NB * bucket);
}

// Explicit template instantiations
template IndexType HalfKAv2_hm::make_index<WHITE>(Square s, Piece pc, int bucket, bool mirror);
template IndexType HalfKAv2_hm::make_index<BLACK>(Square s, Piece pc, int bucket, bool mirror);

// Get a list of indices for recently changed features
template<Color Perspective>
void HalfKAv2_hm::append_changed_indices(
  int bucket, bool mirror, const DirtyPiece& dp, IndexList& removed, IndexList& added) {
    removed.push_back(make_index<Perspective>(dp.from, dp.pc, bucket, mirror));

    if (dp.to != SQ_NONE)
        added.push_back(make_index<Perspective>(dp.to, dp.pc, bucket, mirror));

    if (dp.remove_sq != SQ_NONE)
        removed.push_back(make_index<Perspective>(dp.remove_sq, dp.remove_pc, bucket, mirror));
}

// Explicit template instantiations
template void HalfKAv2_hm::append_changed_indices<WHITE>(
  int bucket, bool mirror, const DirtyPiece& dp, IndexList& removed, IndexList& added);
template void HalfKAv2_hm::append_changed_indices<BLACK>(
  int bucket, bool mirror, const DirtyPiece& dp, IndexList& removed, IndexList& added);

bool HalfKAv2_hm::requires_refresh(const DirtyPiece& dirtyPiece, Color perspective) {
    return dirtyPiece.requires_refresh[perspective];
}

}  // namespace Stockfish::Eval::NNUE::Features
